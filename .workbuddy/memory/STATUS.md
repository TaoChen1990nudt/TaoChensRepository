# AtomicDAW — 当前状态索引

> 最后更新：2026-10-27 · 对应 **Phase 47**（效果器多参数化 + 编辑窗口按键）
> 本文件只回答三件事：**现在在哪、下一步、怎么验**。
> 规则查 `MEMORY.md`，制度细节查 `REFERENCE.md`，踩过的坑查 `PITFALLS.md`。

## 新会话读文档的顺序

1. **本文件** — 当前阶段、自检数、构建命令（1 分钟）
2. **`MEMORY.md`** — 六条跨主题硬规则（2.5 KB，必须全文读）
3. **`REFERENCE.md`** — 按主题查具体制度（构建/路由/音频/工程文件/UI/分栏/混音台/编辑手势/编组/采样/走带/文件树/波形/伸缩…）
4. **`PITFALLS.md`** — 按阶段查坑（P26–P35 归档最全）
5. **`memory/<日期>.md`** — 某阶段的完整实现记录（为什么这么写、断言清单）

不要从日志开始读。日志是流水账，527 行只讲一段。

## 当前状态

| 项 | 值 |
|---|---|
| 已完成阶段 | **Phase 47**（EQ 三段九参 / COMP 六参 / REVERB 五参 / 编辑窗口标题与按键转发）；P44–46 见日志 |
| 自检 | **434 ALL PASSED / 0 FAIL，失败时退出码 1**（P47 咬测：EQ 旁通破坏 + 双窗转发器摘除 → 4 FAIL + EXIT=1） |
| 编译 | 零 error 零 warning |
| `formatVersion` | **20**（insert 写 `params` 逗号串全参数，读回落 p1/p2） |
| 可执行 | `build/AtomicDAW_artefacts/Release/AtomicDAW.exe` |
| 技术栈 | JUCE 8.0.4（FetchContent）+ VS2022 + MSVC 14.38 + Win SDK 10.0.22621 + C++17 |
| 源码规模 | `Source/` 下 67 个 .cpp/.h，分 Model / Audio / UI / Util / Media |

## 下一步

**未定。** 功能排期等你给。工程侧（P45 收尾后）没有未还的已知债：
- ~~const 设计债~~ 已还清（P45）：`Layer::findNote` 补 const 版；hitTestActiveLayer /
  hitTestSample / getExpandedNote / getExpandedGroup / groupLaneFor 全部双版本；
  28 个 const 查询函数成为真 const；const_cast 在 PianoRollComponent.cpp 归零。
- `runSelfTest` 搬走后 `MainComponent.cpp` 6092 行（原 13909），产品/检查不再共用一个编译单元。

## 已知边界（用户提过、还没做）

- **音符粘贴没有 streak**：P37 只修了采样。音符连按 Ctrl+V 仍落同一处（用户还没要求）。
- **VST 乐器没有双击打开界面**：只有右键菜单第 41 项，`RackPanel` 还没有 `mouseDoubleClick`。
- **组复制粘贴不带成员**：`GroupClip` 无成员列表，粘贴出来的组是空壳。
- **插件参数叠加只在单元级验证**：无头自检加载不了第三方 VST3（只有内置 `Atomic Test Synth`）。
  P41 起按键路径已经有真 VST3 断言（按下发声 / 松手当块静音 / 尾音后交还槽位）。
- **录音没接**：按键只发声，不写进钢琴窗。
- **外部控制器那条线只验到开关与端口归属**：本机没有 MIDI 输入设备（设置页显示「已打开 0 个端口」），真硬件没测过。
- **按键不走音源滤波器**：live 声音只做增益与声相，与走带里的音色略有差别。
- **MIDI 来源不跨启动记忆**：每次启动回到代码里的默认值（P40 起是「键盘开、控制器关」）。
- **插件窗口那条按键断言只验到「窗口装上了转发器」**：真键盘在真插件界面里的手感没测过。
  转发器装了两层（窗口 + content），文本框（`juce::TextEditor`）里放行，避免插件预设名输入变成弹音符。
- **音源参数菜单最多列 64 个参数**：超出的追加一条「还有 N 个参数没有列出」。
  内置 `Atomic Test Synth` 只有一个 `Level`，真插件的长参数表只被遍历过逻辑，没看过真实排布。
- **弹出菜单本身无法无头截图**：`-slotparam` 拍的是菜单造出来的那一行，不是菜单。
   P43 起断言改成**读** `juce::PopupMenu`（`fillControlMenu` + `MenuItemIterator`），不再依赖截图。
- **混音台按键断言只验到「窗口装上了转发器 + 空格交给了走带」**，真键盘在真窗口里的手感未测。
- **效果器拖动只验到模型顺序 + 音频 RMS 变了 9.6%**，鼠标拖拽的跟手感、以及拖到槽外的行为未在真机上试过。
- **独奏自动化对 live 演奏不起作用**：`renderLive*` 用的是 publish 时的静态 `audible`，只有时间线路径按拍解算。
- **主输出没有独奏**：它直接喂声卡，"独奏总线"不是一个能成立的意思，所以主输出的独奏菜单项不存在。

## 阶段 → 细节在哪

