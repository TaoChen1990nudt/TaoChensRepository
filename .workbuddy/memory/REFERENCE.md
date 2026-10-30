# AtomicDAW — 详细约定（MEMORY.md 的展开版）

按主题分节。MEMORY.md 只留六条跨主题硬规则，具体规则都在这里。

## 构建与验证
- VS2022 + MSVC 14.38 + Win SDK 10.0.22621；PATH 无 cmake，用 VS 自带
  `.../Common7/IDE/.../CMake/bin/cmake.exe`；JUCE 8.0.4 走 FetchContent。
- **构建前 `unset https_proxy http_proxy all_proxy no_proxy`**（大小写两份）：
  否则 MSB6001，报的却是 "No CMAKE_CXX_COMPILER could be found"。
- 日志别走管道 grep：`> /tmp/buildNN.log 2>&1` 再带 `-a` 过滤，并核对 exe mtime。
- **中文一律 `atomic::utf8("...")`**（CMake 加 `/utf-8`）；裸中文走 CharPointer_ASCII
  直接乱码——**自检的失败信息也算**。
- **`/STACK:67108864`**：`runSelfTest()` 是单个巨帧，优化器把 `[&]{}()` 内联回同一帧。
- 真退出码只有 `cmd /v:on /c "exe ... & echo !ERRORLEVEL!"` 拿得到（bash 给 127）；
  `0xC00000FD` = 栈溢出。出图用 PowerShell `Start-Process -Wait`（前台 Bash 会截断）。
- `--export` 与直接导出 MD5 相同；`--save` 存的是**空白会话**。
- 自检新建音符用中音区 pitch（78/80）：pitch 60 在 520 高画布上已出屏。写死的像素断言是
  **真实像素**、与 scale 无关；数字变了说明有常量没走 px()。

## CI（P89：给退出码装个听的人）
唯一工作流 `.github/workflows/selftest.yml`。`windows-2022` 上三步：configure
（`-G "Visual Studio 17 2022" -A x64`，和本地 CMakeCache 里那份生成器/平台完全一致）→
build Release → `--snapshot selftest.png`。

**门禁四条，全在 ASCII 上判**（日志里的中文会因控制台代码页变样，`[PASS]` 不会）：
1. 退出码 0；
2. `[FAIL]` 行数为 0；
3. 日志里有 `ALL CHECKS PASSED`；
4. PASS 行数 ≥ `env.MIN_CHECKS`（当前 **810**）。**加断言的那一轮顺手把它调大**——
   它必须是一个有人选的数，不能是"这次跑出来多少"。这是"断言只增不减"唯一的执法点。

**故意不做三件事**：
- **不跑视觉回归**。8 张本来就会抖（电平表 / 播放头 / 恢复时间戳），其余依赖 runner 的
  字体与 DPI。这里做逐字节比较，红了也不是缺陷；会喊狼的闸门最后一定被人关掉。
  30 张图照样当 artifact 传上去，给人看。
- **不扫机器上的插件**。`--snapshot` 已经 `setScanExtraPathsOnly(true)`，只看构建树里那两个
  一次性 VST3。"runner 上一个插件都没装"是正常情况，不是坏了。
- **不开 `/WX`**。警告计数并打进 summary（工程要求零 warning），但不因此判红：
  JUCE 头文件里的一个新警告不是本仓库的缺陷。

**跑在仓库根**（cwd ≠ exe 目录），这一形态本地实测过（810/0）：`addDevPluginPaths` 从
**可执行文件**往上找 `*_artefacts/Release/VST3`，不看 cwd；出图落在参数 png 的**同级目录**。
所以根上会多出 `selftest*.png` / `demo.atomic` / `build.log`，`.gitignore` 里有对应五条。

**缓存 `build/_deps`**（key 带 JUCE 版本）。JUCE FetchContent 是两百多兆的浅克隆，
不缓存 15 分钟、缓存后 3 分钟。

## 数据规则
- **挂在音符上的东西都是值成员**（`NoteEnvelope` / `outputSlotOverride` / `groupId` /
  `modTarget` / `modPluginParam`）：复制、拖动时物理上不可能脱钩。采样块同理。
- 音符包络**归一化 0..1**（跟随音符长度），宏曲线**绝对拍**（钉在时间线）。不许统一。
- UI 寻址一律 `(layerIndex, noteId)` 或稳定 id，禁止缓存 `NoteEvent*` / `Layer*`。
- 图层共用一套 content 空间（128 pitch × N beat）；只有激活图层的音符可命中。

## 小节（P56：`beatsPerBar` 在 `Util/SharedRules.h`）
- 4 拍一小节，**只有一处常量** `beatsPerBar`。标尺的小节编号、`nextBarLineAfter()`、
  `barLineAtOrAfter()` 都读它。视图里原来写着字面量 4.0。
- `nextBarLineAfter(beat, grid)`：先按网格吸一下再找**严格更晚**的小节线——
  "下一小节"是标尺画的那条线，不是 beat + 4。
- `barLineAtOrAfter(beat)`：不早于 `beat` 的第一条小节线。别看名字像，它和上面那个是
  两个问题：一个"我要往后走"，一个"我从这儿开始放"。

## 路由（唯一实现 `Model/Routing.h`）
- `routing::resolve()`：**note.outputSlotOverride > group.outputSlotId > layer.outputSlotId >
  第一个乐器槽**。UI 高亮、聚光灯、引擎全调它；另写三段判断，显示和听到就是两套话。
- `routing::resolveSample()`：**sample.outputSlotId（须是 sampler 槽）> 图层默认**。
  它**不看 group** —— 组挂载陈述的是"这些音符由哪个乐器演奏"，采样不由乐器演奏。
- `layerSlotId()` 兜底 `getFirstInstrumentSlotId()`，**跳过 sampler**：落到采样音源会让整层
  音符无声，而且屏幕上找不到原因。
- **P54/P55：两种标，同一个环**。`focusActive` = 聚光灯（用户手动开，**整张网格变暗**）；
  `selectionRouteSlots` = 音符选区落在哪（随选区自动走，**什么都不变暗**）。两者都调
  `RackPanel::drawCardRing(g, card, strength)`——一个实现一份数字（外扩 3、圆角 6/7.5、
  光晕 0.22、主环 0.95、线宽 1.8）。P54 曾用「虚线 + 画在卡内」区分二者，问题是同一张卡
  看起来像两个物体，而且虚线读作"次一等的高亮"；P55 按要求统一。**真正区分它们的是暗幕**，
  不是环的形状。
  · 同卡两者都命中 → 只画一次（焦点优先），避免两条重叠线。
  · 聚光灯在场时选区环 `strength = 0.55`：形状不动，只降 alpha，否则两张卡一样亮，
    分不清哪个是模式。
- **P54：标记是算出来的，不是记下来的**。`updateRackRoutingFocus()` 遍历层 × 音符，用
  `routing::resolve()` 取样集，`setSelectionRouteSlots` 再排序去重、**丢掉已不存在的槽**。
  存 id 的写法会在删槽后留下一个指着洞的环；算的写法自然回落到下一层（层默认）。

## 音频
- 音频线程**绝不**读 Layer / NoteEvent / RackModel：消息线程 `publishSchedule()` 构建不可变
  `PublishedSchedule`，锁只覆盖 shared_ptr 赋值。声部无状态：相位 = freq × 音符内秒数。
- **P70：音频线程读的是裸指针，不是 shared_ptr**。`getScheduleRaw()` → `publishedSchedule`
  （原子发布，acquire 一次 load），发布侧把旧的推进 `scheduleRetired` 并**只保留两个**。
  以前的 `getSchedule()` 每块自旋抢锁 + 拷一个 shared_ptr，而那个拷贝可能是最后一个引用，
  于是整个 `PublishedSchedule`（几十个 note、每个 insert 一个 `shared_ptr<PluginDescription>`）
  在回调里析构。`getSchedule()` 保留给消息线程与 `renderOffline()`。
- **P70：回调里零分配**。`RenderStats` 是成员 `liveStats`，块首 `clear()`（保留 capacity）；
  `addPeak` / `addLayerPeak` / `addChannelPeak` 都带 `size() < capacity()` 闸——容量就是上限，
  超出的槽干脆没有电平表。这条和 `previewRetired`（旧 buffer 交给消息线程释放）是同一条规矩：
  **谁分配谁释放，那个「谁」不能是音频线程**。
- **P70：note-off 判据用取整后的采样偏移**。`on = llround(onOffset) - chunkStart`、
  `off = llround(offOffset) - chunkStart`，跳过条件是 `off < 0 || on >= chunkLen`。
  以前用 `offOffset <= chunkStart` 跳过 + `off < chunkLen` 发送，落在块尾的 note-off
  **两块都不发**，宿主 VST 的长音就一直响到下一次 stop。
- **P70：预览的停止是真停**。`renderSamplePreview()` 每个采样读 `engaged`，为 false 时把
  target 拉到 0，4 ms 淡完后 `active = false`。以前只是清标志位，循环根本不看它。
- 链路：**声部 → 通道总线（通道插入）→ 主插入 → 主推子**。图层推子/组增益在声部阶段（乘），
  音源推子+滤波在总线阶段；`busIndexFor(mixerChannel)`，-1/越界 → 直通。汇总顺序 =
  发布时的 `channelOrder`。
- **主输出也有第三条曲线（mute）**：`MixerModel::numMasterParameters = 3`，
  渲染在 master ramp 上按块首尾各取一次闸值，所以关静音不会跳回满电平。
- 通道条推子**仍是音源推子**："通道 1 → 3"只经通道 3 的**插入**，不经它的推子。
- 自动化算术只写在 `LayerMix::gainAt()/panAt()` 一类函数：音量**乘**推子、声相**加**推子。
  solo/mute 发布时解算成 `audible`。复用缓冲**必须无条件 clear**。
- 只引 `juce_audio_devices`；WAV 头手写 44 字节；dB 走 `Model/Gain.h`。
- **sampler 槽只渲染 clip，不跑声部**（`SlotMix::sampler` 在声部循环入口 break）。
- **clip 的窗口与淡变在 publish 期换成秒**，音频线程只做 `min(elapsed/in, (len-elapsed)/out)`
  的乘法；淡变乘在 de-click 斜坡**之上**。
- **clip 的读取只有 `clipstretch::read()`**（`Audio/ClipStretch.h`）：`(buffer, Playback,
  offset, elapsed, grain)` → 一帧，**纯函数**，没有读头状态。speed == pitch 走单读头
  （原速/变速变调），否则双读头颗粒交叉淡化（只变速/只变调）。

## 电平表（P42：读数是读数，不是最大值）
- 渲染路径一律用 `jmax` 存块峰值（块内峰值不能被平均掉）。**衰减只有一个地方**：
  `audioDeviceIOCallbackWithContext` 里 `beginLiveBlock()` 之后立刻 `playback.decayMeters(numSamples)`，
  **无条件**，在 `isPlaying` 判断之前。原先挂在"完全空转"早退分支里，于是走带在跑但没声音时表永远钉着。
- 衰减速率用 **dB/秒**：`factor = pow(10, -2.5 * dt)`（−50 dB/s），`dt = numSamples / sampleRate`。
  绝不能用"每块乘 0.9"——那与缓冲大小、采样率都绑死，512 帧和 256 帧必须落得一样快才叫时间曲线。
- `hasMeters()`（遍历 `meters / layerMeters / channelMetersL/R / masterMeters`，任一 > 1e-4）
  是"还需要重绘"的唯一判据；落零就停，否则 60 Hz 定时器把静止窗口永远判成还在响。
- 窗口门写作 `if (audio.isPlaying() || audio.getPlayback().hasMeters())`，**放在
  `if (! audio.isPlaying()) return;` 之前**——放后面就漏掉"停止后还在落"那一段，而那正是用户会看的。

## 工程文件（`.atomic`，`formatVersion` = 28）
- **先解析到临时对象、末尾一次性提交**，坏文件绝不破坏当前会话。载入后重置
  `nextNoteId`/`nextSlotId`/组 id（max+1）。
- 老文件必须仍能开：v3 `mod=`、v4 插件（标签 **"PLUGIN"** 全大写）、v5 `mixer=`、
  v6 `<Groups>`（在 `<Notes>` **之前**）、v7 `<Samples>`、v8 `<Mixer channels>`、
  v10 `<Channel out>`、v11 `pluginparam=`、v12 `mono="1"`、v13 `masterMono`、
  v14 `kind="sampler"`、v15 `<Sample offset fadein fadeout>`（不写=0）、
  v16 `<Sample stretch="0..3">`（不写=0=原速窗口）、v17 `<Group mute="1">`（不写=false）、
  v18 `<Slot pluginparam="N">` + `<Slot autotarget="filter|level|plugin">` +
  `<Automation target="plugin">`（三条 lane 全写，`autotarget` 缺失=旧文件）、
  v19 `<Automation target="pan|mute|solo">`（槽位六条 lane 全写）+ `<MasterAutomation>`
  多一条 `target="mute"`、v20 效果器多参数（EQ 九参 / COMP 六参 / REVERB 五参）、
  v21 `<Sample>` 内的 `<Envelope>`（音量曲线，`fadein`/`fadeout` 读成等价斜坡）、
  v22 `<Sample pan="…">` 的 `<PanEnvelope>` + `autotarget="pan"` + `reversed="1"`
  （三者缺失 = 音量曲线 / 未反转 / 声相平 0.5 且禁用）、v23 `<Sample pitchrange="N">`
  （缺省 24）、v24 `<Note groupocc>` / `<Sample groupocc>`——**只在该组有多次出现时写出**
  （`groupId >= 0 && groupOccurrence != 0`），缺失读成 0，所以老文件读进来每处都是第 0 次出现。
  v25（P80）在 root 下追加可选子元素 **`<TempoAutomation enabled="0|1"><Point beat value/>…</TempoAutomation>`**：
  它是 **root 的兄弟节点，不是 `<Mixer>` 的子节点**（速度不属于混音台，`resetToDefault()` 也不该抹掉它）。
  缺失 = 平坦 120 + 自动化关，且**必须显式 `resetToDefault`**，否则打开老工程会继承上一个工程的速度图。
  **新 lane 一律追加在枚举末尾**，老文件不需要映射表。
  v26（P82）把每个对象的一组曲线写成 **`<Curve id="1" name="音量" target="level" pluginparam="3" enabled="1">` + `<Point …>` 子元素，末尾一条 `<CurveState active="2"/>`**；三处（`<Sample>` / `<Group>` / `<Note>`）同一套词汇。
  **`target` 缺失 = 「空」**（曲线留着，不驱动任何东西）——绝不能写成一个词，未知词会被 `routeFromText` 读回 level，"空"就悄悄变成"音量"。v25 及更老的 `<Envelope>` / `<PanEnvelope>` / `<Automation target>` / `mod` / `pluginparam` / `autotarget` **读取分支全部保留**：
  `childHasCurveBank(parent)` 为真才走 bank 读取，否则按老形状迁移进 bank。只有重新保存才写 v26。
  v27（P86）**不改标签，只改值的形状**：`<Sample path>` / `<Slot>` 的采样路径可以写成
  **相对工程文件所在目录的路径**（`media\kick.wav`），写不了才退回绝对。读侧对两种都接受
  （`File::isAbsolutePath` 判），所以 v27 文件 v26 的读法也能读，只是老版本会把相对路径当
  进程当前目录解释——这也是模型里必须存绝对的原因。
  v28（P87）在 root 下追加 **`<Transport loop="0|1" loopStart="8" loopEnd="16"/>`**：
  **`loop="0"` 时两个拍子照样写**（关掉循环不该忘记位置）。缺失 = 关闭 + 默认 0..4。
  它同样是 root 的兄弟节点，不进 `<Mixer>`。
- **选中哪条 lane 显式写 `autotarget`，不靠元素顺序推断**：三条 lane 都写出来之后，
  "最后读到的那条算选中"恒把 target 变成 `plugin`。回退规则只在 `autotarget` **缺失**时启用。
- **P70：`<Layer autotarget>` 同理**。图层只有 volume/pan 两条 lane，所以后果不是
  "恒为 plugin"，而是"**恒为 pan**"——而且读侧是循环里每条就 `setAutomationTarget` 一次，
  连 `expanded` 都没看。现在写侧显式落盘，读侧循环结束后**一次性决定**：
  有 `autotarget` 用它，没有则回退到"带 `expanded="1"` 的那条"（旧文件），都没有就保持默认 volume。
- **组静音存在组上，不存成员上**：一个属性熬过粘贴、复制、合并；写到六十个音符上就是六十个
  可能出错的地方。
- **`out` 的 master 哨兵随上限走**：v11 及更早 = 8，v12 起 = 127。读旧文件必须翻成 master。
- 提交顺序：**先 `setChannelCount`，再 `restoreRouting(parsedOutputs)`**（后者拒绝非活通道）；
  速度图在最后一步提交（`haveTempo ? loadPoints + setEnabled : resetToDefault`）。
- **P70：路由是"替换"，不是"逐条编辑"**。`restoreRouting()` 先把整张表清成主输出，再按文件
  逐条应用，最后一次性 `notify()`。旧写法 `if (parsedOutputs[ch] != master) setChannelOutput(...)`
  有两处错：新工程里"回主输出"的通道一条都不调（上一个会话的路由留下来），而且每条都在
  **残留拓扑**上判环——文件里正确的 `1 → 0` 会因为旧会话的 `0 → 1` 还没清而被拒。
- **P70：模型不给长度设下限**。`addSample` / `restoreSamples` 只 `jmax(0.0, …)`，和
  `restoreNotes` 一致；多短能画是编辑器的 `minimumNoteLength()`。写死 0.25 会让 1/64 的块
  在存盘往返、粘贴、组内补齐时统统变宽 16 倍。
- 导出：发布快照 → 后台按块渲染，长度用 `contentBeats`，不归一化（峰值 >0.999 才拉回）。

