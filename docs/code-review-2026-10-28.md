# 代码审查 — 2026-10-28

范围：`Source/` 全部 78 个源文件（63 813 行）。重点放在最近三个阶段改动的
（P63 剪贴板、P67 标尺拖拽、P68/P69 网格与缩放）、音频线程、工程文件读写。

方法：三路并行粗读 → 逐条读源码核实 → 对能脱离 GUI 验证的用临时断言实测
（跑完删除，工作区未留改动）。**只列能指到行号、并说明触发路径的**；读下来不成立的
写在最后一节，附排除理由。

自检基线：586 ALL PASSED / 0 FAIL。本次审查**未修改任何产品代码**。

---

## 实测证据

在 `SelfTest.cpp` 里加过 5 条临时断言，跑完即删（`git diff` 为空）。输出：

```
[FAIL] [探针1] 图层当前自动化轨往返后仍是 volume   (pan)
[FAIL] [探针2] 1/64 拍的采样块往返后仍是 1/64 拍   (0.25000 拍)
[PASS] [探针0] setup：上一个会话是 16 通道          (16)
[PASS] [探针3] 没有 <Mixer> 的工程加载后通道数回到默认 (8 条)
[FAIL] [探针4] 新工程的通道 0 回到主输出            (1)
3 CHECK(S) FAILED
```

探针 1/2/4 对应下面的 P0-1 / P0-2 / P0-3，三条都复现。

---

## P0 — 会静默改坏数据

### P0-1 图层的“当前自动化轨”不落盘，重开永远是 pan

- 写：`Source/Model/ProjectIO.cpp:515-518`
  ```cpp
  for (auto target : { LayerAutomationTarget::volume, LayerAutomationTarget::pan })
      writeLane (*xml, layer->automationFor (target), layerTargetToText (target),
                 target == layer->getAutomationTarget() && layer->isAutomationExpanded());
  ```
  “当前是哪条”只编码在 `expanded` 属性里，**没有 `autotarget` 属性**。
- 读：`Source/Model/ProjectIO.cpp:894-908`
  ```cpp
  if (targetText.isNotEmpty())
      layer->setAutomationTarget (target);      // 每读一条就覆盖一次
  ```
  两条 `<Automation>` 必然都带 `target`，于是循环里**最后一个（pan）永远获胜**。

- 实测：设 `volume` → 存 → 读，回来是 `pan`。
- 影响：重开工程后左侧面板和自动化窗口指向错误的轨；用户不看标签直接拖点，改的是另一条曲线。
- 对照：slot 那边已经用 `autotarget` 属性修过（`ProjectIO.cpp:391-393`，注释明写
  “元素顺序不是规则，是意外”），图层这边漏了。
- 修法：`<Layer>` 加 `autotarget`，读时优先读它；旧文件回退到“带 `expanded="1"` 的那条”，
  而不是“最后一条”。

### P0-2 采样块长度下限写死 0.25 拍，与 P69 的 1/64 冲突

- `Source/Model/LayerModel.cpp:921`（`addSample`）与 `:942`（`restoreSamples`）：
  ```cpp
  s.lengthBeats = juce::jmax (0.25, s.lengthBeats);
  ```
- 而 P69 把拖拽下限改成 `minimumNoteLength()`（`PianoRollComponent.cpp:8176` 起），
  放大到上限可以是 0.015625。
- 实测：建一个 1/64 拍的块 → 存 → 读，回来是 0.25 拍（**宽 16 倍**）。
- 影响面不止重开：`addSample` 是所有“新增块”的入口，所以粘贴、剪切粘贴、
  `syncGroupShape` 的“补成员”分支都会把 1/64 的块撑成 1/16 音符，出现
  “源块 1/64、副本 0.25”的形状打架。
- 修法：模型侧下限改成 `jmax (0.0, …)`（`restoreNotes` 就是 `jmax(0.0)`），
  下限交给编辑器；若要保留模型兜底，取一个与 P69 一致的最小常量而不是 0.25。

### P0-3 加载工程只设“非主输出”的路由，旧路由残留