| 阶段 | 主题 | 记录位置 |
|---|---|---|
| P1–P10 | 图层钢琴窗 / 音符包络 / 音源机架 / 走带 / 混音 / 工程读写 / 滚动与效果架 / 独立混音台窗 / VST3 宿主 | `memory/2026-10-23.md` |
| P10–P13 | 空白启动 / 机架自动化进窗 / 效果器界面 / 1.3x UI 放大 | `memory/2026-10-24.md` |
| P13–P21 | 右键菜单 / 走带图标 / 编组与聚光灯 / 采样四栏 / 吸附标尺 / 双向拉伸 / 光标中线 | `memory/2026-10-25.md` |
| P22–P43 | 刚体拖动 / 127 通道 / 分栏跟手 / 文件树 / 采样窗口与淡变 / 高亮不压暗 / 伸缩三模式 / 走带落点 / 清扫不变量 / 删除闸门 / 组管采样 / 三层叠加 / 粘贴吸附 / 按键发声 / 默认最大化 / VST 松手 / 插件窗按键与音源参数自动化 / 混音台按键与文件栏抬高 / 琴键音源统一 / 效果器换位置 / 通道条四控件自动化 | `memory/2026-10-26.md` |
| P44 | 自检退出码门禁 / runSelfTest 搬家 / SharedRules.h / const_cast 设计债的发现 | `memory/2026-10-27.md` |
| P45 | const 设计债还清：findNote const / 五个双版本方法 / 30 处 const_cast 归零 | `memory/2026-10-27.md` |
| P46 | 混音链三件套（EQ/COMP/REVERB）+ AudioBuffer 不清零 + 汇总行挪到函数末尾 | `memory/2026-10-27.md` |
| P47 | 效果器多参数化：EQ 九参三段 / COMP 六参 / REVERB 五参，编辑窗口标题+按键转发，v20 | `memory/2026-10-27.md` |
| P26–P43 | 各阶段踩坑与不变量 | `PITFALLS.md` |

**最近的三个阶段**（新会话最需要的前情）：
- **P35** 组管采样：模型一个字没加，`SampleEvent::groupId` 从 P27 就在，只是没接上入口/视觉/声音
- **P36** 三层叠加收成 `AutomationStack.h`：电平相乘、声相相加后夹 ±1、插件参数相乘
- **P37** 采样粘贴：落点在网格上，连按 Ctrl+V 依次往后排，不覆盖前面粘贴的块
- **P38** 工具条按钮认采样选区（`hasAnySelection`）；VST 卡片标题跟随插件、右键可开插件界面
- **P39** 按键发声：`MidiInputHub` 管设备、`PlaybackEngine::noteOnLive/noteOffLive` 管发声、`UI/TypingPiano.h` 是 FL 键位唯一出处
- **P40** 窗口默认最大化（原生 SW_SHOWMAXIMIZED，标题栏保留）；键盘来源默认开；`MidiSources` 双开关可同时输入
- **P41** 选中 VST 音源后电脑键盘一直响：`liveHeldNotes`（pitch → slotId）只写在振荡器分支，插件分支提前 return 从不记事 → note-off 被丢。移到分叉之前，all-off 清表
- **P43** 混音台是第二个顶层窗口（P42 的坑第二批）：`MixerWindow::setKeyForwarder` 复用同一个转发器；`routing::auditionSlotId` 把"裸按键打向谁"收成一份，琴键与电脑键盘不再各说一套；`SlotAutomationTarget` 追加 pan/mute/solo 三条 lane（追加在末尾，老文件不用映射表），静音/独奏当闸读、声相当加法读；`MixerModel::moveInsert` 轮转而不是交换；通道条四个控件各自一个菜单，通道菜单不再挂在它们下面
- **P42** 四条一起：`MainComponent::handleGlobalKeyPress` + `PluginKeyForwarder` 让插件窗口在前台也有空格与音符（`TextEditor` 里放行）；`addSlotAutomationItems` / `applySlotAutomationChoice` 把「创建自动化 ▸」收成一个母菜单（卡片、通道条、编组、钢琴窗、自动化窗共用一套 id 约定）；`SlotAutomationTarget::plugin` + `SynthSlot::pluginParam` 让音源参数能建曲线并真的压插件参数；`PlaybackEngine::decayMeters(numSamples)` 改成每个回调开头无条件按 −50 dB/s 衰减，`hasMeters()` 落零即停止重绘

## 构建

PATH 里没有 cmake，用 VS 自带的：

```bash
cd "/f/PLSMStudio/Atomic DAW"

# 必须先清代理（大小写两份），否则 MSB6001 会谎报找不到编译器
unset https_proxy http_proxy all_proxy no_proxy HTTPS_PROXY HTTP_PROXY ALL_PROXY NO_PROXY

CMAKE="/c/Program Files/Microsoft Visual Studio/2022/Community/Common7/IDE/CommonExtensions/Microsoft/CMake/CMake/bin/cmake.exe"
"$CMAKE" --build build --config Release
```

首次配置才需要 `-B build -G "Visual Studio 17 2022"`。日常只构建。

## 自检

headless，不需要显示器：`--snapshot` 触发 `runSelfTest()` 并导出 PNG。

```bash
build/AtomicDAW_artefacts/Release/AtomicDAW.exe --snapshot docs/phaseNN.png > docs/phaseNN.log 2>&1
tail -3 docs/phaseNN.log   # 要看到 "ALL CHECKS PASSED"
```

断言总数只增不减。**新增功能要加断言并报出新的总数**（当前基准 421），退回去说明有东西坏了。

其他 dev 参数：`--cursor-preview <png>`（光标 8x 放大核对）、`--pluginscan`、`--audiotest`、`--transporttest`、`--load` / `--save` / `--export`。

## 未建成的东西

- `.scratch/<feature-slug>/` — `AGENTS.md` 里约定的 issue 存放处，**目录还不存在**
- `GLOSSARY.md`、`docs/adr/` — 同上，声明了但未创建（按约定 lazy 创建）