## 采样路径与重定位（P86：模型只存绝对，相对是文档的形式）
唯一实现 `Model/SamplePath.h`，纯函数，不碰 `SampleEvent` / `SynthSlot`。`ProjectIO` 只在
写盘与读盘两端各调一次。

**三条规矩**
- **模型里永远是绝对路径**。`SampleEvent::path` 一个字节没变；相对只是文档形式。
  `juce::File("media\\kick.wav")` 会被下一个拿到它的人按**进程当前目录**解释——那就是 bug。
- **相对路径必须解得回原文件才写**。`toStored()` 算出候选 → 用读盘时**同一个** `resolve()`
  跑一遍 → 答案等于原文件才采用，否则退回绝对。跨盘、符号链接、解析后落到别处的 `..` 链
  全部过不了这一关。结果：**永远正确，只是不那么好搬**。
- **读盘时把修好的位置写回模型**，找不到**也写绝对**。否则下一次 Ctrl+S 又把那条死路径存
  一遍，"修好"只维持一个会话。这条是咬测三抓出来的真 bug——第一版把 `media\kick86.wav`
  留在了模型里。

**解析顺序**（`resolveExisting`）：工程目录 → `media/` → `Media/` → `audio/`。子目录只有这
四个"约定俗成"的名字，不做模糊搜索。

**重定位**（`relinkSamplesTo(folder)`）
- **只认用户挑的目录**，不自动猜。`offerSampleRelink()` 在 `loadProjectFromFile` 末尾问一次，
  **默认关闭**（`setSampleRelinkPromptEnabled`，理由同 P84：无头运行会动用户数据，
  而 `renderHeadlessReport` 开工第一件事就是存盘）。
- `FolderIndex` 用**文件名**配对（`byName`），同名多个取先找到的；深度上限 6 层。
  把整块磁盘扫成索引不是"更好的体验"，是一次几十分钟的卡死。
- 修好之后**立刻写回模型**（`SampleEvent*` 与 `SynthSlot*` 两条路都改，`path()` 返回引用
  就是为了这个），再 `layersChanged()`。

**恢复副本里的相对路径**以 `recoveryOf` 指向的工程为基准，**不是**副本自己所在的
`Recovery/` 目录——所以 `restoreState(xml, projectFile)` 多了一个参数，传 `nullptr` 时用
`currentProject`。

**陷阱**
- `juce::File::isAbsolute` **不存在**，只有静态的 `juce::File::isAbsolutePath (StringRef)`；
  默认构造的 `File` 调它要小心，`PianoRollInternals` 之外的地方一律走 `SamplePath`。
- 采样器槽与采样块**共用同一个路径字段语义**，清扫（`pruneOrphanSamplerSlots`）按路径比对，
  改路径等于换文件，别在清扫之后才 relink。

## 循环区间（P87：一个区间，不是一个标志）
唯一实现 `Model/LoopRegion.h`（header-only）。`PlaybackEngine` 与钢琴窗读同一份。

**不变量**
- 起点永远在终点之前——谁从哪个方向拖都一样（`setRange` 排序）。
- 不短于 `minLengthBeats = 0.25` 拍。一次误点不该做出一个在两采样之间反复抽搐的循环。
- 永不小于 0（`setStart` / `moveBy` 撞 0 停住；`moveBy` 可以越过末尾——在静音上循环是
  合法的数拍方式）。
- **`isUsable()` 是唯一判据**（`enabled && 长度够`）。渲染、绘制、磁盘、手势全问它。

**开关与数字分开**
- 关掉**不许忘记位置**，所以磁盘上 `loop="0"` 时两个拍子照样写。
- 关闭时 `renderNextBlock` 里 `wrapStart/wrapEnd` 退化成 `[0, length)` = P87 之前的整首循环，
  **老行为逐位不变**，这是它能上线的理由。
- 绘制：`drawRuler` 里 `if (! bar.isEmpty() && loopRegion.isUsable())` 才画。少了前面那半句，
  29 张视觉快照里 18 张会凭空多一个 0–4 拍的虚框。
- 引擎每块**一次性**读出 `LoopRegion`（三个 relaxed 原子），循环内不再问。

**手势**（`PianoRollInput.cpp`，标尺底部 `ui::px(7)`）
- 5px 内判 `loopStart` / `loopEnd`；`contains` 判 `loopMove`；其余 `loopNew`。只认左键。
- 双击循环带翻 `enabled`，不动数字。
- `loopNew` 拖完**顺手 `enabled = true`**——画它就是在要它，再点一次才生效是没人猜得到的规则。
- `loopMove` 每次从 `loopDragOriginal` **重建**再 `moveBy(总位移)`，不是累加 delta：
  每像素的取整误差会攒成漂移。
- 三个函数分开是为了可测：`applyLoopDragToScreenX` / `finishLoopGesture` /
  `simulateLoopGestureForTest`。测试驱动的是真代码，不是对它的描述。
- `finishLoopGesture()` 只在**真的变了**才 `onLoopRegionChanged`（`==` 早退）——按一下没拖
  是"下次从这儿开始"，不是"我改了工程"。
- 曾经做成 12px + "循环"两个字，把标尺上的 `0:06.0` 压掉了（截图确认过），所以只剩
  `ui::px(7)` 一条窄带 + 车道底色 + 两条竖线。

## 节拍器与预备拍（P88：点击是监听，不是声音）
唯一实现 `Audio/Metronome.h/.cpp`。走带读它，导出那条路根本不认识它。

**它住在哪里**
- `Metronome` 由 `PlaybackEngine` 持有（`getMetronome()`），`setSampleRate()` 里 `prepare()`
  ——**不是** `prepareToPlay()`：自检走 `setSampleRate`，把形状建在那儿才和真机同一条路。
- `renderNextBlock` 每个 segment 之后调一次
  `metronome.render (slice, segment, posBeats, beatsPerSample, false)`。
- `renderInto` 里**没有**。导出 / 分轨 / 离线分析 / 渲染成采样的"没有点击"是结构性的。

**两种声音**
- 同一个形状（`sin × exp(-t/0.0035)` × 0.5 ms 淡入），只是音高不同：
  小节第一拍 1600 Hz / 其余 1000 Hz，增益 0.95 / 0.62（重音更响也更亮）。
- 淡入不是装饰：包络从 1 起而正弦从 0 起，第 1 个采样就是个台阶，那个台阶听起来"刺"。
- 长度 `clickLengthMs = 12`：够长到有音高，够短到 300 BPM 时衰减已经结束。

**分句与声部**
- `for (int beat = ceil (fromBeat - 1e-9); beat < toBeat; ++beat)`。`-1e-9` 保证正好落在
  块边界上的那一拍只算一次。
- `at = floor ((beat - fromBeat) / beatsPerSample)`，落在**该在的那个采样**上。
- 四个声部（`maxVoices = 4`）；满了丢 `position` 最大的那个（衰减已经过去）。
  `mixVoices` 里 `int i = v.position == 0 ? max (0, v.startAt) : 0`——跨块的那一击从块首继续。
- 一块跨一个速度变化时，按块首的速度画：误差只在**两击之间的间隔**，不在**块内那一击的位置**。

**开关与音量**
- `if (! force && ! isEnabled()) return;`。`force = true` 是预备拍：要求被数的人不能因为
  没开节拍器而听不到数。
- `gain <= 0` 时**先 `silence()` 再返回**，否则衰减中途把音量拧到 0，尾巴会冻住。
- 音量是它自己的，不骑主推子（动一下混音台不该改掉演奏者的时间参考）。
- `setBeatsPerBar` 由 `MainComponent::applyMetronomeSettings()` 从工程拍号喂进来。

**预备拍（`PlaybackEngine`）**
- `startPreRoll(int beats)` / `cancelPreRoll()` / `isPreRolling()` / `getPreRollBeatsLeft()` /
  `renderPreRoll(buf, n)`。
- `startPreRoll` 里**先写 payload 再写 gate**：`preRollBeatsLeft` 是 `isPreRolling()` 读的那个，
  所以它最后写。中间溜进来的回调渲染一个"total 老了一块"的数拍，无害；加锁更贵。
- `renderPreRoll` **不改 `positionBeats`**；数拍期间 `blockBeats.store(0.0)`。
- 数完**同一个块里**交接：`cancelPreRoll()` 后把 `numSamples - segment` 交给 `renderNextBlock`。
  在块边界交接会塞一个 buffer 的静音（最多 46 ms，且随 buffer 大小变）。
- `getPreRollBeatsLeft()` = `total - floor (preRollBeat)`，所以读数从 N 掉到 1。
- 速度取**播放头所在位置**的速度，且数拍内部不跟速度曲线变化（数拍不是音乐）。
- `setPositionBeats` / `AudioEngine::pause` / `finishRecording` 一律 `cancelPreRoll()`。

**录音与预备拍**
- `MainComponent::startRecording()` 顺序：`playTransport()`（会 rewind 停在线尾的播放头）
  → `startPreRoll (countIn)` → 读 `recordStartBeat` → `takeCollector.begin` → `beginRecording`。
- `skipSamples = countIn × getSecondsPerBeat() × sampleRate`。
- `Recorder::begin (samplesToSkip)` → `std::atomic<int> skipRemaining`，`append()` 里先按它
  左移 `sourceOffset`。**不是"数完再 arm"**：那要等一个 timer tick，take 会从自己的
  downbeat 之后开始。

**顶栏**
- `getMetronomeButtonBounds()` = `getBpmFieldBounds().getRight() + ui::px(6)`，`ui::px(34) × ui::px(28)`；
  `getNewButtonBounds()` 跟着右移（P88 挪了 36 px）。
- 字形：上窄下宽的描边机身 + 斜臂 + **末端 3.2px 实心配重圆**。少了配重读成 λ。
- 录音时读数区改写 `预备 N`（`theme::record`）；读数区本来在写 `0:02.0`。
- 左键开关，右键菜单：音量 0/25/50/75/100%、预备拍 关 / 1 小节 / 2 小节
  （按工程拍号算，3/4 歌写"3 拍"）。
- 状态还没进工程文件：`metronomeEnabled` / `metronomeVolume` / `countInBeats` 是 `MainComponent`
  的成员，重启回默认（关 / 50% / 4 拍）。`formatVersion` 仍是 28。

## 量化（P90：形状会跟过去，位置不会）

**一个纯函数**：`Model/Quantize.h` 的 `quantize::apply (beat, grid, strength)`。
`nearestGridPoint` 用 `round` 不用 `floor`——一个早 6 ms 的音，向下取整会把它推到下一格。
强度是"走过去多少"，0 = 原样（逐位相同），≥1 走满；**`>= 1.0` 早退就是"夹紧"**，
不需要 `std::min`（写过一次，删了：它咬不住，因为早退先接住了）。

**入口**：`PianoRollComponent::quantizeSelection (gridBeats, strength)`，右键画布菜单的
`量化`（`addQuantizeItems` 单独建菜单，自检读菜单不读截图）。五档网格 + 四档强度
（100/75/50/25，勾在当前）。网格与强度是**工具**的设置，存在组件上，不进工程文件。

**改的是起点**：音高与长度原样。音符与**采样块都动**（都是"人把它放在时间里的某个位置"）。
`moved == 0` 就直接返回——不重绘、不产生撤销步；判"算不算动"用 `abs(diff) < 1e-9`，
因为 `round(x/g)*g` 对 `0.1` 这种网格不是逐位相等（差 5e-17），用 `==` 会把"已经在格上"
报成一次移动。

**编组是这一轮唯一与拖动不同的地方，两件事都要做**：

1. **重锚定** `setOccurrenceStart(groupId, occurrence, 该出现里最早的成员)`。出现原点是
   **存下来的数**（P62），拖动的 `applyOccurrenceOriginsForMove` 会跟着挪它，量化不会；
   而 `syncGroupShape` 是按 `副本自己的原点 + 源码的相对形状` 落位的。原点留在旧位置，
   副本就会整体平移"源码第一个成员的那一份误差"——实测 8.00/8.57 → 7.89/8.64 而不是
   8.00/8.75，形状看着是对的，位置错了一拍误差。
2. **同步形状**，且**无条件**做，包括"整次出现都选中"。`collectTwinsForEdit` 在整次出现
   时跳过同步，因为拖动整次出现**不改形状**；量化改（每个成员按自己的误差归位），
   跳过的结果是一个组两份形状，下一次编辑任何一处都悄悄挑一个。

## 会话与撤销
- **会话重置唯一入口 `buildSession(withEmptyLayer)`**：清 automation/mixer → restoreLayers →
  restoreSlots → BPM 120 → 清 currentProject → reconcilePlugins。空白 = 1 空图层 + 1 原装音源。
  `loadDemoProject()` = `buildSession(false)` + demo，**不要**复用 `buildBlankProject()`。
- 脏标记 = `savedStateText` + `dirtyCache`；关窗/新建/载入统一走 `confirmDiscardChanges`，
  `handleCloseRequest()` 返回 false = 对话框接管退出。
- 撤销 = 整工程 XML 快照 + AsyncUpdater 合并同一轮次；`restoringHistory` 期间 Listener 返回。
  **粒度的单位是动作，不是消息循环**（P71）：`beginUndoGesture()` / `endUndoGesture()` 包住一次
  鼠标按下与释放，`commitHistory()` 在 `undoGestureActive` 期间直接 return，快照只在释放时落一次。
  边界来自 `GestureWatcher`——一个挂在 `juce::Desktop` 上的 `MouseListener`，不是面板自己的：
  混音台 / 插件编辑器 / 自动化窗都是独立顶层窗口。`undo()` / `redo()` 先把打开的手势结掉（拖拽中
  按 Ctrl+Z = 取消这次拖拽）；`beginUndoGesture()` 里**先 commit 再置标志**，否则自己也被拦住。
  唯一一个例外是异步提交的"非鼠标编辑"（键盘删除等）：它们在下一个消息循环里照常落一步。
- 混音台在 `MixerWindow`，面板归 MainComponent：关窗不销毁状态。

## 崩溃恢复（P84：副本不是工程）
- **一句话不变量**：**副本存在 ⟺ 有未保存的编辑且进程没能正常收尾**。保存工程 → 删；
  `confirmDiscardChanges` 里用户选「不保存」→ 删；载入别的工程 → 删（那些改动被放弃了）；
  正常关闭且无改动 → 本来就没有。所以它唯一能活下来的方式是崩溃。这条链子断一环，
  恢复就变成制造重复（该删没删）或制造丢件（不该删删了）。
- **`Util/Recovery.h` 是纯函数层，把判断和对话框分开**：`getFileFor` / `inspect` / `write` /
  `readProjectPath` / `listIn` / `discard`。UI 只有 `MainComponent::offerRecoveryIfAny` 一层胶水。
  自检直接测下面那层——会错的从来是判据，不是弹窗。
- **文件名带路径 hash**（FNV-1a，取整个 full path）：两个都叫 `song.atomic` 的工程不能共用一个
  副本，否则第二个会覆盖第一个正要救的东西。唯一接受的冲突是两个**从未保存**的窗口——它们都是
  `untitled`。文件名无法区分两个无名会话，而每次启动随机后缀会让"这是哪个"在崩溃后从"含糊"
  变成"不可回答"。
- **副本是一个能被直接打开的工程**：根元素只多加 `recoveryOf` / `recoveredAt` 两个属性，
  解析器读它认识的、忽略其余。用户最后的逃生手段就是拿文件浏览器打开它。
- **`inspect()` 的四个答案**：`none`（没有）／`unusable`（空、或不是会话文档）／
  `stale`（不比工程新）／`offer`。工程不存在（从未保存）→ **一定 offer**，那是唯一没有任何
  其他地方存着的场景。**打平算 stale**：同一秒说不清谁新，不打扰比赌一条改动便宜。
- **原子写**：`File::replaceWithText` → JUCE 的 `TemporaryFile`（同目录隐藏临时文件 + 改名）。
  写一半崩只会丢这一次更新，绝不留下半截。**别自己 `createOutputStream` 然后追加**——
  那既不是原子的，也没人会记得 `deleteFile`（P82-A 就是这个）。
- **自动保存的节拍**：`timerCallback` **最前面**（在 `! audio.isPlaying()` 早退之上，因为停着走带
  正是人在编辑的时候），每 5 秒看一次。`recoveryDirty` 由 `modelChanged()` 置位，所以不编辑时
  零成本——否则每 5 秒序列化一次整工程，只为发现"没变"。手势中／导出中／恢复中／对话框开着都不写。
- **绝不改 `currentProject`**。这是最容易犯的错：复用 `saveProjectToFile` 会把 `currentProject`
  指向恢复目录，下一次 Ctrl+S 就把歌存进了 Rescue 文件夹。
- **恢复后仍然是脏的**：`savedStateText` 不更新。顺手标干净 = 下一次关闭一声不响扔掉它。
- **默认关闭**：`setRecoveryEnabled(true)` 只在 `Main.cpp` 的窗口路径调用。`renderHeadlessReport`
  开工第一件事就是 `saveProjectToFile`，默认开启会让一次快照删掉用户真实的副本。
- **没被断言覆盖的一层**：真窗口里的那个模态框（无头跑不起消息循环）。它下面的判据全在纯函数里，
  所以漏的是"弹没弹"，不是"该不该弹"。

## 插件延迟补偿（P83：慢的那条不能被拉快）
- **控制台只有一种汇合**：子通道加进父通道的总线。`MixerModel::masterChannel`（=127）是"直接去
  主输出"的哨兵，同时也就是 bypass 总线的下标。别的地方没有并行路径。
- **`Audio/PdcDelay.h` 的 `pdc::solve` 是唯一的延迟表**，纯函数，`SelfTest.cpp` 直接喂它数字：
  `own[c]` = 自己的插入延迟；`entry[c]` = 该通道**入口**应有的事件时刻（= 最慢孩子结束的时刻）；
  返回值 = 控制台总延迟。`slide[c]` 只对"去主输出"的通道有意义，子通道的等待量是
  `entry[父] - 自己的结束`，**算在汇合点，不进表**（进表就是第二个答案）。