- `Source/Model/ProjectIO.cpp:1222-1224`：
  ```cpp
  for (int channel = 0; channel < MixerModel::numChannels; ++channel)
      if (parsedOutputs[(size_t) channel] != MixerModel::masterChannel)
          mixer->setChannelOutput (channel, parsedOutputs[(size_t) channel]);
  ```
  新工程里 `ch0 → 主输出` 时**一条都不调**，`channelOutputs[0]` 保留上一个会话的值。
- 实测：先造 `0 → 1` 的会话，再加载默认路由的工程，读回来仍是 `1`。
- 影响：跨工程打开会带上一个看不见的旧路由；更糟的是
  `commit` 后的 `wouldLoop` 会沿残留路由判环，把文件里正确的 `1 → 0` 也拒掉，
  两条拓扑同时错，再保存就把错的写进新工程。
- 修法：无条件 `setChannelOutput(channel, parsedOutputs[channel])`（含 master），
  或先把整张 `channelOutputs` 重置成 master 再逐条应用。
  注意紧邻的 mono 循环（`:1229`）就是“两个方向都设”，同一规则没用到路由上。

---

## P1 — 功能失效或实时安全

### P1-1 `stopSamplePreview()` 停不下来

- `Source/Audio/PlaybackEngine.cpp:2851-2857` 只写 `previewEngaged.store(false)`，
  注释说 “The voice notices on its next block.”；
  但渲染循环 `:2931-2948` 只判
  `! previewVoice.active || position >= frames`，**从不读 `previewEngaged`**。
- 触发：试听一个采样，中途点停止（或 `previewSample(nullptr)`）。
- 影响：已经开始的预览会播满请求窗口（最长 60 s）或整个文件，`hasSamplePreview()`
  也一直为真。
- 修法：循环里判 `engaged`，为 false 时把 target 置 0，走完 4 ms 平滑后 `active = false`。

### P1-2 落在块边界的 note-off 会丢

- `Source/Audio/PlaybackEngine.cpp:1728`：
  `if (offOffset <= (double) chunkStart || …) continue;`
- `:1765`：`if (off >= 0 && off < chunkLen) addEvent(noteOff, off);`
- 设 `offOffset == 块尾`（或距块尾不足半采样，`llround` 后 `off == chunkLen`）：
  本块因 `off < chunkLen` 不发；下一块因 `offOffset <= chunkStart` 提前 `continue`。
  **两头都不发。**
- 影响：宿主 VST3 的持续音收不到 note-off，一直响到 stop / loop 的
  `allNotesOffHosted()`，表现为长鸣或复音数泄漏。
- 修法：跳过条件改 `offOffset < chunkStart`，发送时把 `off` 夹到 `[0, chunkLen-1]`。

### P1-3 音频线程每个回调都分配内存

- `Source/Audio/PlaybackEngine.cpp:2036` `RenderStats stats;`（在 `renderNextBlock` 内），
  构造函数 `:135-140` 无条件 `reserve` 三次（slot 16 / layer 16 / channel 127）。
- 影响：每次设备回调 3 次 malloc + 3 次 free；超出预留容量时 `addPeak` 的 `emplace_back`
  还会再分配。违反“回调内不分配”的约定，内存压力下会 dropout。
- 修法：统计对象做成员，`prepareToPlay` 里一次性 reserve，块首只 `clear()`；
  或换成定长数组。

### P1-4 `getSchedule()` 可能让音频线程释放整个 schedule

- `Source/Audio/PlaybackEngine.cpp:899-903`：
  ```cpp
  const juce::SpinLock::ScopedLockType guard (scheduleLock);
  return schedule;                 // shared_ptr 拷贝
  ```
- 音频线程调用点：`:2016 :2180 :2198 :2497 :2617 :2915 :2991 :3005 :3158 :3174`。
  消息线程 `publishSchedule` 换掉成员后（`:887-889`），音频线程手里的 `current`
  就是最后一个引用；函数返回时在**音频线程**析构 `PublishedSchedule`
  （几十条 note/clip，以及每个 insert 的 `shared_ptr<PluginDescription>`）。
- 影响：实时线程上出现不确定规模的 `operator delete`；另外每块还要自旋抢锁。
- 修法：`std::atomic<const PublishedSchedule*>` 发布 + 消息线程侧的 retire 列表
  （保留最近 2 个，音频线程只做一次原子 load）。

### P1-5 预览换源时在音频线程释放旧 buffer

- `Source/Audio/PlaybackEngine.cpp:2878` `previewVoice.buffer = previewPending;`
- 触发：连续试听 A、B 两个采样；消息线程换 `previewPending` 时已放掉 A 的引用，
  音频线程这次赋值释放的就是最后一个引用。
- 影响：在回调里 free 掉整个样本 `frames`（可达数十 MB），与头文件 `:694-696`
  明写的“八兆不能在音频线程上放”正好相反。
- 修法：把被替换的旧 `shared_ptr` 移到 `previewRetired`，由消息线程释放。

---

## P2 — 交互瑕疵（不影响数据）

| # | 位置 | 缺陷 | 影响 |
|---|---|---|---|
| 1 | `MixerPanel.cpp:174-178` | 删通道后 `selectedChannel` 只做 `jlimit`，不随行下移；注释写的是 “follows the row” | 删掉选中通道下方的条，选中会静默跳到另一条通道，右侧机架/插入编辑打到用户没看的通道 |
| 2 | `MixerPanel.cpp:1040-1054` + `:574-581` | 主输出条无条件画 M/S，但 `toggleSolo` 对 master 直接 `return` | S 永远是死键、永不亮；右键菜单还生成一条空操作 |
| 3 | `AutomationWindow.cpp:70` | `target == 1 ? "主声相" : "主音量"`，而 `numMasterParameters = 3`（0 音量 / 1 声相 / 2 静音） | 列表里出现两行“主音量”，静音曲线容易被当成音量曲线编辑 |
| 4 | `LayerPanel.cpp:210-227` | `areAllLayersVisible()` 跳过活动层，单层工程恒为 true | 标题栏眼睛点不动（`showAll` 与写入值相同，图标也不变） |
| 5 | `LayerModel.cpp:1037-1038` | `removeLayer` 删 `index < activeIndex` 时不左移 `activeIndex` | 活动层从 L2 悄悄变成 L3；目前无 UI 入口，但 API/自检路径可触发 |
| 6 | `MixerPanel.cpp:1395` vs `:301/:355` | 推子命中区 `expanded(8,0)` 越过 4 px 间隙，压进电平表 4 px | 点电平表左柱会把电平跳到该处；hover 也给错光标 |
| 7 | `PianoRollComponent.cpp:8011/8064` | 粘贴“落在原地”时推进量是 `jmax(gridDivision, 长度)` = 0.25 拍（P69 之后本可以是 1/64） | 在 1/64 网格上复制一个 1/64 的对象，粘贴会跳到 16 格之外 |
| 8 | `MixerPanel.h:223` + `.cpp:709` | `mixerChanged()` 只 `repaint()`；`setSelectedChannel` 夹到 `[0, 127]` 而不是 `channelCount` | 从大工程切到 8 通道工程，选中可能指向不存在的通道，机架标题写“通道 22” |

---

## 读下来不成立的（误报，附理由）

1. **“`<Mixer>` 缺失时整个混音台都不提交”** —— 不成立。
   `ProjectIO.cpp:1099` 的 `if (mixXml)` 只包住“解析”部分；提交在另一处独立的
   `if (mixer != nullptr)`（`:1198`），两者不是嵌套。实测探针 3：16 通道的会话
   加载无 `<Mixer>` 的工程后回到 8 通道。
2. **“`syncGroupShape` 靠序号配对，数量不等时会错位覆盖”** —— 不成立（是设计）。
   两侧都按 `(startBeat, pitch, id)` 排序，数量不等时尾部多退少补；删成员会走
   同一条同步路径，正常流程下数量不会不等。`PITFALLS.md` 里也记了这条语义。

---

## 建议修复顺序

1. **P0-1 / P0-2 / P0-3**：都是“存盘一圈后悄悄变形”，且都有现成断言可以钉住
   （把本文的三条探针改成正式用例即可）。先修这三条，改完自检应从 586 涨到 589。
2. **P1-2 / P1-1**：用户直接听得出来（长鸣、停不下来），改动局部。
3. **P1-3 / P1-4 / P1-5**：同属实时安全，建议一次做完，单独一个阶段，
   因为没有症状可观察，只能靠约定和代码审查守。
4. **P2**：可以随手带上，也可以等排期。