- **补偿只发生在汇合点**：子通道 → 父通道、通道 → 主输出、bypass 总线 → 主输出。
  **绝不在源头做**：在通道自己的插入链之前按住它，等于让它在还没干活时就开始等。
- **两条延迟线，不是一条**：`pdcEntry[c]` 存这个通道的**入口**（让直达的声音等一个慢孩子），
  `pdcExit[c]` 存它**处理完的出口**（让它等一个更慢的兄弟）。它们在同一个 block 的不同时刻被写，
  一条线做不到两件事。
- **唯一问延迟的地方是 `PluginHost::getLatencySamples`**，只在发布期调（消息线程，会拿宿主锁）。
  内置效果全都是 0 延迟（`fx::Type::vst` 以外不问），音频线程不做任何插件调用。
- **容量 2048 采样（≈42 ms @48k）**，`pdc::maxSamples`，`Line::prepare` 是唯一的分配、惰性按用到的
  通道分配（128 通道全开是 16 MB）。超出的插件被夹住——这是本设计唯一用户能察觉的边界。
- **退化性质是它上线的理由**：没有任何插件报延迟 ⟹ 所有表为 0 ⟹ 渲染走 P83 之前那条路，逐位相同。
  `maxPdcLatency == 0` 时有断言钉住，别加"至少 1"这类兜底。
- **延迟线是记忆体**：locate、停止（`pluginPanicRequested`）、`resetMixState()` 都要清，
  否则导出会带上上一遍的开头 42 ms。
- **自检的边界**：无头自检加载不了 VST3，所以 `setInsertLatencyForTest(channel, samples)` 是
  唯一让补偿路径真的被渲染的手段。端到端断言两条：给一条**静默**通道强加 32 采样延迟，
  「bypass 总线」与「过通道」两条路径都必须整个混音逐样本后移 32。

## 曲线库（P82：一个形状 + 一个去处）
- **`Model/CurveBank<CurveType>` 是唯一的曲线容器**：音符 / 采样 / 编组三个对象各持一份，
  `CurveSlot{id, name, target, pluginParam, curve}`，`target = -1` 即**空**。以前"哪条曲线"和
  "驱动哪个参数"是同一个答案（曲线集合就是枚举），所以「留着形状、先不驱动」这句话说不出来。
- **三条不变量**（写在类注释里，改动前先读）：
  ①**目标至多归一条曲线**——`route()` 会从原来那条手里夺走并把它的 `target` 置 -1；
  插件目标例外地按 `pluginParam` 区分（同一台音源的参数 3 和 17 是两个去处）。
  ②**列表永不为空**——`remove()` 拒绝最后一条，因为引擎里每个读者都直接解引用"当前那条"。
  ③**用 id 不用下标**——框开着的时候可能增删，下标会把编辑器悄悄指到别人的形状上。
- **const 读永不创建**：`volumeCurve()` / `automationFor()` / `activeAutomation()` 找不到就返回
  `idleCurve` / `idleLane`。`NoteEnvelope` 的默认构造是**启用的 ADSR**，替身必须用
  `NoteEnvelope::makeIdle()`（扁平 + 禁用）；`AutomationLane` 默认 `enabled = false`，直接默认构造即可。
  任何 `ensure…` 只能在写路径上调，painter 里调一次就等于"看一眼就改模型"。
- **新建曲线不路由、且是扁平的**：`add()` 之后 `target = -1`、形状是 `makeIdle()`。点「新建」是在要
  一张白纸，不是要接管别人正在驱动的参数——顺手 `route` 到 level 是过去那条 `add` 咬测挂掉的原因。
- **旧访问器必须保语义**：`NoteEvent::envelope()/modTarget()/setModTarget()/modPluginParam()`、
  `SampleEvent::volumeCurve()/panCurve()/curveDriving()/ensureCurveDriving()`、
  `NoteGroup::automationFor()/getLevelAutomation()/ensureAutomationFor()/laneDriving()` 全是
  bank 上的薄壳。改它们之前先数一遍调用点：引擎、三个编辑窗、撤销、剪贴板都在用。
- **UI 一侧**：三个自动化框的底部是**两个下拉**，`splitCurveRow()` 一处定义、paint 与命中测试
  都调它（左格 56% 给"哪条曲线"，右格给"去哪"）；hover 是一个 `int`（0/1/2）而不是两个 bool，
  否则相邻两格会同时点亮。左菜单是曲线列表 + 新建/重命名/删除，右菜单第一项是**空**。
- **不要给"当前是哪条曲线 / 哪个目标"建缓存字段**：`PianoRollComponent::expandedGroupTarget`
  曾经就是这样一个第二真相（P73 同类 bug 的源头），现在只有 `getExpandedGroupTarget()`
  每次去问 `group->curveTarget()`。

## 自动化
- 每个 owner 常驻 `std::array<AutomationLane, N>` + `automationFor(target)`；
  `AutomationModel` 只存 `{id, owner, ownerId, target}`，**从不存 `AutomationLane*`**。
- 建轨只有一条路 `MainComponent::ensureAutomationTrack(...)`；第三项"展开/收起曲线"是内联
  曲线唯一开关，别删。
- 行高是 UI 偏好、**按 trackId 存**；`index * rowHeight` 即 bug；行底 6px resize 带命中优先。
- 通道 owner 是**音源**；主输出 owner = `master`、ownerId = 0。
- **槽位有六个 target**（`SlotAutomationTarget`：`filter` / `level` / `plugin` / `pan` /
  `mute` / `solo`，`numSlotAutomationTargets = 6`）。P43 追加后三个**只能加在末尾**：
  枚举顺序就是 `automationLanes` 的下标，中间插一个会让所有老工程读错 lane。
  解析一律 `jlimit (0, numSlotAutomationTargets - 1, target)`。
- **静音/独奏是闸不是量**：`SlotMix::isMutedAt/isSoloedAt` 以 0.5 为界读曲线，
  不参与乘法链；**声相是加法**（`panAt` = 推子 + 曲线×2-1，夹 ±1）。
- **音源参数 lane 是"值 + 地址"两件套**：`AutomationLane` 存曲线，`SynthSlot::pluginParam`
  存参数下标（-1 = 没选）。发布条件 = `pluginId > 0 && drivesPluginParam() && lane.isEnabled()`
  且有点，窗口 `0 .. jmax(1, contentBeats)`、`priority = 0`。
- 建音源参数轨只有 `MainComponent::ensureSlotPluginParamTrack(slotId, paramIndex)`：
  先 `rack.setPluginParam`，曲线为空/仅两点才 `resetFlat(current, length)`（别抹掉用户画过的），
  再 `setAutomationTarget(plugin)` + `setAutomationExpanded(true)` + `addTrack` + `revealTrack`。
- 参数清单只有 `pluginParamsForSlot(slotId)`（**非 const**，要开插件才拿得到参数）。
  `pianoRoll.setPluginParamLister` 只是转发，不能有第二份实现。
- **「创建自动化」是动词**：`addSlotAutomationItems(menu, slotId, idBase)` +
  `applySlotAutomationChoice(result, slotId, idBase)` 成对提供，卡片 / 通道条 / 编组 /
  钢琴窗 / 自动化窗全都调它，菜单名只出现一次。参数项 id = `idBase + kSlotAutoParamBase + index`
  （`kSlotAutoParamBase = 10000`，必须避开卡片菜单已有的 10..13 / 20 / 30..41 / 200+n / 1000+ch），
  最多列 64 个，超出追加一条禁止项的「还有 N 个参数没有列出」。
- 时间线长度变化经 `PianoRollComponent::onTimelineChanged` 灌给自动化窗。

## 速度自动化（P80：时间是积分，不是一个数）
- **`secondsPerBeat` 已经不存在了**。速度一旦会变，「一拍几秒」就是 `1/tempo` 在拍上的积分，
  任何「当前 BPM × 拍数」的写法在变速下都是错的。时间换算的唯一出处是 `Model/TempoMap.h`。
- 公开面：`flat(bpm)`、`fromLane(lane, manualBpm, lengthBeats)`、`bpmAtBeat`、`secondsPerBeatAt`、
  `secondsAtBeat`、`beatAtSeconds`、`secondsBetween`、`isFlat`、`hasSameShapeAs`。
  只读、`noexcept`、不分配 —— 音频线程可以随便调。
- `setPoints` 是唯一入口（`flat` 与 `fromLane` 都经过它）：过滤非有限值 → clamp 到 `[0,∞)` 与
  `[minBpm, maxBpm]` → `stable_sort` → 同 beat 去重（**后者胜**：曲线从那一拍起就是这个速度）→
  空则补 `{0, 120}` → 首点 beat > 0 就补 `{0, 首点.bpm}` → 重建 `secondsAt` 累积表。
- 段内积分（`q = (b.bpm - a.bpm) / span`）：
  `secondsAt[i] = secondsAt[i-1] + (60/q)·ln(b.bpm / a.bpm)`；`|q| < 1e-9` 走 `span·60/a.bpm`。
  反函数 `bpm(t) = a.bpm·exp(q·dt/60)`、`beat = a.beat + (bpm - a.bpm)/q`。两者互为精确反函数
  （自检用 20 万步中点法数值积分对撞，误差 < 1e-6）。
- **平坦分支必须走线性形式**：`|q|` 极小时 `ln(1+ε)` 的有效位比 `dt·bpm/60` 少。改这两条分支
  别只看语法——**它们互换了也编译得过**，只是解析出来的东西是错的（第一版咬测还原时就撞上过）。
- **曲线值 → BPM 是 `60·4^v`**（`tempo::bpmForValue`），不是线性映射：v=0 → 60、0.5 → 120、
  1 → 240。画到一半就是「半个速度」，而 60 → 120 正好一个八度。反函数 `tempo::valueForBpm`。
- `TempoModel` 持一条 lane + `Listener::tempoChanged()`。**它不在 `MixerModel` 里**：
  `MixerModel::resetToDefault()` 是「重置混音台」，不该顺手抹掉速度图；工程文件里
  `<TempoAutomation>` 也是 root 的子元素、与 `<Mixer>` 平级。
- `TempoModel::resetToDefault` 用 `resetFlat(valueForBpm(120), len)` + `setEnabled(false)`，
  **不是** lane 构造时的 1.0 —— 否则「打开速度自动化」这一步就把工程变成 240 BPM 了。
- `fromLane` 的闸门是 `! lane.isEnabled() || lane.getPoints().size() < 2` → `flat(manualBpm)`，
  也就是「关掉自动化就把控制权还给 BPM 框」。`resetFlat` 永远留两个点，所以这两条够用。
- `AutomationOwner::tempo`：`ownerId` 与 `target` 恒为 0，`addTrack` 的 `ownerId <= 0` 校验
  必须为它（和 master 一样）开一口。它**不是 master 的第四条 lane**：速度不在信号路径上，
  共用一个数组等于让「主输出的声相」和「工程速度」互相挤位置。
- 发布：`PublishedSchedule::tempo` 是一个 `TempoMap` **值**，消息线程算好、随 schedule 一起
  发布；`renderNextBlock` 里**每段重算** `beatsPerSample = 1/(sr·map.secondsPerBeatAt(posBeats))`。
  每块只算一次不够——渐快时整块都按块首速度爬，推一秒音频能差 4e-3 秒出来。
- 走带与导出一并走图：`setPosition` = `beatAtSeconds`、`getPosition` = `secondsAtBeat`；
  `AudioExport` / `ScopeRender` 的 `contentSeconds = tempo.secondsAtBeat(contentBeats)`，
  时长读数反向走 `beatAtSeconds`。**任何地方都不该再出现 `beats * spb`。**
- UI 入口在 **BPM 框的右键菜单**（`ui::showValueMenu` 的 `addExtra`，id `9101`）：
  「显示速度自动化行」。速度没有卡片、没有行、没有天然归属，入口只能挂在用户已经在看的那块
  控件上。勾选态由 `MainComponent::automationTracksChanged()` 反灌
  （`bpmField.setTempoAutomationShown(hasTempoAutomationTrack())`），BPM 框不自己记状态。
- 窗口里 `parameter` 只写「速度」，量程 `60 / 120 / 240 BPM` 画在**窗口右缘**：
  row 横跨整条时间线（64 拍），右对齐到 row 就跑到视野外了。
- 老工程的迁移是「补默认」不是「沿用」：文件里没有 `<TempoAutomation>` 就 `resetToDefault`，
  否则打开一个 v25 老工程会继承上一个工程的速度图。

## 录制（P81：一次按键，两样产物）

- 录制不是一个功能，是两件：**音频**（声卡听到了什么）和 **MIDI**（手弹了什么）。
  它们落到不同地方是因为它们是不同的事物——音频变成采样（WAV + 采样器槽 + 块），
  音符变成一个自动命名的编组，组上那两条曲线就是同一只手拧过的旋钮。
- **输入侧是本阶段才有的东西**：`AudioEngine::start()` 一直写 `initialise (0, 2, ...)`，
  连输入通道都没开。现在先要 `(2, 2)`，失败回落 `(0, 2)`——"没有麦克风"不等于"没有 DAW"。
  `hasAudioInput()` 看 `getActiveInputChannels().countNumberOfSetBits()`。
- `Audio/Recorder.h` 两个类，各自只待在一个线程里：
  - **`TakeCollector`**（消息线程）：`begin / noteOn / noteOff / control / finish`。
    还按着的音符在 `finish(endBeat)` 处收尾；同音高重触发把前一个截断；没有 on 的 off
    直接忽略；零长度敲击抬到 1/64 拍（`minRecordedNoteBeats`）。
  - **`RecordBuffer`**（音频线程写、消息线程读）：预分配 `AudioBuffer` + 两个原子
    （`writePos` / `armed`）。音频线程只 `copyFrom`，不分配、不加锁；满了**自己停**，
    绝不回绕——丢尾巴读作"它停了"，覆盖开头是"整段没了"。
    `finish()` **先读 `writePos` 再 disarm**：反过来的话，正在写的那一块会被算进 take 里，
    交回来的是半块。
- **时间戳在事件发生的地方取**：`MidiInputHub::onNote/onControl` 是 MIDI 线程，
  在那里读 `playback.getPositionBeats()`（原子 relaxed），再把 beat 一起 `callAsync` 到消息线程。
  在消息线程里问位置等于把队列延迟算成走音——每个音符都错 5 ms。
- **CC 映射只有三条**：CC 7（channel volume）/ CC 11（expression）→ 组的 `level` lane，
  CC 10（pan）→ 组的 `pan` lane。其余（调制轮、呼吸……）**记账但不猜**：
  在 `Take::ignoredControls` 里数一下，不发明映射。理由：旋钮到曲线的映射一旦猜错，
  回放听起来不对，而画面上没有任何东西能解释为什么。
- **曲线要稀疏化**：`AutomationLane::maxPoints` 是 32，而一只手推一次推子能发两百个 CC。
  `decimateRecordedCurve` 两趟：先按"值动了 1%"或"过了 0.5 拍"留点（第二个条件不能省，
  慢速扫弦时单看值差会被削成两个点），再按等距抽到 30 个以内。首尾永远保留。
  **少于两个点就不写 lane**——`loadPoints` 会把单点修成一条横贯全工程的直线，
  那等于把工程音量悄悄改掉。
- `MainComponent::commitTake(take, audio, sampleRate)` 是唯一落位入口，按钮与自检共用：
  1. 音频 → `makeSampleBuffer` → `writeSampleFile`（16-bit PCM，手写 44 字节头）→
     `rack.addSampler` → `layer->addSample`；块的**宽度走 `TempoMap`**
     （`beatAtSeconds(secondsAtBeat(start) + lengthSeconds)`），不是 `秒 × BPM / 60`。
  2. 音符 → `layer->addNote` × n → `addGroup("录音 N")` → `assignNotesToGroup`。
  3. CC → `applyRecordedControls` 写组的两条 lane 并 `setEnabled(true)`。
  4. 空 take（没音符也没音频）**什么都不做**，连编号都不加——一次没录到东西的按键
     不该在工程里留下一个空组。
- `recording` 是 **`std::atomic<bool>`**：MIDI 线程读它。普通 bool 在这里就是数据竞争。
- 停止键先 `finishRecording()` 再 `parkTransportAt()`：反过来的话，take 的结束位置
  是"跑回起点之后"的位置，最后一个音会变成长得离谱的一条。
- 录音文件落在 `Music/AtomicDAW Recordings/录音 N.wav`（`setRecordingsFolder` 可改；
  自检和 demo 都改到临时目录——测试不该往用户的音乐库或仓库里扔 WAV）。
- **工程文件不升版本（仍是 25）**：录音产出用的全是已有字段——`<Sample>` 的
  `slot / path / name / start / length`、`<Slot kind="sampler">`、`<Group>` + `<Occurrence>`。
  没有新的运行时状态需要落盘：`recording`、`takeCollector`、`recordingCounter`（编号只影响
  名字，下一次启动从 1 数起也不会错），`lastRecordGroupId / lastRecordClipId` 是给自检与
  定位用的缓存。凡是"新一轮产出刚好全是旧字段"的功能，都别顺手升版本。

## 三层叠加（P36：`Model/AutomationStack.h`）
**法则只有一份**，纯函数，引擎与界面都调它，禁止在别处再写一遍乘法链。
- 三层：**宏**（图层 volume lane × 合成器 level lane，两个 fader 也在这里）、**组**（trim ×
  level lane × mute）、**音符**（envelope，按 u 归一化）。
- `stackLevel(macro, group, note)` = **相乘**；`stackPan(macro, group, note)` = **相加后夹 ±1**；
  `stackPluginParam(group, note)` = **相乘**。缺层 = identity（电平/参数 1、声相 0）。
- 入参里的"组"那一层，P42 起也承载**音源自己的参数 lane**（`SynthSlot::pluginParam` 指定的那条）：
  它跟编组 lane、音符包络走同一个 `stackPluginParam`，没有第四条法则。
- 音符曲线只在 `modTarget == level` 时算电平层；pan/pitch 曲线不参与乘积（否则画面会说谎）。
- 宿主插件参数**不再是"高优先级覆盖"**：`applyHostedParamAutomation` 先收集本 chunk 的曲线
  （定长数组，音频线程不分配），按 `paramIndex` 去重后**累乘写一次**。`priority` 只剩排序意义。
- UI：`NoteEnvelopePanel::rebuildOverlay()` 把宏/组采样到音符的 u 坐标（`sampleLaneInto`），
  `EnvelopeEditor` 画青（宏）橙（组）虚线 + 琥珀合计线 + 一行图例
  「宏 a × 组 b × 音符 c → 乘积」。**图例的乘积必须由 `stackLevel` 算**，不能自己乘。
- 面板在 `layersChanged/notesChanged/rackChanged/openFor` 四处重建 overlay；路由会换合成器，
  所以 rackChanged 也算。

## UI 制度
- 唯一密度旋钮 `ui::scale = 1.3f`。几何 `ui::px()/pf()`、字号 `ui::sp()`，**禁止裸像素**。
  **不缩放**：`numPitches`、内联自动化框最小宽度、gutter。
- 时间线左侧只有 `ui::gutterWidth` 一个宽度：钢琴键与自动化窗列表都指向它。
- 菜单/对话框字体走 `Util/UiLookAndFeel.h` + `setDefaultLookAndFeel`（popup 归 desktop），
  `shutdown()` 置 nullptr。
- 自绘图标由控件尺寸派生，禁止固定 inset。`▾` 一律 `mixerlook::drawCaret()`。
- `g.drawText(t, {...}, ...)` 会 C2668 歧义，必须写 `juce::Rectangle<int>(...)`；量中文用
  `GlyphArrangement` + `getBoundingBox`。塞不下**先让邻控件让位**，别压字号（≈9.5sp 下限）。
- `withHeight()` 保中心、`withY()` 定顶边；**`withX()` 保留宽度**（要改宽得配 `.withWidth`）。
- 窗口最大化 = `setBounds(userArea)` **减 `peer->getFrameSize()`**。
- **顶栏顺序**：新建 → 保存/载入/导出 → 撤销/重做（图标）→ 混音台 → 设置。
- **图层栏标题栏的眼睛（P65）**：`LayerPanel::getVisibilityToggleBounds()` 在标题右端，
  图标复用行内的 `drawEye()`。状态不另存字段，由 `areAllLayersVisible()` 从模型派生
  （活动层永远画，不参与判断）；一次点击 `toggleAllLayerVisibility()` 在"全部可见"和
  "只留活动层"之间翻转，写的是每行眼睛的同一个 ghost 标志。
- **数值控件右键 = 复位**：`Util/ValueMenu.h` 的 `ui::showValueMenu()`，复位 id = **9001**。
  **二级层级一律 `addSubMenu`**；宿主菜单拆 `fill…` + `apply…`。
- **同一个"动词"只许有一个菜单出处**（P42）：卡片 / 通道条 / 编组 / 钢琴窗 / 自动化窗的
  「创建自动化 ▸」全部由 `addSlotAutomationItems` + `applySlotAutomationChoice` 生成，
  名字出现一次，参数（slotId、idBase）由调用方给。平铺回去就是把"加一种目标"变成"改五处"。
- **`PopupMenu` 的 id 空间是扁平的**，子菜单不占 id。新加一组 id 前先数一遍已用的区间
  （`10..13 / 20 / 30..41 / 200+n / 1000+ch / 9001 / 10000+`）。空白子菜单要给一条禁用项。

## 钢琴窗的时间线长度（P11 增长 / P60 回收 / P61 缩放同档）
- `timelineBeats` 是滚动的上界，`totalBeats = 64` 是最短长度，`beatChunk = 64` 是增长/回收的步长。
- **增长**：`ensureTimelineExtendsTo(beats)`，每次最多四块，一次离谱的调用不会造出五小时的标尺。
- **回收**（P60）：`trimTimelineToView()`，凡是 `clampScroll()` 或 `setScrollBeats()` 跑过都会问一遍
  "现在还需要多长"。三个锚点一个都不能少：**内容末 + 4、`scrollBeats + visible`、`playheadBeat + 4`**。
  少第一个会把音乐截掉，少第二个回滚会把视野拖走，少第三个标尺会比播放头短、播放头点不到。
  结果同样按 `beatChunk` **向上取整**——落在非块边界上的长度会让下一格滚轮立刻再长一次，标尺会呼吸。
- 不变量：**任意时刻 `timelineBeats == max(totalBeats, ceil(needed / beatChunk) * beatChunk)`**。
  滚进空白 → 长；滚回来 → 短，滚动条滑块宽度自己恢复到移动之前（用户 P60 的原话）。
- **缩放（P61）也走 `clampScroll()`**，而且必须用**夹过下界**的 scroll 去问增长：
  `wanted = jmax(0.0, scrollBeats) + visibleBeats`。缩放的锚点常在 beat 0 左边，`scrollBeats`
  此刻是负数、两行后才被夹回 0；用原始负值求和会把需求算小，于是"这一档不涨、下一档才涨"，
  中途右边一条没网格的死带。见 `PITFALLS.md` P61-A。
- 缩放回缩时同一条路径回收：`visible` 变小 → `needed` 变小 → 长度回落到 `beatChunk` 的整数倍，
  滑块宽度跟着恢复。**不需要**在 `setBeatWidthAround` 里另写一份。
- 只回滚一半的典型 bug：`setPlayheadBeat` 的跟随分支直接写 `scrollBeats`（不经过这两个口子），
  它**故意不回收**——播放头还在那儿，标尺不该在它脚下缩。位置回滚由 P58 的 `userScrollBeats` 负责。

## 分栏（P25：绝对位移）
- `VerticalSplitter` **不传增量**：`onDragBegin` 记起始高度，`onDragTravel(totalTravel)` 用
  `e.getScreenY() - 锚点`；增量会把被 clamp 掉的越界量永久丢掉。组件自己会动，必须屏幕坐标。
- 回调一律 `height = 起始高度 - travel` + `resized()`，clamp 留在 `resized()`；自检走
  `simulateDrag(travel, continueGesture)`。三个实例：自动化窗 / 文件栏 / 编组栏。

## 混音台（P23 / P25 扩 127 / P27 推子让宽）
- **主输出在显示槽位 0**，`displaySlotFor/channelForDisplaySlot` 是唯一映射。
- **容量 ≠ 默认**：`numChannels = 127`（= `masterChannel` 哨兵），`defaultChannels = 8`。
  发布与排序只跑 live 通道；插入链按需准备（`chainsReady` 只增不减）。
- **通道条固定宽** `stripWidth = ui::px(92)`，溢出时底部手绘滚动条。
- **主输出不是通道**：无删除项、无输出下拉；有 MONO（`masterMono`），引擎在**主推子斜坡之后**
  折叠——那是信号离开声卡的唯一位置。
- **P70：主输出也没有 SOLO**。`toggleSolo()` 早就对 master 直接 return，但绘制、hover、右键
  还都把它当按钮——结果是一个永远点不亮、光标却会变手型的死键。现在四处一起排除
  （`drawStrip`、`hitTest`、`mouseDown`、右键菜单）；M 是真的（静音整个混音）。
- **P70：删条之后选中跟着"行"走**。删掉选中条**上方**的一条，`selectedChannel` 要 `-1`——
  只 `jlimit` 会让选中静默落到隔壁那条上，机架标题和插入编辑随即打到用户没看的通道。
  删掉选中条本身则保持索引（下面那条顶上来的就是同一行）。
- **P70：选中值夹到 `channelCount`，不是 127**。`setSelectedChannel()` 的上界是**活通道数**
  （master 例外，它永远存在）；`mixerChanged()` 再兜一层，通道数缩小时把选中拉回来。
  否则从 24 通道的工程切到 8 通道，选中还停在 22。
- **P70：推子命中区不能吃电平表**。`getFaderHitBounds()` 的横向扩展取 `min(ui::px(8), gap/2)`，
  gap 是推子与电平表之间的 4 px。平扩 8 的话，最后 4 px 归电平表，点它会把推子跳到那个位置。
- 通道路由只写在 `getChannelOutput/setChannelOutput/wouldLoop`：默认全 `masterChannel`，
  环路在模型层**拒绝**，`sanitiseOutputs()` 在缩通道时回落。
- **单声道监听**：`channelMono` → `ChannelMix::mono`，引擎在**插入之后、求和之前** (L+R)/2。
- **删除通道**：地板 `deletionFloor = 8`（`canRemoveChannel()` 唯一判据，`－` 标灰不隐藏）。
  `removeChannelAt(i)` 的 `inserts`/`channelMono`/`channelOutputs` **三个数组都要下移**；
  机架发送重编号在 `MixerPanel::deleteStrip`（先拔 `==i → -1`、再 `>i → --`）。
- **立体声电平表**：`channelMetersL/R` + `getChannelPeakSide(ch,side)`；`stereoMeterHasOwnPixels()`。
- **推子/表共用一行**：`getFaderRowBounds()` 切左半给推子（下限 16 px）、右边全给表（约 37 px）。
  两者必须同源派生；`faderYieldedHalfTheRow()` 一个断言钉两件事。
- `SynthSlot::maxMixerChannels` 必须等于 `MixerModel::numChannels`（`static_assert`）。
- 机架卡片发送子菜单 id 基址 `kSendToChannelBase = 1000`（`21 + ch` 会撞 30–40）。
- **效果器机架可换位置**（P43）：`MixerModel::moveInsert(channel, from, to)` **轮转不是交换**
  （交换会把目标槽的插件甩到出发点，用户无法靠再拖一次还原）。
  UI 两条路：拖插槽名上下移动（`pressedInsertSlot` + `mouseUp` 才决定是"拖动"还是"点菜单"）、
  菜单里的「上移一格 / 下移一格」（id 103 / 104）。
- **通道条四个控件各一个菜单**（P43）：`MixerPanel::StripControl{fader,pan,mute,solo}`，
  `fillControlMenu` 只放"复位（或开关动词）+ 那一行自动化"，**不再挂通道菜单**。
  宿主侧 `fillStripAutomationItems` / `applyStripAutomationChoice` 决定"推子=level lane"。

## 编辑手势（P22 起）
- **双击 = 打开**（P78）：音符双击开包络检查器（`onNoteDoubleClicked` → `NoteEnvelopePanel`），
  采样块双击开它的包络窗（`onSampleDoubleClicked` → `SampleEnvelopePanel`）。采样这边有**两档**：
  chevron（P49）开贴着块的内联窄条，用来"调一下就走"；双击开窗口，用来"这是什么、哪条曲线、去哪"。
  开窗会先 `closeSampleAutomation()`，同一根曲线不留两个编辑器。三条边界：①**顺序照绘制顺序**——
  `drawSamples` 在 `drawNotes` 之前，块画在音符下面，所以采样那一问排在 `hitTestActiveLayer` **之后**；
  压在块上的音符先回答。②**闭组里的块不开窗**：那一击仍是「让我进去」（P73），进组之后再双击才开。
  ③ghost 块换层之后，非成员要连窗一起开（P77 的对称）。
- **ghost 挡单击、不挡双击**（P77）：`getEditableNoteIdAt()` / 所有手势命中都只看活动层
  （ghost 点穿，P1/P20 不变）；`handleDoubleClick()` 末尾加一问 `hitTestGhost(pos)`，
  命中就 `setActiveLayerIndex()` 换层，再按 P74/P73 收尾——组里的成员 `selectGroup(gid, occ)`
  + `enterGroupEdit(gid)`，不在组里就选中它自己**并触发 `onNoteDoubleClicked`**（打开它的包络
  检查器）——那正是同一个音符在自己那一层上双击会得到的两个结果，少做一半就变成两套语义。位置在**活动层全部命中之后**，因为 ghost
  画在所有东西下面；`hitTestGhost` 内部倒序扫非活动+可见层（`drawNotes` pass 1 是升序画的，
  后画的在上），层内先音符后块（块画在音符下面）。换层本身会让 `layersChanged()` 清掉旧选区
  并关掉旧组，所以"先换层、再选"的顺序是必需的。
- **打开不是选择**（P76）：`handleDoubleClick()` 进组只改 `editingGroupId`，**不重写选区**。
  `if (! isSampleSelected (clip->id)) setSampleSelection ({ clip->id });`（音符侧同构，
  用 `isSelected`）。无条件的 `setSampleSelection({clip->id})` 会把双击第一击刚选好的
  「整次出现」收窄成一个成员，之后拖高度/长度只动一块——用户看到的是"要动两次其他才
  联动一次"。这条与「按在已选中的东西上不能替换选区」（`handlePress` 的
  `else if (! isSelected)`、P65 的组名 pill）是同一条规则在"打开"上的版本。
  想让"只留这一个"，正确入口是点空白（P75）再点它。
- **按下归谁：已选中的块优先于"画在最上面的"**（P75）：`clipEdgeAt()` 在四条边都没
  命中之后，再倒序遍历一次已选中的块，点在谁的矩形里就回 `(那块, none)`。
  `none` 是"抓住了块、没抓住边"，`handlePress` 因此保留选区并 `armSampleMove()`。
  没有这一步时，两块叠在同一拍同一行（粘贴叠到自己身上是最常见的来历）的按下会落到
  上面那块**没被选中**的，选择被 `setSampleSelection()` 换掉，多选只剩一个动。
  它只对"已经在选区里"的块生效，所以"点一个没选中的块 = 只选它"没有被改掉。
- **框选 = 命中 + 组扩散，Shift/Ctrl 是"加"**（P74）：`simulateMarquee` 先 `hitsInMarquee()`
  再 `expandToWholeOccurrences()` 再写选择。`marqueeAdditive` 在 `mouseDown` 的空白分支就记下
  （松手时修饰键早就抬起来了），加选时旧选择**排在前面**，列表顺序才不会每次都重排；
  Shift 空扫不清空——"这里什么都没有"和"忘掉一切"是两件事。`simulateMarquee` 是松手时唯一
  的入口，`mouseUp` 里两条路径都从这里走。
- **多选拖动是刚体**：唯一实现 `clampMove()`，夹的是 **delta** 不是每个音符；吸附只作用在块上，
  锚 = 最左成员。成员含采样（`clips` 参数）。
- **应用只有 `applyMove()`**：音符 + 已武装的采样一起走。
- **拉伸只作用于被抓住的那一个**：`dragResizeNoteId` + `resizeTargetId()`，数学只有一份
  `applyNoteResize()`。循环改整个选中会把尾巴拉成一条线。
- **音符两端都能拉伸**：`kResizeHandleWidth` 是设计值、处处 `ui::pf()`。
- **选中 ≠ 悬停**：选中 = 主体变亮 + 2 px 白环 + 四角括号；悬停只有 1.2 px 淡环。
  聚光灯的实芯环会盖住选中环 → `drawFocusHighlights` 对**已选中**音符只画光晕和左耳。
- **琴键可弹**：键盘区分支排在标尺/网格之前，用 `routing::layerSlotId()`。
- **菜单栏自绘**：`shortcutKeyDescription` 画快捷键，`kCommandKey` 分平台；新增命令要同时改
  `fill*Menu` 和 `keyPressed`。
- **P54：菜单标题只有一张表** `MainComponent::menuTitle(i)`，`numMenus = 3`（文件 / 编辑 / 工具）。
  标题曾在 `paint()` 里用三元链写死，加第三个菜单时要改四处；现在 `getMenuBarItemBounds` 的
  `widths[numMenus]` 与 `menuTitle` 是唯一的两个地方。
- **P54：工具菜单不实现工具**。9 项全部调工具条同一个方法（`setTool` / `copyNoteStateFromSelection`
  / `groupSelection` / `ungroupSelection` / `setSnapEnabled`），置灰也读同一个 `hasAnySelection()`。
  菜单里另写一份判断，就是"按钮能用、菜单不能用"的开始。模式项用 `isTicked`（平台唯一画得一致的勾），
  节头写当前模式名。
- **已选块的边缘优先于命中拾取**（P58）：按下块时先问 `selectedTrimEdgeAt(pos)`——"这一像素是不是
  某个**已选中**块的 trim band"，是就以那一块为准，并且**不改写选区**；只有它答 -1 才用
  `hitTestSample()` 的结果。`hitTestSample` 同时改成**倒序遍历**（最后添加 = 画在最上层 = 先命中），
  与绘制顺序一致。两条一起才治好"选中几块拖长度有时失败"：以前抓到的是盖在上面的邻居，
  然后选区被改成邻居。自检走 `getBlockGrabAtForTest()`（顺序唯一），别只测 hitTestSample。
  `mouseMove` 里同步：`selectedTrimEdgeAt(pos).first >= 0` 就显示左右箭头光标。
- **band 只在块内成立**（P59）：`selectedTrimEdgeAt` 现在要求 `left <= x <= right`，还要
  `y` 在块内——少了第一半，`x >= right - band` 就是"块右边一切像素都算这块的尾巴"，
  `x <= left + band` 对称地算"左边一切"。多选三块相邻时按中间的尾巴，倒序遍历先问右边那块，
  它的头带对左方一切成立 → 返回"右块的头"→ 拖头被 source offset 夹住 → 什么都没动。
  命中之后按**距离最近**的边定 tail/head（`toTail <= toHead`），平局让最上层赢（严格小于）。
  它不是"多选专用"：单块也走这条路，只是以前恰好被 `hitTestSample` 的包含判定兜住了。
- **多选变高：全部写完再同步，不是写一块同步一块**（P75）：`applySampleSpan()` 的
  组同步由 `propagate` 参数控制，`applySampleSpanDrag()` 传 false，循环里只写各自的
  绝对值（快照 + delta），最后对涉及的每个 `(groupId, occurrence)` 各 `syncGroupShape()`
  一次。边写边同步的写法在两处出现**形状本来不一致**时（老工程、撤销回来的快照）会
  轮流把对方当模板：第一块的同步改掉第二块的当前值，第二块再拿自己的快照加 delta 写回，
  最后停在指针没指过的那个高度。自检：`60+5 / 60+7`（错）对 `60+5 / 60+5`（对）。
- **多选拉伸只算一次 delta**（P56 起）：`applySampleTrimDrag(primary, wanted, fromTail, siblings)`
  先按主块算出实际 delta（最小长度 / 源偏移会吃掉一部分），再把**同一个 delta** 按各自限位套给
  兄弟块。所以抓哪一块的边都等价——这也是 P59 那条拾取 bug 表现为"按了没反应"而不是
  "只有一块变长"的原因：主块选错，delta 算出来是 0。
- **播放跟随后退**（P58）：跟随曾是单向的——播放头推出屏幕就跟着滚，退回来却不回原处。
  现在 `setScrollBeats` / `setBeatWidthAround`（用户自己滚）会 `noteUserScroll()` 记住
  `userScrollBeats`；`setPlayheadBeat` 开头先判断"播放头是否落回 `[userScrollBeats, +可见宽度]`"，
  是就 `scrollBeats = userScrollBeats` 并清 `followScrolled`，否则才走原来的跟随分支
  （首次推走时 `if (!followScrolled) userScrollBeats = scrollBeats`）。

## 编组
- **组编辑态下，组框里的空白就是空白**（P75）：`getGroupFrameAt()` 命中之后，只有
  `frameGroup != editingGroupId` 才 `armGroupMove()`；编辑中的那个组要**落下去走 marquee**
  ——那一击于是清空选择（`mouseDown` 的空分支本来就这么做），单击不动指针时选择保持清空。
  以前组框把这一击整个吃掉，"在组里取消选择"变成了"重新选中整组并起拖"。
- **选中一个成员就是选中整组**（P74）：选择是"就是这些东西"的那句话，不是"我点到的那一个"。
  三个入口把成员扩成整次出现，一个都不许漏——
  点击走 `armGroupMove()`（它本来就会 `selectGroup()`），框选走
  `expandToWholeOccurrences()`（`simulateMarquee` 里、写选择之前），Shift/Ctrl 加选走
  `addOccurrenceToSelection()`（`armGroupMove(..., additive=true)`）。
  漏了框选那次，框切到七个音符里的一个只回来一个，下一次 Delete 就从队形里撕掉一个成员——
  而"不许撕队形"正是关闭状态存在的全部意义。
  **扩散只写在手势层，绝不写进 `setMixedSelection()`**：底层写入必须保持精确集合，P62 的
  "改一部分才同步形状"（`collectTwinsForEdit` 拿整处判据决定要不要复制给兄弟）全靠它。
  `isGroupLocked(gid)` 是唯一闸门——打开编辑的组不扩散，那里成员就是对象。
- **默认是对象，不是容器**（P73）：没进入编辑时，组里任何成员的按下都只回答"整个出现"——
  `armGroupMove()` 选中整组并起拖，缩放带、橡皮、刻刀、右键菜单一并改道组菜单。
  双击成员 / 组框 / 组名 pill 才 `enterGroupEdit(groupId)`，之后点谁选谁，可以单独移动、改长度、
  删除。双击空白 `exitGroupEdit()`；点到组外的成员（含别的组）当场退出。
- **状态只有一个 `editingGroupId`**（P73）：组 id 全会话唯一（`groupIdCounter()` 静态），所以不另存
  layer。判定只有 `isGroupLocked(gid)`（`gid >= 0 && gid != editingGroupId`）。
- **失效判据写在 `layersChanged()`，且无条件**（P73）：撤销/加载会把同一个组 id 带回来，
  "这个组还在吗"会答 yes，然后把用户没双击过的组悄悄打开。`notesChanged()` 另有一条按存在性清的
  自愈（对应 `pruneEmptyGroups`），它只在组真的消失时才生效。
- **`mouseDown` / `mouseDoubleClick` 的实体是 `handlePress` / `handleDoubleClick`**（P73）：
  参数是 `(Point<float>, ModifierKeys, int clicks)`，自检走 `simulatePressForTest` /
  `simulateDoubleClickForTest`。事件里的整数点单独取 `ipos = pos.roundToInt()`——JUCE 的
  `Rectangle<T>::contains` 只吃 `Point<T>`，两种点不能互相隐式转换。
- **`clicks >= 2` 的空白按下由 `handlePress` 先吞掉**（P73）：`mouseDoubleClick` 在那之后才跑，
  不吞的话铅笔会在"退出编辑"的同一次双击里再画一个音符。
- **视觉：外圈虚线**（P73）：编辑中的组在实线框外 3 px 再画一圈 `drawDashedLine`（dash 4/3.5），
  pill 实心 + `surface1` 描边。不用加粗，加粗已经是 hover/focus 的语义；不用四角角标，
  组名 pill 长在左上角会吃掉一个，组越短越看不见。
- **内联曲线框**：框的左右 = **组的起止拍**，`getGroupCurveBounds()` 水平零内边距；X 轴是组自身
  跨度、Y 轴 0..1。打开即 `setEnabled(true)`；**与音符内联框互斥**。
- **组要能拖起来**：唯一入口 `armGroupMove(groupId, occurrence, pos, additive = false)`。
  `additive` = Shift/Ctrl（`isAdditive(mods)`，与采样命中用同一对修饰键）：
  整处还没在选区里就把**整处**加进去而不是替换掉——"选三个组"不能要三次单独的拖拽。
  两条路：点标签 `getGroupLabelAt`、
  点框内空地 `getGroupFrameAt`（只认激活图层，排在音符命中之后、marquee 之前）。
  **P65：这一处若已经整处在选区里，就不调 `selectGroup()`** —— 多选后点其中一个组的标签，
  意思是"搬走我选的东西"，把选区缩成这一个组会让其余组在指针动起来的瞬间掉队。
  抓取也从"这一组的块"放宽到"所有选中的块"（`captureGroupClips()` 现在就是
  `captureSampleDragOriginals()`），所以两组各自的采样块一起平移。
  整处判据是 `isWholeOccurrenceSelected(gid, occ)`，不是 `isWholeGroupSelected`（后者问整个选区，
  多选时永远 false）。
- **名字标签不能比组宽（P66）**：`getGroupLabelBounds()` 的文字宽度再和组框宽度取 `jmin`。
  标签最小 34 px，而一个 0.1 拍的组在屏幕上只有 5 px（`getNoteBounds` 的 5 px 下限），
  不夹的话标签会横出组框压到隔壁内容。宽度不够 `12 + 6` px 时 `getGroupAutomationHandleBounds()`
  返回空——曲线把手不画，也别在标签外面画。
- **采样是组成员**：成员计数/`getGroupContentBounds`/组拖动/`pruneEmptyGroups` 都要同时看
  notes 和 samples。**P56 起复制粘贴也带采样**：整组（音符 + 块）被选中才算"整组"，
  `pasteEditClipboard()` 见到 `! groupClips.empty()`（P63 之前是 `groupClip.valid`）直接走
  `pasteGroupFromClipboard()` —— 一个锚点、一次事务。以前是两趟（音符建组、采样各粘各的），
  结果是"组 + 旁边散着的音频"。
  `cutNotesToClipboard()` 也必须把块一起从时间线上删掉，否则剪切→粘贴会复制出一份没被搬走的音频。
- **组 = 一个定义 + 多次出现**（P58 的模型核心）：`NoteGroup` 是**定义**（name / 颜色 / 挂载 /
  trim / 曲线），时间线上的每一份是它的一次**出现**。成员身份 = `(groupId, groupOccurrence)`，
  `NoteEvent` / `SampleEvent` 各带一个 `groupOccurrence`（默认 0 = 老工程行为）。
  - 两处出现**各自独立移动**，共享同一条曲线、同一份 trim、同一个挂载。
  - `assignNotesToGroup(ids, gid, occurrence = 0)` / `assignSamplesToGroup(...)` 写出现号；
    `getGroupOccurrences(gid)` 是派生出来的升序列表（不存字段）；`nextGroupOccurrence(gid)` 给新出现的号。
  - **凡是比较 `groupId` 的地方，都要再问一次 occurrence**：`getGroupContentBounds`、
    `captureGroupClips`、`drawGroupBands/Frames/Labels`（改成双层循环，每处一个括号一个标签）、
    `isWholeGroupSelected`、`getSelectedGroupOccurrence`、`getGroupBeatSpan`。
    约定：**`occurrence < 0` = 该组所有出现**（"整组"），`>= 0` = 只那一次。所有新参数都带默认
    `-1` 或 `0`，老调用点行为不变。
- **粘贴 = 新增一次出现**（P57 起因 / P58 定案）：`pasteGroupFromClipboard()` 复用源 `NoteGroup`，
  但用 `nextGroupOccurrence()` 生成**新的出现号**——不是"往组里塞成员"。P57 的做法（借成员给源组）
  让副本和源共用一个括号，拖哪处都一起动，用户看到的就是"粘贴到了组的内部"。
  只有源组不在了（剪切榨干 / 另一图层）才新建一个同色同名的组。`GroupClip::sourceOccurrence`
  记住模板是第几次出现。
  **所有粘贴入口必须转发**：`pasteNotesFromClipboard()` 开头见 `groupClips` 非空就转给组路径
  （P63 之前是 `groupClip.valid`），否则它会以"散音符"的身份粘一遍，块留在剪贴板上
  （P56 的 bug 以另一种形式回来）。
  复用路径**不重放曲线的平移**：绝对时间的点留在原处，"同一个组"就是同一条曲线。
- **组 = 一个形状 + 多个位置**（P62）：出现拥有的是**位置**，其余（成员、音高、长度、力度、
  包络、采样块的宽/高/曲线/反向/伸缩模式）都是**组的**，改一处就是改组。
  - **位置是真的存下来的数**：`NoteGroup::occurrenceStarts`（occurrence → 起始拍）。
    只按"最早成员"推导不够——删掉那个成员，位置就会跟着走，之后每次同步都把这一处拖一次。
    所以 `Layer::occurrenceStart()` 第一次被问到时物化并保存，此后永不自动变；
    `setOccurrenceStart()` 只在**整处移动**时用。
  - **同源判据 = "整处 / 局部"**：整处动（拖、删）= 对这一份的决定，兄弟不动；
    动一部分 = 形状变了，兄弟跟着变。`wholeGroupOccurrencesInDrag()` / `isWholeOccurrenceSelected()`
    （P63 起；之前误用 `isWholeGroupSelected`）是同一个问题的两个入口。
  - **形状同步** `Layer::syncGroupShape(gid, source)`：按成员序配对，
    把源出现的相对位置 + 全部属性写进每个兄弟出现，锚点是兄弟自己的起点。多退少补。
    成员序 = `noteShapeLess` / `clipShapeLess`，**配对键只能写形状编辑自己不会改的字段**：
    起点 + 名字（block 侧，P72；以前第二键是 pitch，拖底边改 pitch 就把序号配对扰乱了，
    两个副本互换名字和音频）。音符侧仍是 start, pitch, id——同样的隐患还在，没改。
  - **离开形状**（删/取消编组）没有模板可抄，用 `Layer::collectTwinMembers()`：在原件还在时
    按序号配对，返回兄弟出现里对应的成员，由调用方删除或解组。判据同上：整处选中就跳过。
  - 同步挂在**编辑动作本身**，不挂在 mouseUp：`applyMove`（拖动/整组动）、`applyNoteResize`、
    `dragAutomationPoint`、采样曲线/trim/刀切/反向/伸缩模式/高度。mouseUp 那份是兜底，
    对内联框那种"没有拖动"的编辑尤其重要。
  - 建组（`groupSelection`）、粘贴/放一处（`insertGroupMembers`）、读工程（`materialiseOccurrenceStarts`）
    三处**必须把起点钉住**，否则第一次编辑就会用"删完之后的最早成员"当锚点。
  - `formatVersion` 24 → **25**：`<Group>` 下加 `<Occurrence index="n" start="beat"/>`，
    只在出现有起点时写。老文件没有这些元素，靠懒物化补上，打开即正确、不会移动。
- **剪贴板 = 一组出现**（P63）：`groupClips` 是 `std::vector<GroupClip>`，每个元素是**一处完整出现**，
  并且**自带成员**（`GroupClip::notes` / `::clips`）。`noteClip` / `sampleClip` 只装
  **不属于任何整处**的散音符/散块。
  - **复制**：选区按 `(groupId, occurrence)` 切桶（`std::map<std::pair<int,int>, …>`），
    整处选中的桶变成一个 `GroupClip`（起点用 `Layer::occurrenceStart()`，即 P62 钉住的那个数），
    其余落散。`copyNotesToClipboard()` 一次填三个剪贴板；
    `copySelectionToClipboards()` **不再**追调 `copySamplesToClipboard()`（那会把整处已认领的块
    重新填回 `sampleClip`，粘贴时贴两遍）。
  - **判据 `isWholeOccurrenceSelected(gid, occ)`**：成员数从**模型**里数、选中数从**选区**里数，
    相等且 > 0。**不要拿 `isWholeGroupSelected(gid, true, occ)` 当它用**——那个函数循环整个
    `selection`，选区里有第二个组就返回 false，多组复制/删除全都会走错分支（P63-A）。
  - **粘贴**：所有对象共用**一个锚点**（`clipboardFirstBeat()` / `clipboardLastBeat()`），
    delta 只算一次，所以两组的间距原样保留；每个对象各自 `nextGroupOccurrence()`，
    "同一组的两处一起复制"= 该组再来两处，而不是一处塞进双倍成员（P63-C）。
  - **剪切**删**所有选中的块**，不再按剪贴板内容判断——整处的块已经不在 `sampleClip` 里了（P63-B）。
  - 菜单"粘贴目标"里 `isGroupClipboard()` 必须排在 `canPasteNotes()` 前面，否则"只有音符的组"
    （两个松散列表都空）会显示"剪贴板为空"（P63-D）。
- **组框就是成员框，零外力扩展**（P57）：`groupFrameOf()` 返回 `getGroupContentBounds()` 本身，
  不再 `expanded(3px)`；绘制、命中（`getGroupFrameAt`）、自检读的是**同一个函数**。
  填充色块画在成员之下（`drawGroupBands`），**描边画在成员之上**（`drawGroupFrames`，在
  `drawNotes` 之后），并向内缩半个线宽——线压在块自己的边框上，形成"组框即块的外框"。
  这是用户要的"组边框与采样边框之间不留距离"：挪线做不出来，只能同一条边、画在上层。
- **编组栏是钢琴窗选区的镜像**（P59）：`PianoRollComponent::getSelectedGroups()` 返回
  `(layerIndex, groupId)` 列表——每个组、每个**出现**问一次 `isWholeGroupSelected(gid, true, occ)`，
  任一出现整体入选就算这个组被选中（选区只在激活图层，所以只需要看一层）。
  主窗体 `onSelectionChanged` → `syncGroupPanelSelection()` → `GroupPanel::setSelectedGroups()`，
  面板**没有**自己的 `selectedGroupId`（存一份就要在删除/撤销/切图层三处各自清理）。
  绘制：`cardlook::drawSelectionRing()`（P60，见下），**左竖条留给"喂给聚光灯音源"**——
  两个标记可以同时出现，所以必须长得不一样。
- **编组行可点选（P60）**：`GroupPanel::selectRow(i)` 是唯一实现，`mouseUp` 在"按下后没拖动"
  时调它。它做两件事——①把本面板的 `selectedRows` 乐观地设成这一行（没接线的裸面板也亮）；
  ②`onGroupSelected(layerIndex, groupId)` 交给主窗体 → `pianoRoll.selectGroup(groupId)`（整组）。
  选中仍然只有钢琴窗一个 owner，面板那一下会被 `setSelectedGroups` 覆盖成真相。
  **折叠不再是点击 body 的动作**：body 的点击含义被"选中"占用了，折叠只剩左侧 chevron
  （`Part::collapse`）和右键菜单两项。一个矩形上的一个按下事件只能有一个点击含义。
- **选中环只有一份（P60）**：`Source/UI/CardLook.h` 的 `cardlook::drawSelectionRing()`。
  机架卡（`RackPanel::drawCardRing`）、编组行都调它：外圈 `accent@0.22` 扩 3px 线宽 3，
  内圈 `accent@0.95` 缩 0.5px 圆角 7.5 线宽 1.8。用户要求"编组框的选中样式跟采样/VST 音源框
  一致"——一致性只能是同一个函数，三份同样的常数迟早会漂。
- **折叠行只写组名**（P59）：折叠分支以前把 `0.0 dB · C` 画在**和组名同一个矩形**里，于是每个
  折叠组背后都有一行幽灵字。现在 `if (collapsed) return;`——折叠 = 一行，那一行是标题；
  fader / 声相 / 挂载展开才有。
- **整组判定有两个口径**（P56）：`isWholeGroupSelected(groupId, includeSamples)`。挂载音源用
  false（块没有振荡器可挂，只看音符），复制用 true（少一个块就是半个组）。默认值 false，
  所以老调用点的行为不变。
- **组的"复制到后一小节"也是新增出现**（P56 起 / P58 改）：`duplicateGroupToNextBar(layerIndex,
  groupId, occurrence = -1)`，走模型直接克隆，**不经过剪贴板**（用户的剪贴板是他的）。
  现在**不再 `addGroupCopy`**：`copyId = groupId`、`newOccurrence = nextGroupOccurrence(groupId)`，
  空结果直接 return false（组不是我们建的，不能删）。目标 = `nextBarLineAfter(起点)`；若仍压在源上，
  改用 `barLineAtOrAfter(源结束)`——长组不会自己盖住自己。菜单项放在"选中整组"下面，节头带
  `· 第 N 处`，计数按次（`countNotes/SamplesInOccurrence`）。
- **编组栏的行能拖到钢琴窗放一处**（P58）：`groupDragDescription(layerIndex, groupId)` →
  `"atomic-group:<层>:<组>"`，**两个号**（组 id 只在层内唯一），解在 `groupDragTarget()`，
  两端都在 `Util/SharedRules.h`；解析失败不许改写入参。
  - `GroupPanel`：行 body 按下只 `dragMayStart = true`，`mouseDrag` 超 `ui::px(4)` 才
    `startDragging`，**折叠延后到 `mouseUp`**（`foldOnRelease`）——否则每拖走一次就顺手折叠一次。
    折叠箭头（`Part::collapse`）仍立即响应，它是控件不是把手。
  - `PianoRollComponent::placeGroupAppearance(layerIndex, groupId, atBeat)` 是唯一落点实现
    （drop 与自检都调它）：模板取该组**最早一次出现**（面板那一行说的是"这个组"，不是"它上次
    落在哪"），指针所在拍是**第一个成员**的落点，新出现号，零新组。预览画一条落点竖线 + 一个
    虚线跨度幽灵，标签「放一处 <组名>」。
- **P35：组是一个 trim，成员可以是两种东西**。`NoteGroup` 只存 levelDb / pan / muted，
  **成员列表不存**——`groupId` 在音符和采样块自己身上，所有"组里有几个""组里是谁"都是扫出来的。
  这仍是唯一模型：删除成员不会留下悬挂 id。
- **建组 / 选组 / 解组都认两类成员**（P35）：
  - `groupSelection()` = `assignNotesToGroup(selection)` + `assignSamplesToGroup(sampleSelection)`，
    G 键条件与右键菜单项都放宽到"选中任一即可"；菜单文案「编组选中内容」。
  - **P38：工具条按钮的门是 `hasAnySelection()`**（`!selection.empty() || !sampleSelection.empty()`）。
    P35 只改了 `groupSelection()` 和 G 键，按钮的点击门与绘制门还是 `!selection.empty()`，
    于是"只选采样 → 编组按钮灰着、点了没反应，但按 G 能建组"。现在四处（两次点击、两次绘制）
    加 Ctrl+D、G 键共用这一个谓词，自检直接断言谓词。
  - `selectGroup()` = `setMixedSelection(notes, clips)`。**这是唯一能同时写两个选区的入口**；
    `setSelection` / `setSampleSelection` 互相清空（点击语义：一次点击只抓一种）。
  - `ungroupSelection()` 两类都清，清完 `pruneEmptyGroups()` 才真的把组删掉。
  - `getSelectedGroupId()` 混合选区要求两半同组，否则 -1。
- **组的音量/声相/静音作用于采样块的音频**（P35）：`GroupMix` 冻结 level / pan / **muted**
  （`gainAt()` 里静音直接返 0，音符和块共用一份，不可能各说一套）。引擎里块的循环用
  `groupBlockFor(clip.groupId)`，与音符同一份数据；块的 pan 是 **balance**（立体声文件自带
  声像），音符是 constant-power，两者不混。
- **组永远不决定采样块的路由**：`routing::resolveSample` 不看 `groupId`。重新挂载组不会换掉
  某块脚底下的音频文件——那是 P27 定下的，P35 没动。

## 机架采样卡的"拉伸"控件（P51）
- 卡片读数行右端一颗 chip（`RackPanel::getStretchBounds`，与 SAMPLE pill 同行同高、
  右对齐——那是合成器卡"越级"徽章的槽位，采样器永远不画它）。label 显示该槽位全部块的
  共同模式：原速 / 只变速 / 只变调 / 变速变调；不一致 = **混合**；没有块 = 拉伸（无勾选）。
- **模式是块的属性，卡片是音源的界面**：所以读和写都走宿主回调——
  `samplerStretchState`（→ `MainComponent::samplerStretchStateFor`，遍历层找
  `outputSlotId == slotId` 的块）与 `onSamplerStretchChosen`（→ `applySamplerStretchToSlot`，
  `findSample` 逐块写 + **一次** `notifyNotesChanged`，撤销一步、徽标与 schedule 一起走）。
  面板自己看不见层，别给它加 LayerStack。
- 菜单含全部四个模式（含 原速，那是"改回去"的路）；选哪个就写到该槽位的**所有块**。

## 机架卡片拖拽（P52 采样卡 / P53 双卡 / P58 组行）
- **三种 payload，都在 `Util/SharedRules.h`**：卡片两种前缀（下面详述），P58 又加了
  `atomic-group:<layerIndex>:<groupId>`——**两个号**，因为组 id 只在层内唯一。
  编解码成对出现（`groupDragDescription` / `groupDragTarget`），解析失败不许改写入参，
  这样"拖到一半组被删了"是"落点找不到"而不是野指针。
- **两种卡是两个问题，两个前缀**（`SharedRules.h`）：`atomic-sampler:<id>`（交出文件）与
  `atomic-instrument:<id>`（交出挂载）。拼与解都在 `dragpayload::describe / idOf` 一处；
  接收端**不许**回头查槽位身份来分辨——拖拽途中槽位可能被删。
- **所有卡都能拖**（P53）：`RackPanel::mouseDown` 卡片正文 arm（`dragMayStart`），
  mouseDrag 超过 4 px 才 `startDragging`；拖影 `makeCardDragImage` 一份代码，tag 行区分
  SAMPLE / VST3 / SYNTH。**注意**：有选区时点卡片是 P23 的"挂载选区"，那个分支直接 return，
  不起拖——拖卡是"无选中"时的手势。
- 钢琴窗分派（`itemDropped`，**先重新跑 `updateCardDrop` 再分派**，预览与落点不可能各测各的）：
  - 音源卡 → 只认音符：`routeNoteToSlot` → `routeSelectionTo`（音符级 override）。落空 = 什么都不做。
  - 采样卡 → 块上 = `replaceSamplesWithSlot`（P32 `mountSelectedSamplesToSlot` 同一规则：
    name/path/length/offset 跟新文件，`startBeat` 不动；已在该槽的块是刻意 no-op，不重 trim）；
    空格 = `addSampleFromSlot` 新块（P52 行为不变）。
- **选中规则**（两个方向一致）：命中目标已在选区内 → 整个选区跟着变；否则只选目标。
  落在单个音符上永远走音符级 override，不走 `mountSelectionTo` 的整组挂载（P24 教训）。
- 预览 `drawDropPreview` 按 `dropKind` 分派：instrumentCard 高亮音符 + 指针旁 tag「挂到 X」，
  空处画「拖到音符上以挂载」；sampleCard 命中块 = 虚线高亮 + 「替换为 X」，空格 = 落块脚印。
- 测试钩子：`simulateCardDrop`（真 `itemDropped`，SourceDetails 只差真鼠标）、
  `simulateDropPreview`（摆预览，空串清除）；截图 `-dropnote` / `-dropsample`。

## 刻刀（P51：工具条第五个工具）
- `Tool::knife`，按钮 index 4，图标是**美工刀**（橡皮同款旋转圆角矩形 + 刀尖三角 + 折断槽；
  第一版"细三角+虚线"16 px 下就是一道斜杠，见 PITFALLS P51-C）。光标 = 十字线：切口发生在
  点击点上，十字中心比刀尖图更精确。状态/编组/解组/磁铁顺移到 index 5–8。
- **一刀两用**：点块走 `splitSamplesAt ({id}, beat)`，点音符走 `splitNotesAt ({id}, beat)`。
  两个函数**内部不吸附**——刻刀在 mouseDown 处按磁铁开关决定 snap；播放头路径照旧精确切。
  严格内部判定与块相同（切在边上 = 什么都不发生）。
- **切分即选区**：切完两半就是当前选择；两条曲线（音量/声相）与音符包络都按同一接缝
  `subEnvelope` 切分，ramp 不会在缝上重新开始。音符切开后**两半同组**——切开组成员是
  变成两个成员，不是一个成员加一个外人。
- 右键菜单的"在播放头处切割"已删（工具语义不该长在块菜单里）；反转、修剪、拉伸、挂载照旧。
  `splitSelectedSamplesAtPlayhead` 保留为测试钩子（P50 断言还在用它）。

## 音源卡片（P9 VST3 / P38 命名与界面）
- 一个槽三种身份：`isSampler()` / `isPlugin()`（`pluginId > 0`）/ 其余是内置振荡器。
  pill 依次写 `SAMPLE` / `VST3 · 插件` / `waveformName()`。
- **卡片标题 = `SynthSlot::getDisplayName()`**：插件时返回 `pluginName`，否则 `name`（"Synth A"）。
  **派生，不写回**——只有派生才可能让 `replaceWithNative` 把原名还回来。读名字一律走
  `RackModel::getSlotName()` 或 `getDisplayName()`，**不要再直接读 `slot->name`**。
  "替换成 VST3 看起来没成功"的根因就在这里：pill 变了，标题还是 Synth A。
- **打开插件界面**：卡片右键第 41 项 → `openPluginEditor(pluginId, getDisplayName())`，
  与混音台 insert 共用 `PluginEditorWindow`（一实例一窗口，实例卸载时窗口自毁）。
  enabled 条件 = `plugins.isLoaded(pluginId)`；没载入时文案写「· 未载入」而不是把项藏掉。
  P42 起窗口尾部 `setKeyForwarder(&pluginForwarder)` —— 前台时的空格与电脑键盘音符靠它。
- **双击卡片打开界面**（P85）：`RackPanel::handleDoubleClick` 里先问名字——**名字那块是改名**
  （P79），**其余部分**才 `onCardDoubleClicked(slotId)`。面板不解释"打开"是什么意思（它没有插件
  宿主），宿主侧 `MainComponent::openSlotEditor(slotId)` 回答：非插件槽 / 实例没载入 → `false`
  且不弹窗，否则 `openPluginEditor` 并返回 `true`。菜单第 41 项与这个手势共用同一个函数，
  「菜单和手势一致」因此是代码的结论，不是两条分支各自记得。
  排除区在 `RackPanel::isOverCardControl`：caret / × / S / M / 波形 pill / 只变调 / 拉伸 /
  → 输出 / 两个推子的 `expanded(0,6)` 带。**折叠卡的 caret 要单独先测**——折叠后整张卡都是
  标题行，`collapsed` 分支一律返回 `false` 会把 caret 一起放行。
- **卡片右键的「创建自动化 ▸」**（P42）列出该槽三样东西：`滤波器` / `音量` / `音源参数 ▸`；
  「音源参数」展开后是该插件自己的参数表（下标直通 `pluginParam`），内置振荡器没有这一段。
- 替换 = `replaceWithPlugin` / `replaceWithNative`，**槽 id 不变**，路由 / override / 自动化全保留。
  两者都返回被换下来的旧实例 id，调用方接 `reconcilePlugins()` 收尸。
- 替换后不用手动 republish：`AudioEngine` 自己是 rack 的 listener，`rackChanged()` → `republish()`。
  这条不成立时"换音源没声音变化"就会回来。

## 钢琴窗的文件布局（P85：一个类，十一个 TU）
拆的是**文件边界**，不是类：`PianoRollComponent` 的 316 个成员函数按主题分开定义在
`Source/UI/PianoRoll*.cpp` 里。C++ 允许一个类的成员函数分散在多个翻译单元，所以不需要
friend、不需要改一行头文件、也不存在"两个对象互相问状态"的问题。

| 文件 | 管什么 |
|---|---|
| `PianoRollComponent.cpp` | 构造/析构、三个 listener、坐标与网格、滚动与时间线、工具与光标、试听、走带 |
| `PianoRollInput.cpp` | `mouseDown/Drag/Up/Move/Exit`、`handlePress`、`handleDoubleClick`、`keyPressed`、两处 `hitTestActiveLayer` |
| `PianoRollPaint.cpp` | `paint` 与全部 `draw*`（工具条、标尺、键盘、音符、组框、播放头、跑马灯） |
| `PianoRollNoteEdit.cpp` | 音符增删改、拖拽夹取、形状同步、进/出组编辑、`simulate*` |
| `PianoRollSelection.cpp` | 选择集、路由、编组归属、三个菜单（组/画布/音源参数） |
| `PianoRollAutomationNote.cpp` | 音符自动化框（几何、命中、两个下拉、绘制） |
| `PianoRollAutomationGroup.cpp` | 编组自动化框 + 曲线剪贴板（`readSelectedCurve`…`dropCurveSelection`） |
| `PianoRollAutomationSample.cpp` | 采样自动化框 |
| `PianoRollClipboard.cpp` | 音符/采样/编组的复制粘贴、`GroupClip`、`duplicate*` |
| `PianoRollSamples.cpp` | 采样块编辑：窗口、裁切、拆分、反相、拉伸、挂载、删除 |
| `PianoRollSampleDraw.cpp` | 采样块绘制、拖放预览、采样块菜单 |
| `PianoRollInternals.h` | **唯一**的共享层：调色板别名、菜单 id 段、`boldFont` / `groupLabelText` / `curveCellFitsText` / `addCurveItems` / `neutralCurveValue` / `uniqueGroupName` 等 |

- 共享层放在 `namespace atomic::roll`，**每个定义都 `inline`**，每个 TU 顶部一句
  `using namespace roll;`。链接器合并 inline 定义，所以十一份 include 只有一份代码。
  **不要退回匿名 namespace 写进头文件**：那是每个 TU 各一份副本，且 MSVC 会为没用到的
  静态函数准备 C4505 警告位。
- 仍然留在各自 cpp 里的匿名 namespace：画笔/橡皮光标图（只有 core 用）、
  `membersDescription`（只有剪贴板用）、`subEnvelope`（只有 `splitSamplesAt` 用）。
  **判断标准是"几个 TU 用"**，不是"它看起来像工具函数"。
- 新增一个函数时先问：它在哪个主题里？跨主题才进 internals。**放进 internals 的代价是
  改它要重编十一个 TU**（实测约为单文件的三倍），所以宁可留在主题文件里。
- 改一个主题文件 ≈ 1,000–1,700 行重编（实测 1,028 行 ≈ 22 秒，JUCE 模板的实例化占绝大部分），
  改 internals ≈ 十一个全编。这就是这次拆分唯一的收益，别把它再长回去。

## 采样包络窗（P78）
- `SampleEnvelopePanel`，452×314，和 `NoteEnvelopePanel` 同尺寸同一套行距；浮动 child，
  由 `MainComponent::openSampleEnvelopePanel()` 贴着块摆放再夹进窗口。
- **曲线编辑复用 `EnvelopeEditor`**：`setEnvelopeSource` 每次 `sample->activeCurve()`，编辑的就是模型；
  切「音量/声相」写的是 `SampleEvent::automationTarget`（P50 的字段，和内联框、工程文件共用）。
- **两条曲线的中性点不同**：音量 1.0、声相 0.5。预设「直线」按目标给不同的平地值，
  `setCaptionName` 在右上角报出是哪条 + 中点值。
- 开窗即启用曲线（与内联框同一条规矩）；换层、块被删、工程加载都会自己关。
- 输出行写 `outputSlotId`，由 `routing::resolveSample` 解释（不自己算路由）；badge 三态：
  采样器 / 跟随图层 / 采样器失效。

## 改名（P79：一份数据，三处入口）
- **三个名字各有唯一字段**：`Layer::name`、`NoteGroup::name`、`SynthSlot::name`。面板、钢琴窗
  组框标题、菜单里的组名、混音台的轨名全都每次读同一个 getter——所以"改完别处跟着变"没有同步
  代码可写，写了就是错的。
- **入口只有双击那行名字**：`LayerPanel::beginRenameLayer` / `GroupPanel::beginRenameGroup` /
  `RackPanel::beginRenameSlot`，共用 `UI/NameEditor`（位置取各自 `getNameBounds(index)`，与绘制
  同一份数字；回车/失焦提交，Esc 取消，空名不收）。hover 名字变 I-beam，因为可编辑的标签和
  不可编辑的标签长得一模一样。
- **记住的是 id，不是行号**：`renamingLayerId` / `renamingSlotId` / (`renamingLayerId`,
  `renamingGroupId`)。改名框开着时删层、删卡、重排都会让索引指向别人。
- **`LayerStack::Listener::namesChanged()` 默认转发 `layersChanged()`**：漏通知（某个面板不刷新）
  比多清场糟糕。只有 `PianoRollComponent::namesChanged()` override 成纯 `repaint()`——
  `layersChanged()` 会无条件关掉编辑中的组（P73），而"站在组里给组改名"是常态。
- **采样块头是唯一需要同步的副本**：`SampleEvent::name` 是导入时抄的槽名，`handlePress` 拖
  另一个采样器上来时也会重抄（`sample->name = slot->name`）。所以 `MainComponent::renameSynthSlot()`
  在改名后遍历所有层，只改 `outputSlotId == slotId && name == 旧槽名` 的块。判据是"还穿着旧名字"，
  不是"挂在这个槽上"——自己起过名的块（或从别的采样器转过来的）有自己的名字。
- **块头怎么拼**：`PianoRollComponent::sampleLabelText()` = `sample.name`，仅当挂载槽的 `name`
  与之不同才加 `"  → 槽名"`（P27）。改名后两者又一致，箭头自己消失。
- **`SynthSlot::userName`**：用户起的名字压过 `pluginName`（P38 的派生命名只对没被改过名的槽
  生效）。`replaceWithNative` / `replaceWithPlugin` 清掉它——换了乐器就是另一种东西。
  存为可选属性 `<Synth userName="1">`，缺省 false，**不升 `formatVersion`**。
- 改名进撤销：`captureState()` 是整份 XML，`namesChanged` → `MainComponent::modelChanged()` 即可。

## 采样音源（P27）
- 拖入采样 → `RackModel::addSampler(...)` 长出 sampler 槽，块 `outputSlotId` 指向它；
  **同一 path 复用已有槽**。
- 块的音频来自**挂载槽**：引擎与绘制（`clipAudio()`）都只调 `routing::resolveSample()`。
- sampler 卡片：pill 写 `SAMPLE`，读数行写时长；点 pill 不换波形，点卡片只高亮不挂载。
- 图层输出菜单、组挂载菜单、音符 override 都**不列 sampler**。

## 采样与音源是双向引用（P32 / P33 改为派生不变量）
- **块没了 → 槽没了**：`MainComponent::pruneOrphanSamplerSlots()` 扫所有图层所有块的
  `outputSlotId`，把没人引用的 sampler 槽删掉。
  **P33 起它挂在模型通知上**（`layersChanged()` / `notesChanged()` 各调一次），不再是
  `onSampleReferencesChanged` 的回调——那条路只有"滚轮删块"会走，剪切、撤销、粘贴顶掉、
  代码直接改模型全都漏，规则只在一条路上成立。**块消失必发这两个通知之一**，所以规则
  现在成立。"谁记得去调回调"不是规则。
- **护栏**：`pruningSamplers`（自身重入）、`restoringHistory`（撤销分两步恢复）、
  `suspendSamplerPrune`（`buildSession` / `project::load` 期间层与机架短暂不同步）。
  三者缺一，就会在换工程的窗口里删掉"下一行正要引用"的槽。
- **快速通道**：机架上没有 sampler 就直接返回——这条路径在拖音符时每帧都跑。
- **剪切→粘贴必须是"移动"**：切走会立刻触发清扫，剪贴板里的 `outputSlotId` 因此可能指向
  已删除的槽；`pasteSamplesFromClipboard()` 会 `rack.addSampler(clip.audio, path, name)` 重新登记。
  否则粘回来的块指着空气，`resolveSample` 回落到图层默认乐器，听感换了却找不到原因。
- **槽没了 → 块没了**：`MainComponent::removeSlotAndItsClips()` 是删槽的**唯一入口**（卡片 ×、
  菜单项 40）。**先删块再删槽**：反过来会短暂留下 dangling mount，音频/面板/撤销都会看到。
  `PianoRollComponent::deleteSamplesMountedOn()` 跑**所有图层**，且**不再回调**
  `onSampleReferencesChanged`（宿主正在删槽，回调会重入 rack 的通知）。
- **换采样音源 = 换文件**：`mountSelectedSamplesToSlot(slotId)`（选中集）把块的 `audio/name/path`
  换成槽的、`offset` 归零、长度回到新文件全长、fade 夹住；**`startBeat` 不动**（换采样不是搬音乐）。
  `slotId < 0` = 跟随图层：先把当前听到的 buffer 抄进块，再解挂。
- **已经挂在该槽 + 已经持有同一 buffer → 直接跳过**。否则"点一下自己那张卡片"会把用户的裁剪
  悄悄还原成全长。
- 高亮槽用 `selectedSamplerSlotId()`，只认 `resolveSample().viaSampler`——块跟随图层时它响的是
  乐器，点亮乐器卡会谎称卡里装着那段音频。

## 删除音乐前先问（P34）
- **两个方向都要问**：块没了 → 删槽（清扫），槽没了 → 删块（删卡片）。P32/P33 是自动的，
  P34 加了闸门：**自动发现，人工确认**。
- **询问只有一处** `askSamplerCascade`（`std::function<void(title, body, onAnswer)>`），
  执行只有一处（`removeSlotAndItsClips` / `removeSamplerSlots`）。别把对话框塞进执行器——
  程序化调用会跟着弹框。
- `installSamplerCascadePrompt()` 装真对话框（`AlertWindow` + `enterModalState`，
  删除/取消）；自检把它换成**同步返回**的脚本答案，并清掉延迟——自检没有消息循环可泵。
  跑完 `runSelfTest` 必须装回去，否则整个 app 从此不问。
- **清扫走消息队列**（`deferSamplerPrompt = true`）：清扫是从 `notesChanged` 进的，拖音符时
  每帧都跑，在拖拽中途开模态框会吞掉 mouse-up。`prunePromptQueued` 保证一次只问一个问题。
- **问之前先重算**：`orphanSamplerIds()` 在真正发问时再跑一遍，不搬运队列里的旧名单。
- **"保留"要记住**（`pruneDeclinedSlots`）：清扫每帧都跑，不记住就不是提问是骚扰。
  某块重新挂上这个槽时**自动解除**；槽被删也清掉（否则复用的 id 会继承豁免）。
- **不承诺做不到的事**：最后一个槽删不掉（`getNumSlots() <= 1`），所以**不问**；
  候选数也夹到 `numSlots - 1`。问完却拒绝执行比不问更糟。
- **没有成本就不问**：卡片上没挂块 → 直接删；非 sampler 槽 → 直接删。
- **删块会通知**，所以 `removeSlotAndItsClips` 也持有 `pruningSamplers` 守卫，否则那一瞬
  卡片被当成孤儿，用户会收到第二个问题（问的是"屏幕上的这张卡"）。

## 走带（P33 空格开关 / P51 停止回起点）
- **暂停键已删除**，界面上只有播放 + 停止两个按钮（`play` 188 / `stop` 226，4 px 间距）。
- `stopDecision(engaged, playStartBeat)` 是停止语义的**唯一算术**（`Util/SharedRules.h`）：
  **在跑 → 停 + 回到本次播放的起点**（P51；起点由 `playTransport` 记在 `playStartBeat`，
  默认 0 = 无锚点的调用方行为同旧的回零）；**已经停着 → 什么都不做**。
  P32 曾以"停止变成第二个从这里播"为由删掉回起点——P51 用"记住的是本次运行自己的起点"
  把它请了回来：空格再按一次还是从同一段开头播，回零仍然归回车和标尺。
- `playTransport(bool openDevice)` 是唯一实现：先读 `audio.getPositionBeats()` 存锚点再
  `audio.play()`。无设备重载供自检用（不抢声卡）。
- `playStartBeat` 只在 playTransport 里写；**播放中途拖标尺不更新锚点**——停止仍回本次
  播放的起点，这是已知的、用户认可的语义。
- 落点写盘只有一处 `parkTransportAt(beat)`：`audio.stopTransport()`（引擎语义 = 归零 + 淡出）
  **之后**再 `setPositionBeats`，中间不重绘，看不到归零那一瞬。
- **空格 / 回车**：空格 = 在跑就停、停着就播；回车 = 永远回零。
- **播放跟随视图（P58 / P64 / P66）**：`setPlayheadBeat()` 里两条互斥分支。
  **贴边跟随（P66）** = 播放头越过窗口**右边界**（`scrollBeats + visible`）就 `scrollBeats = beats - visible`
  —— 播放头钉在标尺尾部，画面在它下面连续滑动；越过**左边界**就 `scrollBeats = beats`，钉在标尺头部。
  每帧都滚一小段（60fps ≈ 0.03 拍/帧），所以**每帧整窗重绘是它的正常成本**，不是 bug。
  左边界要求 `beats > 1e-9`：回退到 0 是"回家"不是"滚动"，交给下面的归还分支（用户明确要求排除 0 点）。
  **归还** = 播放头回到 `userScrollBeats` 那个窗口内，视图退回原处；**必须再要求"播放头在往回走"**
  （`beats < previous`）。推走时抓拍的那个窗口本来就含着当前播放头，少这一条就是：本帧推走 →
  下帧归还 → 再下帧推走，60fps 横跳一整个窗口，表现为钢琴窗疯狂闪屏（P64）。
  P66 之前的"推走"是**翻页**：触发线 3/4、落点 15%，一次挪 60% 窗口后画面静止。别把两种写法混用。
- **标尺拖拽的自动滚动（P67）**：`scrubRulerTo(screenX)` 是标尺左键的唯一入口——mouseDown
  与 mouseDrag 都调它，点击和拖动不可能对不上。顺序固定：**先扩标尺**（`raw > timelineBeats`
  → `ensureTimelineExtendsTo(raw)`）、**再吸附**、**再滚**、**最后 locate**。
  滚动的判据用**吸附后**的 beat（不是指针）：指针越出右边界就 `setScrollBeats(beat - visible)`，
  越出左边界就 `setScrollBeats(beat)`——和 P66 的贴边跟随是同一个几何，所以拖过去的播放头
  **恰好**停在标尺头/尾。吸附开着时最多差半格，视觉上是"头贴边、指针在边外一点"，别再调回来。

## 文件树与扫描（P28 / P33 默认折叠）
- **层级唯一来源是扫描出来的 `DirNode` 树**：`buildRows()` 摊平成行。空目录不出行。
- **默认全部折叠**（P33）：store 里记的是**已展开**的分支 `!expand <path>`，未列出的就是折的。
  P28 记的是 `!collapse`（默认全开），扫完一个大库当场变成千行墙。读旧 store 时任何
  `!` 开头且不认识的标记**跳过**，不能当路径。
- **根行仍默认展开**：只展开它自己，子目录一律折——否则块面板里只剩一行，用户以为扫崩了。
- `setSubFolderExpanded(dir, expanded, persist = true)`：`persist = false` 给快照夹具用，
  否则临时路径会写进用户真实 store。
- **行列表缓存 `cachedRows` + `rowsDirty`**：改树/折叠都要 `invalidateRows()`。
- **`addFolder(folder, persist)`**：`persist = false` 是快照夹具和 `loadFromDisk` 用的。
- **扫描在后台线程**，只写三个 atomic，结果经 `AsyncUpdater` 回消息线程。**模型只在消息线程改**。
- 进度用**逐层 BFS**；收尾设 **1.0**（不是 0）。`waitForPendingScans()` 自己泵 AsyncUpdater，
  所以 `handleAsyncUpdate()` 必须幂等。**析构必须 `stopThread(-1)` 兜底**。

## 采样块：选择 / 窗口 / 曲线（P28–P29 / P49 / P50）
- **单击替换整个选择，框选可以同时选中两类**。`setSelection`/`setSampleSelection` 清对方
  （单击语义）；`setMixedSelection(notes, clips)` 一次性替换两者（marquee 与粘贴用）。
  Delete / Ctrl+C / Ctrl+D 一律"两类一起"，不留"你指哪个"的暗选择。
- **拖动只有一份实现**：`captureEditOriginals()`（音符+采样）→ `clampMove()` + `applyMove()`。
  `DragMode::moveSample` 已删除；块拖动走 `moveNote` 分支。
- **拉伸是选区级的**（P56）：抓住选中块的一条边 → `applySampleTrimDrag(主块, 目标, 边, 兄弟原件)`，
  先按现有规则摆好主块，**再把实际 delta 读回来**应用给兄弟（尾：`length += delta`；
  头：`start += delta`，终点钉住）。不要用鼠标算的 delta：最小长度和 offset 会把主块夹住，
  兄弟就会超前。每个兄弟走的是同一个 `applySampleTrim`，各自的 offset/最小长度仍然生效。
  早先只有被抓住的那块动（P25 的规则照搬到块上），多选时看着像"只改了一个"。
- **高度是上下两条边**（P72）：`edgeOfClip()` 一次命中四条边（头 / 尾 / 上 / 下，取最近的），
  `clipEdgeAt()` 只在**已选中**的块里找（P58/P59 的规则不变），`selectedTrimEdgeAt()` 是它
  过滤出「头 / 尾」的结果——不要各判一次，角落像素会得到两个答案（按下按一个走、光标画另一个）。
  上边缘拖 = `pitch` 不动、`laneSpan` 跟指针所在行变；下边缘拖 = 顶行不动、`pitch` 跟指针行变。
  两向都按整行（`pitchAtScreenY`），因为 `laneSpan` 就是行数，块不跨行。上限
  `maxLaneSpanFor(pitch) = min(128, numPitches - pitch)`——夹 `laneSpan` 本身会让 pitch 120 的块
  长到 128 行以外（画出去，用户以为没反应）。
  高度带 `jmin(band, height * 0.25f)`：一行高的块只有 `keyHeight`（约 18 px），上下各一条 8 px 的
  带子会把整块吃掉，中间就没有「拖位置」的地方了。光标：上 / 下 → `UpDownResizeCursor`。
  改高走 `applySampleSpanDrag()`——delta 取**已生效**的那个（clamp 会吃掉一部分），并且加在
  **手势起点**上而不是当前值上：组同步已经把主块的绝对高度抄给了别的副本，按当前值加就是两倍高。
  右键「加高一行 / 降低一行」走 `growSelectedSamples(lanes)`：作用于整个选择、各自一步、
  副本由 `syncShapeOfClip()` 跟上（高度是形状，见「编组」节）。
- **块是窗口不是拷贝**：`sourceOffsetSeconds` 是文件内起点；左端拉伸同时改 `startBeat` 与
  offset（同样的秒数），音频留在原时间位置；右端拉伸只改 `lengthBeats`。
- **尾部无上限（P51）**：右端想拖多长拖多长（见"采样伸缩"节）；左 ≥ offset 允许的距离
  （offset 不会 < 0，那个判据仍在）。旧的 `maxSampleLengthBeats()` 已删除。
- **P65：开关没了**。块自动化框底部那行不再是"目标标签 + 启用开关"，整行就是目标按钮
  （`getSampleTargetArea()` 就返回 `getSampleMenuArea()`），曲线画出来就生效。右键菜单里的
  「启用 / 停用曲线」一并删掉——留着它只是把同一个谎言挪一次点击的距离。
  `openSampleAutomation()` 会把 `enabled = false` 的旧曲线恢复成 true：开关删了之后，
  那是唯一的回头路（老工程里可能存在这个状态）。
- **命中区（P49 起是两个 + 一个按钮；P51 按钮挪到块顶；P72 加两条横边）**：左右边 = 拉伸，
  上下边 = 改高度，其余 = 拖动，**块顶右上**的 chevron = 展开曲线箱（Y = 块顶 + 1，距右缘 = 音符 chevron
  的同一公式 band+air+width——"右上角的小箭头展开曲线"对音符和块是同一个习惯）。
  **淡入淡出手柄已删除**（P29 的三个 `kFade*` 常量、`applySampleFade`、
  `hitTestSampleFadeHandle`、`drawSampleFades` 全套退场）。
- **块自己的音量曲线**（P49）：`SampleEvent::volumeCurve`（`NoteEnvelope`，x 归一化 0..1）。
  新建/粘贴的块是 `makeIdle()`——平线 1.0 且**禁用**，第一次编辑才点亮。
  `publishSchedule` 把 x 乘 `blockSeconds` 换成秒，渲染循环只求值；咔哒 ramp 保留。
  改动要走 `notifyNotesChanged()`（撤销与采样槽清扫都挂在通知上）。
- **块自己的声相曲线**（P50）：同一个块可以带两条曲线，
  `SampleEvent::automationTarget` 决定哪条生效、哪条显示。
  `panCurve` 的空闲值是 `makeIdle(0.5f)`（中位），不是 1.0——`sampleAutomationNeutral()`
  按目标返回中性值，重置菜单必须调它，不能写死 1.0。渲染端先按块两端声相插值，
  再叠加曲线，最后**只夹一次** `jlimit (-1, 1)`。
- **箱几何**：宽 = 块宽（与音符箱同一条规矩），块下方 3 px，没空间就翻到块上方；
  菜单行 26 设计像素，开关固定 22×12（**不要**拿行高当直径）。
  菜单行三区：左侧目标按钮（"音量"/"声相"，带 caret）→ 开关 → 右上角叉号（关闭）。
- **曲线区的横向范围就是块的横向范围**（P50 修）。P49 的实现给箱内缩了 10% 边距，
  曲线与块对不齐；现在 `getSampleCurveArea` 直接用块的 `getX()/getWidth()`，
  自检里有一条断言读同一对数字（"曲线 x 范围 = 块 x 范围"）。
- **绘制顺序是一条规则**：底色 → 网格 → 填充 → 参考线 → 曲线 → 端点。
  声相中线必须在 `fillPath` 之后画，否则被自己的洗色盖掉（见 PITFALLS P50-E）。
- **切割**（P50）：`splitSelectedSamplesAtPlayhead()`，只切**被选中且跨越播放头**的块。
  左手半段长度截到播放头，右手半段的 `sourceOffsetSeconds` 推进
  `min (window, elapsedOutput * speed)`——是左半段**实际消耗的源秒数**，
  不是窗口比例。两条曲线在接缝处 `subEnvelope (env, 0, u)` / `(env, u, 1)` 切分，
  保持端点插值，ramp 不会在接缝处重新开始。
- **反转**（P50）：`SampleEvent::reversed` 只是**播放期镜像**——
  渲染循环用 `readElapsed = length - elapsed`，源文件不动、共享 buffer 不动。
  波形按列反向绘制，块头加"反向"徽标。撤销是整工程快照，所以不用单独处理。
- 波形只画窗口内的 peaks：裁过前后的块**画面必须不同**。
- 复制/粘贴（Ctrl+C/X/V、Ctrl+D）：粘贴的块 **`groupId = -1`**，锚点与音符一致
  （playhead > 剪贴板起点 > 右移一个块长）。`copySelectionToClipboards()` **清空另一类**。
- **P37 贴落点三条规则**（只对采样，音符仍是老行为）：① 连按 Ctrl+V 要接着排——
  `samplePasteNextBeat` / `samplePastePlayheadSeen`，playhead 没动就接上次终点，动了就按新位置；
  ② 吸附开着时**落点 beat** 必须是网格线，所以让位/前进一律 `snapUp`（ceil 到网格），
  不能先 `snapBeat` 再 `+= length`（长度非网格整数倍时那个加法把落点顶出网格）；
  ③ 落到源位上要往前让，让完再吸附。`copySamplesToClipboard()` 重新起 streak。
- 绘制：块被选中 = body 提亮 ×1.6 + 白环 + 四角括号（与音符同一套语言），只在激活图层画。

## 高亮（P30 起：没有遮罩）
- **不再压暗网格**。P16–P29 那层 46% 黑遮罩已删除；高亮 = 被聚焦音源那些音符上的
  彩色描边 + 左侧小竖标 + 一层同色薄染（同色太淡就看不出来，太浓就毁掉"不改亮度"）。
- **选中音符不再点亮高亮**：`updateRackRoutingFocus()` 只喂 `rackPanel.setSelectionActive`，
  不再写 `focusedSlotId`。"点一下音符整张网格变灰"就是这么来的。
- **出口必须永远在**：`PianoRollComponent::dismissSpotlight()` 是唯一出口（网格空白处单击 /
  Esc / 机架菜单项 34 / chip）。chip 挪到**右下角**——右上角是采样块和高音区，压的是要看的东西。
- **P32：选中采样会自动点亮它的采样音源**，这是唯一自动点亮的情况（音符仍然不点亮）。
  `focusFromSampleSelection` 记住"这次高亮是选择带来的"：选择没了只撤销**这一种**高亮，
  用户手动点亮的那张卡不会被下一次点空地带走。

## 采样波形（P30–P31）
- 两种画法，由 `sampleWaveFillsBlock()` **一个函数**决定，`sampleWaveColumns()` 与绘制都调它：
  * **填满**（变速变调 / 只变速不变调）：整段文件被压进块里，`secPerPixel = 剩余秒 / 块宽`。
  * **截断**（原速 / 不变速只变调）：`secPerPixel = (60/bpm)/beatWidth`，一个像素永远是
    同一段源，块变短只是画面被切断——**长度变了波形不能变**。
- 块比文件长时尾部留空（P31 起拖入不再向上取整，正常情况不会再出现）。
- 右键「恢复原始长度」= `restoreSampleLength()`：offset 归零、长度回到文件全长、
  `startBeat` 左移 offset 那么多（音频回到时间线原位）。**整个过程不吸附**。

## 采样伸缩（P31 / P51 尾部无上限）
- `SampleEvent::stretchMode`（`none/resample/tempo/pitch`）是**块**的属性，不是一个全局开关。
- **唯一比例是 `stretchRatio(bpm) = 剩余源拍数 / lengthBeats`**：起点裁剪同时改 offset 与
  长度，比例不变；只有拖尾会改它。"恢复原长"= 比例 1.0。
- **尾部没有上限（P51，P30 的上限已删）**：`applySampleTrim` 只夹最小长度。resample/tempo
  拖长 = 真时间伸缩（比例 < 1，publish 期自己夹 0.125..8）；none/pitch 拖长 = 文件末尾之后
  是静音，波形只画有声的列。P30 的断言（"不能比文件长"）已改写成新规则，不要复活。
- publish 期折算成 `speed`（源秒/输出秒）与 `pitch`（颗粒内读取倍率），夹到 0.125..8：
  原速 1/1；变速变调 r/r；只变速 r/1；只变调 1/r。音频线程只做乘加，不算 log。
- **拖入的块长度 = 文件精确长度**（不再 ceil 到网格）。长度不在网格上是正确的——块就是那段音频。
- 块上的黄色徽标是模式的唯一可见痕迹：`变速变调 ×2.50` / `变速 ×2.50` / `变调 +12.0`
  （半音 = `12*log2(pitch)`）。宽度 < 82% 块宽才画，"块上什么都不写"就是默认模式。
- **入口分布（P51）**：右键"拉伸方式"只剩 原速 / 变速变调 / 只变速不变调；
  不变速只变调从菜单移除，和变速变调一起进了**机架采样卡的"拉伸"控件**（见下节）。
  模式本身与工程格式不变，老文件的 pitch 块照常播放、徽标照常画。

## 标尺缩放（P31，范围 P69）
- 右键拖标尺 = 以**按下那一点**为锚缩放：`setBeatWidthAround(px, anchorBeat, anchorX)`，
  锚点由 mouseDown 捕获（`rulerZoomAnchorBeat/X`），不是每帧重算。
- 滚轮缩放仍锚定**视图中心**（`setBeatWidth()` 只是它的特例）。两者共用同一份数学。
- 唯一的例外是 `clampScroll()` 在时间线两端生效——左边没有负拍，锚点只能让位。
- **范围 `12 … 768` px/拍**（P69）。768 不是随便定的：12 px/拍时一个**拍**占 12 px，
  768 px/拍 时一个 **1/64 拍**占 12 px，两端对称。要再放宽就得同时看网格阈值（P69-A）。

## 网格与吸附（P68 退档 / P69 细分）

三个量，别混：

| 量 | 值 | 回答的问题 |
|---|---|---|
| `gridDivision` | `0.25` 拍 | 音乐上的最小单位：粘贴推进、`nextBarLineAfter`、导入音符的保底 |
| `visibleGridStep()` | `0.015625 … 256` 拍 | 这一屏**画得出来**的那一档 |
| `minimumNoteLength()` | `jmin(0.25, 可见步长)` | 音符/采样块能缩到多短 |

- `visibleGridStep()`：从 `finestGridStep`（`gridDivision / 16` = 1/64 拍）起，逐档 ×4
  （1/64 → 1/16 → 拍 → 小节 → 4 小节）直到相邻线至少 `ui::pf(5)` px。**同一个循环管两个方向**：
  放大就细分、缩小就退档，整条梯子是一条 while，没有第二处写步长。
  切换点：`beatWidth ≥ 416` 出 1/64 拍；`104…415` 是 1/16 拍；`26…103` 是 1/16 音符。
- **绘制与吸附读同一个 step**：`collectGridLines(from, to, out)` 产出线表，`drawGridLines` /
  `drawGridInside` 遍历它，自检也数它。"略去中间的线"不是画笔里的 if，而是几何的函数——
  画笔没有机会忘记检查。
- **线条等级按 beat 判，不按循环下标**：`gridLineAlpha(beat)` 用"是不是 4 拍的整数倍 / 1 拍的
  整数倍 / 0.25 拍的整数倍"决定 0.16 / 0.075 / 0.030，其余 0.018（1/64 那一级）。
  旧的 `step % 16 == 0` 只在 step 恒为 1/16 时成立；step 升到 1 拍后第 4 拍是 `4 % 16 = 4`，
  会被判成普通拍线，整个网格变平（P68-A）。四级同亮度也不行，那是 P68 要消灭的那片灰。
- **吸附跟着网格走**：`snapBeat()` 读 `visibleGridStep()`。缩小时吸到 1 拍，放大到上限吸到
  1/64 拍。一个"看得见的线才吸得动"的判据，比多一个吸附开关诚实。
- **最短长度只往下让**：`minimumNoteLength()`（P69-C）。直接拿可见步长当最短长度，缩小到
  12 px/拍时下限会涨到 1 拍，音符画不短——视图不该抬高模型的能力。
- 不受影响的地方：`snapUp()` 的粘贴推进、`nextBarLineAfter(first, gridDivision)`、
  `clamp()` 里的 `timelineBeats - gridDivision` —— 它们要的是**音乐最小单位**。

## 按键发声（P39 通道 / P40 双开关）

- **两个独立开关**（`MidiSources{ typingKeyboard, externalController }`，P40 默认
  `{true,false}`）：不是三选一。"什么都不开" = 两个都关。**不跨启动记忆**（默认值是代码里的）。
- **关掉一个来源才释放它自己的音符**：键盘 → `releaseAllTypingNotes()`；控制器 → 一条
  `allLiveNotesOff()`（端口说不出自己按着什么）。**打开一个来源不打扰另一个**——这是双开关
  存在的全部理由，自检里断的就是这条（按着 Z/X 再开控制器，`typingHeld` 不能变）。
- **设备归 `MidiInputHub` 所有**（`Source/Audio/MidiInputHub.*`）：一个 MIDI 端口只能开一次，
  所以"打开设备的东西"和"发声的东西"必须是同一个对象；设置页只是它的视图。
  `setSources` 在控制器关掉时关所有端口，`setDeviceOpen` 在控制器关着时**拒绝**打开。
- **设置页布局要算，不能假设**（P40）：两块会同时出现，`MidiSettingsPage::plan()` 返回
  `{keyboard, devices, monitor, deviceRowsTop, maxDeviceRows}`，**绘制和命中测试读同一份**；
  装不下的设备行会被裁掉并在标题行写「显示 N / M 个」。
- **线程边界**：MIDI 回调在 MIDI 线程；它只碰原子变量（`liveTargetSlot`）和引擎的
  无锁队列。模型（`RackModel` / `LayerStack`）只在消息线程读。
- **目标音源**：`focusedSlotId`（且不是 sampler）→ 否则 `routing::layerSlotId(activeLayer)`。
  与 FL 的"选中通道"同义；`applyFocus()` 与 60 Hz 定时器把这个答案写进原子。
- **钢琴窗预览也走 live 通道**（P71）：`MainComponent::auditionPreview()` 是唯一分岔点——
  目标槽 `isPlugin()` → `noteOnLive()` 发真 MIDI（`hostedPreview` 记槽与音高，60 Hz `timerCallback`
  到 420 ms 发 `noteOffLive()`，同音高 + `retrigger == false` 只续期不重发）；否则才 `auditionNote()`。
  切目标时两头都要收：`stopAudition()` 掐正弦（标志把 `played` 推到 `kAuditionLength` 走 4 ms 淡出，
  **消费点必须在 serial 分支之后**），`releaseHostedPreview()` 放插件音。`restoreState()` 开头也放一次。
- **发声走 `PlaybackEngine` 的 live 通道**，不是试听通道：
  `noteOnLive(slotId, pitch, vel)` / `noteOffLive(pitch)` / `allLiveNotesOff()` +
  `beginLiveBlock()`（每个回调一次，唯一排空点）+ `renderLive()`（`AudioEngine` 在 masterFade
  之后调用，和试听一样不吃走带淡出）。
  队列是 `AbstractFifo`；**生产者侧上 `SpinLock`**（生产者有两个：MIDI 线程和消息线程），
  音频线程**不加锁**。16 个 voice，attack 3 ms / release 60 ms。
- **插件乐器**：走带在跑时，`renderHostedInstruments` 把本块的 `pendingLive` 合进它自己的
  `pluginMidi`，并把 slot 记进 `transportPluginSlots`；`renderLivePlugins` 跳过这些 slot。
  一个 block 处理两次不是"双倍输出"，是**双倍 DSP 速率**。
- **note-off 只有音高**（键不知道自己是哪个音源开始的）→ `liveHeldNotes` 记 `pitch → slotId`，
  note-off 靠它找回原音源；否则插件会一直按着那个音。这条表也是 all-off 展开的依据。
  **写入点必须在"振荡器 / 插件"分叉之前**（P41）：写在某一个分支末尾，另一个分支就永远
  不记事，它的 note-off 带 `slotId = -1` 进来、匹配不到、被丢掉——症状是"选中 VST 卡片后
  键盘弹一个音就一直响"。all-off 之后整张表清空（没人按着，就不该还指着音源）。
- **all-off 是一条事件**（`LiveEvent::allOff`），不是消息线程遍历 voice 池；展开成每个
  (slot, pitch) 的 note-off 追加进 `pendingLive`，由正常路径送出。
- **裸按键打向谁只有 `routing::auditionSlotId(focusedSlotId, layer, rack)`**（P43）：
  选中且不是 sampler 的槽优先，否则 `layerSlotId`。电脑键盘、钢琴窗琴键列、试听全调它。
  这条原来是两份实现，只在"选中 VST"这一种情况下说法不同——也就是用户唯一会点的那下。
- **键位唯一出处 `Source/UI/TypingPiano.h`**：FL Studio 默认布局，24 键两八度，
  `Z = C4 = MIDI 60`；低八度白键 `Z X C V B N M`、黑键 `S D G H J`；高八度白键 `Q W E R T Y U`、
  黑键 `2 3 5 6 7`。设置页的映射表和自检都读这张表。
  小键盘 `+` / `-` 换八度（音圈没有别的空键可放；数字键是黑键）。
  八度范围夹在 -4..+4，越界返回 -1 而不是发明音符。
- **按键优先级**：`TypingKeyForwarder`（`juce::KeyListener`）挂在 `pianoRoll` 与
  `envelopePanel` 上——**KeyListener 先于组件自己的 `keyPressed`**，所以 `G` 是音符而不是"编组"。
  `Ctrl` / `Alt` 组合仍归快捷键（`Ctrl+S` 还要保存）。
- **插件编辑器窗口是独立顶层组件，按键不走父链**（P42）：`PluginEditorWindow::setKeyForwarder`
  把 `MainComponent::pluginForwarder` 装到**窗口 + `getContentComponent()` 两层**；
  转发器唯一动作是调 `handleGlobalKeyPress()` → `keyPressed()`，**键表只有一份**。
  有 `juce::TextEditor` 拿着焦点时返回 false（放行），否则在插件界面里输预设名会弹出音符。
- **窗口开着也要能用空格**：同一套转发器把空格交给走带（播放/停止），
  所以插件前台时"点一下空格能停"不需要用户先点回主窗口。
- **释放**：JUCE 没有 key-up 事件。`keyStateChanged` 里按 `isKeyCurrentlyDown` 逐个查；
  60 Hz 定时器再兜一次底（焦点跑到别的窗口时也能松手），**卡音是键盘最不能有的故障**。
- **不过音源滤波器**：live 声音只做 slot 增益 + 声相 + master，和走带里的音色略有差别（已知边界）。

## 主窗口（P40：默认最大化，保留标题栏）

- `MainWindow`（`Source/Main.cpp`）的顺序是 **`centreWithSize(px(1440),px(820))` →
  `setVisible(true)` → `setFullScreen(true)`**，没有手算 `userArea` 减边框。
- **"最大化"就是窗口管理器自己的状态**：Windows 上 `setFullScreen(true)` =
  `ShowWindow(SW_SHOWMAXIMIZED)`，标题栏照旧（`DocumentWindow::maximiseButtonPressed`
  走的也是它），**不是**无边框全屏、**也不是** kiosk（那是 `Desktop::setKioskModeComponent`）。
- `centreWithSize` 那一步是"还原尺寸"：`ResizableWindow::setFullScreen` 在显示状态下会存
  `lastNonFullScreenPos`，点还原就回到 1440×820（实测 1872×1066 px）。
- **自证入口 `--windowtest`**：建真窗口，打印 `maximised` / `titleBar` / 边框 / 内容区 /
  `userArea` / 显示器 scale，再还原一次并打印尺寸。改窗口代码后跑它，不要靠肉眼看。
  本机实测：`maximised=1 titleBar=30 border=7/7/7 content=0 23 2048 1081
  userArea=0 0 2048 1104 scale=1.25x → OK`，`restored to 88 17 1872 1066`。

## 渲染成采样（P50：`Audio/ScopeRender.h`）
- **一个 mixer，不建第二个**：`scope::extract()` 读的是**已发布**的 `PublishedSchedule`，
  按范围**过滤**出子集（`Kind::selection / group / layer`），不重新发布子图。
  `renderToFile(schedule, …, startSeconds)` 多一个起点参数：时间**不 rebase**，
  从该秒开始读，长度才按选区算。
- **插入点由模型算，不由 UI 猜**：`scope::findFreeBeat (occupied, lengthBeats, contentEnd)`
  在已占用区间合并后的空隙里找第一段够长的地方，空图层返回 beat 0。
- **插入逻辑是自由函数**：`scope::insertSampleBlock (stack, rack, layerIndex, file, bpm, name,
  laneSpan, pitch)` → `InsertResult{ok, sampleId, beat, message}`。
  它不碰窗口、不碰单例，所以自检能用本地 `LayerStack` 跑同一份代码
  （否则跑一次自检就往 `docs/demo.atomic` 里塞一个块，见 PITFALLS P50-D）。
- **两段式空位检查**：渲染前查一次（省掉几分钟 CPU），渲染后**再查一次**（用户可能中途编辑）。
  查不到就弹提示框，不静默丢弃。
- 菜单项由 `PianoRollComponent::addRenderScopeItems (menu, hasSelection)` 统一产出
  （id 7 = 渲染选中内容、id 8 = 渲染整个图层），自检用 `MenuItemIterator` **读菜单**
  而不是截图；"没有选区时第一项变灰"就是这条断言的实质。
- 后台任务 `ScopeRenderTask` 带 `scopeRenderSerial`，完成即插入，**不弹完成框**
  （用户要的是"渲染好直接落到空位"，不是"渲染好了，请你自己放"）。

## 排版层（P50：`Util/Metrics.h`）
- **四个可复用入口**：`tracked (designPoints, kerningPixels, style)`、
  `title (points)`、`section (points)`、`drawTabular()` / `tabularWidth()` / `digitCellWidth()`。
  换字号只换调用，不要另写一套。
- **`tracked` 的存在理由**：`withKerningFactor` 的单位是 **EM**，不是像素。
  直接传 "0.5" 会得到半个字高的空隙（PITFALLS P50-A）。要"0.5 像素"就调 `tracked`。
- **等宽数字是手绘的**：JUCE 8 不暴露 tabular figures。`drawTabular` 按"每字一格、
  格宽 = 该字体最宽数字、字在格内居中"绘制，所以比例数字字体也能对齐。
  用在：走带读数、混音台 dB 读数、BPM 数值。
- **层级**：标题 `title()` > 正文 > 大写副标题 `section()`（小字号 + 字距 + 降饱和）。
  已落地的面板：`LayerPanel` 标题与副标题、`BpmField` 标签与数值。

## 踩坑档案
平台陷阱、自检夹具摆位、出图尺寸见同目录 **`PITFALLS.md`**。动以下任一处前先读它：
VST3 扫描 / 采样解码 / 试听声部 / 标尺 / ghost / `--snapshot` / 折叠行高 / 插件参数自动化 /
剪贴板 / 组双曲线 / 删除通道与电平表 / sampler 与 clip / 文件树扫描 / 拉伸与淡变手柄 /
采样波形比例与聚光灯出口 / 采样槽引用与删除 / 走带落点。
