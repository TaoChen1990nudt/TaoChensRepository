# AtomicDAW — 细节与踩坑档案

`MEMORY.md` 存制度与口径；本文件存「某个具体机制怎么实现的、哪里会咬人」。
按需查阅，不必每次全读。

## 各阶段遗留要点
- **VST3 只能在子进程扫**（本进程 dlopen 会 SIGSEGV）：`self --plugin-scan-child`，缓存
  `userAppData/AtomicDAW/plugin-library.txt`（首行 search-path 签名）。插件一律 **int id** 寻址；
  音频线程 try-lock、消息线程阻塞锁；离线导出 `createOfflineClone()`；卸载前先销毁窗口。
- **测试插件** `Tools/TestPlugins/TestSynth.cpp` 暴露 `"level"` 参数并乘进振幅——E2E 断言用它。
  JUCE 把 MIDI 控制参数也塞进 `getParameters()`（实测 2082 个），断言要**按名字**不按数量。
- 音高调制必须用相位积分表（发布时制表），直接乘进 `f*elapsed` 有无界相位误差。
- 采样：`SampleEvent` 只存 `laneSpan`（整数行）+ `lengthBeats`，"占整数行/随缩放"是构造保证。
  WAV 解码手写在 `Media/SampleLibrary.cpp`，别引 juce_audio_formats。采样不进合成器滤波器，
  声相用 balance 不是 constant-power。demo 夹具自产 `demo-clip.wav`，采样块放 beat 12 / pitch 81。
- `beatWidth` 是**成员**（默认 48，12..180，Ctrl+滚轮 / 拖标尺），`setBeatWidth()` 以视图中心为锚并
  fire `onScrollChanged()`。缩放只改 beatWidth，不许改 keyHeight。
- 折叠状态一律存模型（`Layer/SynthSlot/NoteGroup::collapsed`），行高 `f(model)`，命中走 `getRowBounds()`；
  四个列表栏各自持像素 `scrollY`。MainComponent 是 DragAndDropContainer，文件栏只
  `startDragging(var(path))`（`ScaledImage` 重载），钢琴窗是 DragAndDropTarget。
- `--snapshot` 主图 1872x1066、混音台 1534x390 与 px(700) 窄图；**不覆盖已存在的 PNG**，先 `rm -f`。
  `runSelfTest()` 结尾必须 `loadDemoProject()`；摆位函数一律在自检之后调。
- **试听声部**：`auditionNote()/renderAudition()` 只由 AudioEngine 回调在 masterFade **之后**调用——
  导出永不发声。`retrigger=false` 是拖拽滑音；波形/电平/声相取 `SlotMix.waveform`。回调的 early-return
  必须问 `hasAudition()`（= `engaged || serial != consumed`），只比 engaged 会吞请求。
- **采样试听**：`previewSample()/renderSamplePreview()`，buffer 走 `SpinLock` + `shared_ptr`，
  **新预览顶掉旧的**，按源/设备采样率比重采样。FileBrowserPanel 单击行请求。
- **标尺**：左键 = `rulerScrub`（连续 `onLocateRequested`，`setPositionBeats` 不停机），右键 = `rulerZoom`；
  两者都要在 mouseUp 里 `updateCursor()` 收尾。
- **JUCE 8 往 `juce::Image` 画图**：必须先析构 `juce::Graphics` 再读像素/写文件（否则全透明）。
  手绘光标走 `renderToolCursorImage()`，`--cursor-preview` 出 8 倍图。
- **图层可见性 = ghost 开关**：`Layer::visible` 默认 **false**，含义是"不是当前层时要不要画成 ghost"。
  判断只走 `isLayerShown(i)`。**当前层永远画、永远可编辑**；眼睛点击不顺带激活行。

## 从制度册移出的实现细节

### 原属「音频」

- **插件参数自动化（P24）**：发布期把音符归一化 time 与组绝对拍统一成绝对拍，塞进
  `PublishedSchedule::pluginParams`（`PluginParamCurve`，priority 组 1 < 音符 2，`stable_sort`）；
  音频线程只按序 `setValueNotifyingHost`，`|现值-目标| < 1e-4` 才写（否则插件编辑器每块重绘）。
  参数列表走 `Model/PluginParam.h`，UI 不碰 PluginHost。

### 原属「编辑手势（P22 起）」

- **曲线剪贴板**：`CurveClip` 只存「距源起点的拍偏移 + 源长度 + target」。音符包络 `time×length`、
  组 lane `beat-start` 各一处转换，两种框互换粘贴不需要特判。不拉伸：目标短 → 截断且末点保持
  （**别在末尾砸 0**，那是原曲线没有的断点）；目标长 → 补三连点落 0。剪切 = 复制 + 放平到中性值。

- **Ctrl+C/X/V 去向**：`curveSelection`（打开且被点过的内联框）优先，否则音符剪贴板。画布别处一点
  清选择但不关框（要能对比两条曲线）。

### 原属「编组」

- **组复制粘贴（P24）**：`GroupClip` 刻意**不存 `NoteGroup`**（id 私有、`addGroup` 分配）。粘贴先
  `addGroup(uniqueGroupName(...))` 再逐条落音符，组 lane 点 `beat + delta` 平移。编辑菜单的「粘贴目标」
  行在组剪贴板时必须说 `整体：组「X」· N 条曲线`。

- **两条曲线**：`NoteGroup` 用 `std::array<AutomationLane,2>`（**不能**用引用别名成员，vector 要求可赋值）；
  `GroupMix::panAt()` 与图层同算术，`GroupBlock` 按块存 pan0/pan1。**`GroupPanel` 必须同时覆写
  `notesChanged()`**——编组动作走 notes 通知，只覆写 `layersChanged()` 会让新组等到鼠标移上去才画。

## Phase 26（删除通道 / 立体声表）

- **`Rectangle<int>::withX()` 保留宽度**，`withWidth()` 保留 x。画"右半根柱子"必须
  `withX(right - bar).withWidth(bar)`，只写 `withX` 会从对的起点一路跑出表外。
  这类错误肉眼在 1x 截图上看不出来（两根柱都"在那儿"），要靠自检打印矩形。
- **`removeChannelAt` 是三张表的位移**，不是一张：`inserts` / `channelMono` / `channelOutputs`
  都按位置寻址。只位移前两张而只改第三张的值，会让被删处之上的条子"丢了目的地"、
  被删处之下的条子"留着别人的目的地"——而且渲染不报错，只听得出信号跑错地方。
- **机架发送的重编号不属于模型**：`SynthSlot::mixerChannel` 由 `MixerPanel::deleteStrip`
  先拔后减（`== i → -1`、`> i → --`）。顺序不能反：先减再拔会把"指向被删条子的发送"
  改成一个还在的条子，用户的发送凭空换了目的地。
- **`maxMixerChannels` 是镜像常量**：它和 `MixerModel::numChannels` 必须相等，
  ProjectIO 的 `static_assert` 就是钉这个的。P25 改上限时漏了它，通道 9+ 的发送全被钳成 -1，
  UI 上只是"下拉里选了却没反应"。

- **组框网格与吸附（P24）**：`drawGridInside()` 与钢琴窗 `drawGridLines()` 共用 `screenXForBeat` +
  `gridDivision` + 同一套强调与 `floor(x)+0.5f` 对齐；曲线区 x/宽度必须等于 beat 0 的 x 与 N 拍宽度
  （自检断言）。打点与拖动都经 `snapCurveBeat(b, span)`，只有一个吸附函数。

- **挂载只有一条路** `mountSelectionTo(slotId)`：只有 `groupId < 0 || !isWholeGroupSelected(groupId)`
  才 `routeSelectionTo`（逐音符 override），整组选中才写 `group.outputSlotId` 并清组内 override。
  机架卡片：有选区时普通点击 = 挂载，无选区 = 只看（`setSelectionActive` 由 `applyFocus()` 维护，
  别拿 `focusActive` 顶替）。Alt+点击永远挂载。

## Phase 27（采样音源 / 采样入组）

- **播放与绘制必须同源**：`routing::resolveSample()` 同时被 publish（引擎）和
  `PianoRollComponent::clipAudio()`（波形缩略图）调用。曾经块上画的是 `sample.audio`、
  引擎读的是挂载槽的 buffer —— 换挂载时"看着没变、听着变了"，这种错位比 bug 更难查。
- **`RackModel::addSampler` 按 path 复用**：同一文件第二次拖入返回已有槽 id。
  自检里比较"挂 A 时播 A"要拿 `rack.findSlot(id)->sampleBuffer` 比，
  **不能**拿本次 `loadSampleFile()` 的返回值比 —— 复用时那是一次新的解码，指针不同、内容相同。
- **`auto*` 撞 `shared_ptr`**：`const auto* b = slot->sampleBuffer;` 会推成 `const int*` 并报
  C3535/C2440。用 `const auto b = ...`，三目里给 `std::shared_ptr<const SampleBuffer>()` 兜底。
  `std::shared_ptr` 之间不能 `==`（C2678），要比较 `.get()` 或直接比两个 shared_ptr 对象。
- **`SlotMix` ≠ `SynthSlot`**：音频线程只见 `SlotMix`。给 `SynthSlot` 加字段后必须同步搬到
  `SlotMix` 并在 publish 里赋值，否则引擎访问不到（`error C2039: "sampler": 不是 SlotMix 的成员`）。
- **sampler 不能 `continue` 掉整个槽**：clips 的渲染写在同一个 `for (slot)` 体内、声部循环之后。
  在循环入口 `continue` 会连 clip 一起跳过；正确做法是在**声部循环第一行 break**。
- **sampler 也别出现在音符的挂载目的地**：`layerSlotId()` 的兜底换成第一个**乐器**槽；
  图层输出菜单 / 组挂载菜单 / `mountSelectionTo` / `routeSelectionTo` 全部跳过 sampler。
  否则"点了采样卡，音符全哑"是零提示的破坏性操作。
- **机架空位**：`cardHeight = px(128)`，视口只放得下 2 张卡。新增槽后必须滚到底
  （`rackChanged` 只在**增长**时滚，改名不滚），否则用户拖完文件看不到新卡片 —— 看起来像没成功。
- **`pruneEmptyGroups()` 只看音符会把"只有采样的组"删掉**：成员判定要 notes + samples 都为零。
- 组复制粘贴（P24）目前只带音符，采样留在原组。改的时候别以为它会自动跟着。

## P28 文件树 / 异步扫描 / 采样选择
- **Thread 对象不能在线程运行时析构**：JUCE 只 `jassert`，Release 下线程会去碰半个面板。
  析构里 `signalThreadShouldExit()` → `waitForThreadToExit(5000)` → 还不行就 `stopThread(-1)`。
  `findChildFiles()` 不可中断，所以等待是必需的，不是保守。
- **`addFolder` 在 `loadFromDisk` 里写盘会截断 store 文件**：读取时每加一个就
  `saveToDisk()`，写的是"当前已加的"，后面的路径全被覆盖掉。load 期间一律 `persist = false`，
  读完写一次。
- **`handleAsyncUpdate` 要幂等**：线程跑完 `triggerAsyncUpdate()`，自检的 `waitForPendingScans`
  会 `cancelPendingUpdate()` 后手动调用它。第二次进来 `scanner` 已 reset，必须直接 return。
- **进度收尾是 1.0 不是 0**：`startNextQueuedScan()` 队列空时若把 progress 归零，
  扫描完之后任何"看结果"的地方都会读到 0%，看着像失败。
- **自检断言"进度从 0 开始"前必须先 drain 队列**：构造时 store 里的文件夹已经在扫描，
  不先 `waitForPendingScans()` 读到的是别人跑到一半的百分比。
- **空目录不出行**（`fileCount == 0` 直接 skip），否则树里全是没有内容的空壳。
- **`DragMode::moveSample` 删掉之后**，单块拖动走 `moveNote` 分支（`dragOriginals` 为空、
  `dragClipOriginals` 有值）。`moveNote` 里的音高试听只在 `dragOriginals` 非空时跑，安全。
- **出图裁剪**：`ffmpeg -i in.png -vf "crop=W:H:X:Y,scale=..."` 可用（`scale` 用 `neighbor`
  才是像素倍放大）；`Add-Type -AssemblyName System.Drawing` 会被安全策略拦，别试。
- **快照要看清树就给面板单独出图**：主窗口里文件面板只有 ~8 行高，四层树放不下。
  `createFileBrowserSnapshot()` 临时 `setSize` 再 `setBounds(previous)` 还原。

## P29 采样窗口 / 淡变 / 混合选择 / 聚光灯出口
- **别用整块 `rect.withX()` 挪手柄**：`withX` 保宽度，右柱会跑到块外（P26 老坑的邻居）。
- **撤掉淡变手柄的整块命中**：手柄只在顶部条（≤ 半块高）生效，否则 1 格高的块再也拉不长。
- **左端拉伸必须同时改 offset**，只改 `startBeat` 会让文件在新起点重新播放——听感是"剪"
  不是"裁"，而且波形（按窗口画）也跟着跳。
- **窗口上界要用 `clipAudio()` 解析出的 buffer**（挂载槽的），不是块自己的 `audio`：
  块挂到别的 sampler 之后"最长能有多长"由那个文件决定。
- **裁尾时上界别用 `ceil` 到网格**：丢块时长度是 ceil 过的，用精确值当上界会让第一次拖尾
  就偷偷缩不到一格。取 `beatsForSeconds(remaining, bpm)` 原值，误差在亚格内不可见。
- **自检里 `simulateXxx()` 必须先拍快照再改**：`applySampleTrim(origin, …)` 拿 origin 算，
  用 `*findSample()` 当 origin 会让"改过的值"参与下一帧的数学。
- **`setMixedSelection` 要按模型校验 id**：marquee 矩形可能覆盖已被删除的块。
- **聚光灯下"选中"看不见**：`drawFocusHighlights` 的实芯环正好压在选中白环上，且遮罩在
  drawNotes 里就画完了（选中环也一起被压暗）。已选中音符只画光晕 + 左耳。
- **快照里的选中要挑"被聚光灯重画"的音符**：demo 的 melody 前 6 个音符在 Synth A 上，
  被遮罩压暗，选它们拍出来什么都没有；要选组 A（挂 Synth B）范围内的。
- **`-clip` 特写要临时 `focusSynth(-1)`**，拍完恢复，否则 150 px/beat 的图仍然是灰的。
- 出图裁剪按**真实像素**算（1872×1066），按屏幕显示尺寸算会裁到隔壁区域。

## P30 高亮不再压暗 / 波形固定比例 / 恢复原始长度
- **聚光灯遮罩已删**，别再按"灰色 46% 遮罩"的思路加回去。想强调某个音源就加**正向**标记
  （描边 + 薄染 + 左竖标），薄染 alpha ≥ 0.30 才看得见，但槽色和图层色撞车时仍然很淡
  （Synth A 靛蓝 vs Layer 3 紫罗兰），别指望单靠颜色分辨。
- **`updateRackRoutingFocus()` 曾经在有选中音符时写 `focusedSlotId`**：那不是"跟着选择走"，
  那是"点一下就进入模式"。P30 起只留 `applyFocus()`。想还原这个功能先想清楚出口。
- **chip 挪右下角**：右上角放采样块/高音区，压住的是要看的东西，而且吞点击。
- **波形比例不许依赖块宽**：`secPerPixel = (60/bpm)/beatWidth`。把窗口映射到块宽 =
  一裁剪整幅画面重画，用户看到的是"声音没变但波形变了"。`sampleWaveColumns()` 只做
  `min(文件列数, 块宽)`。
- **每个像素取该列覆盖的 peaks 最大值**，不是"抽一个桶"：否则每次缩放都换一个形状。
- **拖动尾巴的 ceiling 不能低于起点**（`jmax(origin.lengthBeats, maxLength)`）：拖入时长度
  向上取整到网格，比文件长一点，按文件长度钳会让第一次按下就自己缩进去。
- **"恢复"要连位置一起恢复**：只把 `lengthBeats` 拉回全长、不动 `startBeat`，音频会整体右移
  ——那是剪切不是撤销。`startBeat -= beatsForSeconds(offset)` 之后 offset 才能归零。
- **自检断言"不变亮/不变暗"要真的量像素**：`createComponentSnapshot` 两次 + 平均亮度。
  `getGridArea()` 是 private，用 `img.getBounds().reduced()`。
- **`--snapshot` 主图会带上 demo 摆位的副作用**：`showDemoRoutingOverride()` 以前会顺手
  `focusSynth(otherSlot)`，图里就永远挂着高亮 chip。摆位函数里不要留"相机外的状态"。

## P31 标尺锚点 / 恢复原长 / 伸缩三模式
- **缩放锚点必须在 mouseDown 捕获成一个值**，不能每帧用 `beatAtScreenX(anchorX)` 重算：
  `clampScroll()` 一旦生效，重算出来的锚点会一格一格往时间线里走，拖得越久偏得越多。
- **滚轮缩放 ≠ 标尺缩放**：前者钉视图中心（`setBeatWidth()`），后者钉按下点
  （`setBeatWidthAround()`）。两者共用 `setBeatWidthAround`，别再复制一份数学。
- **时间线两端锚点会让位**：`anchorBeat` 在视图左侧且缩小拍宽时 `scrollBeats` 可能为负，
  被夹到 0，锚点漂移是正常现象。自检要放在能双向滚动的位置，否则断言测的是钳位。
- **"恢复原长恢复不回去"的根是投放时 ceil 到网格**：块长 4.25、文件 4.04，恢复给 4.04，
  用户看到的是"短了一点"。P31 起 `addSampleAt()` 用文件精确长度，块可以落在非网格拍上。
- **恢复路径不许吸附**：`restoreSampleLength()` 里一个 `snapBeat()` 都不能有。裁尾是吸附的，
  所以"恢复"必须是整个块生命周期里唯一不受吸附影响的那一步。命令还作用于**整个选择**。
- **`maxSampleLengthBeats()` 的 `jmax(origin.lengthBeats, ...)` 别再改回去**：那是 P30 的
  防自缩补丁，P31 换成精确长度后它仍然管用（块被别的路径拖长时）。
- **颗粒伸缩不建状态**：`clipstretch::read()` 是 `(t) → 一帧` 的纯函数，
  `k = floor(t/hopOut)`、`p` 由 t 推出，所以跳播放头、离线导出都不会和实时播放分叉。
  双读头权重用 smoothstep（两端斜率为 0），两个权重恒和 1，否则会周期性起伏。
- **`srcPos >= 文件长就 break` 的循环已经删了**：变速后"块结束"和"文件结束"是两件事，
  越界读返回静音即可。别把 break 加回来。
- **波形两种画法必须同源**：`sampleWaveFillsBlock()` 同时喂 `sampleWaveColumns()` 和
  `drawSampleBlock()`；一处改另一处不改，就会出现"画着整段、听着半段"。
- **徽标宽度用 GlyphArrangement 量**（`Font::getStringWidthFloat` 在 JUCE 8 已废弃且量不准
  CJK）；放不下就整块不画，不要截断成半个字。
- **`--snapshot` 的快照帧不能靠 `setScrollBeats()` 摆机位**：壳子的布局一跑就会重新
  fit 拍宽。摆"块"而不是摆"相机"——把块放在默认视图就能看见的拍位上。
- **看图的工具会缓存**：同一路径第二次 `ViewImage` 可能给旧内容。核对像素时用**新文件名**
  再裁一刀（ffmpeg `-update 1`，路径固定时会报 "Cannot write more than one file"）。

## P32 走带落点 / 采样槽引用
- **停止不能只调 `AudioEngine::stopTransport()`**：引擎的语义是"归零"（离线导出靠它），
  MainComponent 必须**紧跟一句 `setPositionBeats(target)`** 把它摆回本次播放起点。
  两句之间不重绘，用户看不到归零那一瞬。
- **起点只在 `!isTransportEngaged()` 时记**：暂停中按播放是"续播"，不能把锚点挪到现在。
- **删除暂停键后键盘没有"播放"了**：空格 = 停止是用户明确要求的。别"好心"把空格改回
  play/pause，那会让"停止回到起点"变成猜谜。
- **`getPauseButtonBounds` 这类函数删掉就真删掉**：留着不用的 `pauseGlyph` 会 C4505，
  而 continue 用它画一个看不见的按钮更难查。
- **删除采样槽只有一条路**：`MainComponent::removeSlotAndItsClips()`。绕过它直接
  `rack.removeSlot()`，块就会留在钢琴窗里指着不存在的槽（dangling mount），
  声音回落到图层默认——用户以为采样被替换了。
- **顺序是"先删块再删槽"**：反过来的那几毫秒里，每个 Listener（音频、面板、撤销）都会
  看见一个指向空气的挂载。
- **`deleteSamplesMountedOn()` 不许回调 `onSampleReferencesChanged`**：宿主正在删槽，
  回调会再进一次 prune → `rack.removeSlot` → 通知 → 重入。
- **`pruneOrphanSamplerSlots()` 必须 guard**：`removeSlot` 会 notify，而这条链是从通知进来的。
  用 `ScopedValueSetter<bool> pruningSamplers`。
- **`setSampleSelection()` 必须无条件 `notifySelectionChanged()`**。P28 只在"顺手清掉了音符选择"
  时通知，于是**第一次点块不亮卡**（音符选择本来就是空的），第二次才亮。
- **高亮只认 `viaSampler`**：块 `outputSlotId < 0` 时它放的是图层默认输出，那通常是一台乐器。
  点亮乐器卡等于宣称"卡里装着这段音频"，是谎话。
- **`mountSelectedSamplesToSlot()` 的早退不能省**：已经挂在该槽、且 `audio` 已经是槽的 buffer
  时必须直接 continue。否则"点一下自己那张卡"会把裁剪过的块还原成全长——一次查看动作
  变成一次丢失编辑。
- **批量换挂载后再统一 prune**：`mountSelectedSamplesToSlot` 内部只通知一次，宿主在循环外
  prune。逐个 prune 会把第二个块还指向的旧槽提前删掉。
- **自检里的中文一律 `utf8()`**：`juce::String ("卡 ")` 编译过、跑得过，只是日志里显示
  `å¡ 5`。P31 的老坑，P32 又踩了一次。
- **出图帧 `-sampler` 要自己 `updateRackRoutingFocus()`**：`setSampleSelection` 只是把状态改了，
  真正推导焦点的是宿主。快照函数是宿主，所以它得自己调。

## P33 清扫不变量 / 投放选中 / 走带开关 / 树默认折叠
- **"回调式规则"只在被接线的那条路上成立**。P32 的"块没了→槽没了"挂在
  `deleteSelectedSamples()` 里，而块消失的路有三条半（Delete、右键删除、Ctrl+X、撤销、
  代码直接改模型）。P33 把它挂到 `layersChanged()` / `notesChanged()`：
  **凡是能改模型的动作都会通知**，这才是规则的落点。加规则时先问"这条路上有没有通知"，
  而不是"谁记得调我"。
- **清扫必须能重入**：`removeSlot` 会 notify，而这条链是从通知里进来的；再加
  `restoringHistory`（撤销分两步：层、机架）与 `suspendSamplerPrune`（`buildSession` /
  `project::load`）。少一个护栏，就在"新层 + 旧机架"的窗口里删掉正要恢复的槽。
- **拖音符时 `notesChanged` 每帧都发**：清扫要有空机架快速通道（先扫槽找 sampler，
  没有就直接返回），否则每个鼠标移动都要遍历所有块的 `outputSlotId`。
- **剪切→粘贴不是复制**：切走立刻清扫，剪贴板的 `outputSlotId` 变成悬空 id；粘贴必须
  `rack.addSampler(audio, path, name)` 重新登记。悬空挂载不会报错，只会静默回落到图层默认
  乐器——"采样声音变了"但屏幕上一切正常。
- **投放必须自己选中**：`addSampleAt()` 末尾 `setSampleSelection({newId})`。不选中时
  Delete 静默无效（用户以为没投上），卡片也不会亮（高亮跟着选择走）。
  快照帧 `-sampler` 因此**不再手动** `updateRackRoutingFocus()`——它拍的是自动链路。
- **走带别留第二个"从这里播"**：P32 的停止=回起点让停止键与播放键功能重叠，还顺手把键盘
  变成只能停不能播。P33：`stopDecision(engaged)` 两支（跑→归零 / 停着→不动）、
  空格 toggle、回车永远回零。`playStartBeat` 这种东西一旦没用就必须删，留着会被下一阶段
  当成"还没实现"。
- **折叠状态的默认值决定 store 里存哪一半**：存"折叠的"→默认全开（扫大库当场爆炸）；
  存"展开的"→默认全折。改语义时旧 store 里的旧标记（`!collapse`）必须**跳过**，
  否则会被当成文件夹路径去扫描。
- **`setSubFolderExpanded` 要能 `persist = false`**：快照/自检里展开一个临时目录，
  写进真实 store 就是给用户塞一行 `!expand C:/Users/.../Temp/...`。
- **树的自检要显式展开分支**：默认折叠之后，"嵌套行"的断言必须先
  `setSubFolderExpanded(first/nested/deeper, true, false)`——三层都要，少一层则在叶子
  那一级找不到文件行（`deep.wav` 在 `Deeper` 里）。

## Phase 34：给自动删除加闸门
- **对话框不能塞进执行器**。`removeSlotAndItsClips` 是执行器，`pruneOrphanSamplerSlots` 在
  自检里直接调它——把 `AlertWindow` 放进去，自检当场挂死（无头，没人点）。闸门在外面
  `requestRemoveSlotAndItsClips`，执行器保持纯净。
- **清扫是从通知里进的，通知又来自拖拽**。在 `notesChanged` 里同步弹模态框会吞掉 mouse-up，
  拖拽卡住在半路。所以清扫的提问走 `MessageManager::callAsync`，`prunePromptQueued`
  保证一次只有一个问题在飞。
- **`runSelfTest` 必须把 `askSamplerCascade` 装回去**。脚本答案是个捕获局部变量的 lambda，
  留在成员里 = 自检出栈后悬空引用，而且整个 app 从此不问就删。
- **删块会发通知**：`deleteSamplesMountedOn()` 里 `stack.notifyNotesChanged()`，
  那一刻卡片正被持有却已无人引用——清扫会把"下一行就要删掉的槽"当成孤儿再问一次。
  `removeSlotAndItsClips` 因此也要 `pruningSamplers` 守卫。
- **"保留"不记住就是骚扰**：清扫在拖音符时每帧跑，第二次问同一个问题等于没问。
  但"保留"也不能永久：块重新挂上该槽就解除，槽被删就清掉（id 复用会继承豁免）。
- **别问做不到的事**：`getNumSlots() <= 1` 时最后一张卡删不掉，问了也会被拒。
  候选数还要夹到 `numSlots - 1`——批量删两个槽，第二个会被同一规则拦下。
- **快照拍不到模态框**：`createComponentSnapshot` 只拍传入的组件。要留证据就**构造但不显示**
  一个 `AlertWindow` 再拍它（`createSamplerPromptSnapshot()`）。文案必须和真实提示同一个
  builder，否则图会慢慢变成"没人被问过的问题"。

## Phase 35：组是"这些成员"
- **`getGroups()` 只给 const 引用**（`const std::vector<NoteGroup>&`）。想改组必须
  `findGroup(id)` 拿指针——`getGroups().front().setMuted(...)` 根本编不过，别在快照函数里
  写成一行委曲求全的 copy。
- **同一个字符串要有同一个 builder**：组 pill 的文字原先在 `getGroupLabelBounds`（量宽）和
  `drawGroupLabels`（画）各拼一遍，前者算了 `+ 采样数`、后者漏了，于是"为不存在的字预留了宽度"。
  P35 收成 `groupLabelText(layer, group)`，加"· 静音"时才没有变成第三处。
- **点选语义 vs 整组语义**：`setSelection`/`setSampleSelection` 互相清空是**点击**该有的样子；
  "选中整组"必须走 `setMixedSelection`。别为了让整组能选就把互斥删掉——那样单击一个块会留下
  一堆音符仍然选着。
- **静音别在渲染循环里分支**：`GroupMix::gainAt()` 里 muted 直接返 0，音符和采样块共用这一个
  判断。散在两地写 `if (muted)` 迟早有一处忘记，于是"静音的组还有一块在响"。
- **截图核查要先确认在看哪一帧**：同一个文件名在不同 build 下滚动位置会变，
  `createComponentSnapshot` 里的坐标是**组件局部**坐标（钢琴窗在壳内还有偏移）。
  照着旧图裁出来的"文字被截断"是假的——先用像素扫描定位金色像素的真实范围，再裁。
- **demo 也要有一个混合组**：不把块塞进 组 A，这个特性的三张图就都拍不出东西；
  对应加了"demo 里组 A 含 8 音符 + 1 采样且框架包住块"的断言，防止以后改 demo 时悄悄丢掉。

## Phase 36：三层叠加
- **`--snapshot` 出图可能写不进去，且不报错**。若 `docs/phaseNN-*.png` 被 ViewImage 或 ffmpeg
  打开过，Windows 上 `createOutputStream()` 会静默失败，文件保留**上一次**的内容，mtime 却会变。
  症状：改了代码、自检全过、图的配色/文字还是旧的。**先 `rm -f docs/phaseNN*.png` 再跑**，
  并核对文件大小（内容变了大小通常也变）。
- **ViewImage 会缓存，且换同目录新文件名也可能命中**。可靠做法：转存到 `%TEMP%`
  （`cp x.png /tmp/y.png`，再用 `cygpath -w` 拿真实路径）后查看。
- **不要用像素统计去判断配色**：虚线是 `createDashedStroke` + `fillPath`，抗锯齿后颜色是
  "alpha × 覆盖率"的混合值，预测值和实际差很远，容易得出"没生效"的错误结论。
  要验证就打印运行时值（`colour.toString()`）。
- **`Layer::getNotes()` 只给 const 引用**。快照里要写音符的曲线，必须先收 id 再
  `layer->findNote(id)`，否则 `chosen->envelope = ...` 直接编不过。
- **`LayerStack::notifyLayersChanged` 是 private**，外部改完模型用 `stack.touchLayers()`。
- **UI 里画叠加不能自己乘**：图例数字与合计曲线都调 `automation::stackLevel`，否则迟早漂。

## Phase 37：粘贴的落点
- **"先吸附再加法"等于没吸附**。老代码 `anchor = snapBeat(anchor); if (落在源位) anchor += length;`
  第二行把第一行的结果顶出网格：长度 0.375（1.5 个 1/16 格）→ 落点 400.375。
  凡是"让位/前进"都必须 `ceil` 到网格（`snapUp`），不能复用 `snapBeat` 的 `round`——
  round 会把 400.375 拉回 400.25，甚至退回源位内部。
- **粘贴的锚点只要读同一个来源，连按就是叠一起**。playhead 不动 → 每次 Ctrl+V 落同一处，
  用户看到的是"前面的粘贴被吃掉了"。必须记 `samplePasteNextBeat`，并且**用 playhead 是否
  变化来判定 streak 是否继续**（移动 playhead = 用户指明了新地方）。
- **`copySamplesToClipboard()` 必须重置 streak**，否则新剪贴板的第一次粘贴飞回上一轮尾部。
- **自检里改剪贴板前先 `setSampleSelection({src})`**：上一次 paste 会把 selection 换成新块，
  忘了这一步就是在复制上一轮的产物，断言值会莫名其妙地对不上（我第一版就栽在这）。

## Phase 38：按钮的门 / VST 卡片
- **同一个谓词散在四处，改三处等于没改**。P35 让 `groupSelection()` 认采样、也让 G 键认了，
  却漏了工具条的**点击门**（`if (!selection.empty()) groupSelection();`）和**绘制门**
  （`enabled = !selection.empty()`）。症状很怪：按钮是灰的、点了没反应，但按 G 能建组。
  修法是加 `hasAnySelection()` 并让四处 + Ctrl+D + G 键全调它，自检断谓词而不是断像素。
- **UI 直接读 `slot->name` 会让"替换音源"看起来没生效**。`name` 是槽位身份（"Synth A"），
  `pluginName` 才是装了什么。改了 `pluginName` 而卡片还画 `name`，用户看到的就是"点了没反应"
  （只有 pill 从 SAW 变成 VST3 · 插件，那个 9px 的小字没人盯）。
  收成 `SynthSlot::getDisplayName()`，派生而不是写回——写回就再也换不回原名了。
- **`openPluginEditor` 存在 ≠ 能打开**。P12 就写好了，但只有混音台 insert 菜单调用它，
  机架上的 VST 乐器根本没有入口，参数全锁在默认值。加菜单项时 enabled 条件要与
  `openPluginEditor` 内部的前置条件一致（`isLoaded` ↔ `getProcessorForEditor`），
  否则点下去静默失败。
- **自检里的中文字面量必须 `utf8()`**，包括 `check()` 的名字。漏了不会编不过，只会在
  `--snapshot` 日志里显示成 `ç¼ç»`。规律：写的时候没报错，就不用指望它会提示。

## Phase 39：按键发声
- **note-off 里没有音源 id，先查 slot 再处理就等于把它扔掉**。我第一版 `applyLiveEvent`
  开头就 `if (slot == nullptr) return;`，而 note-off 的 `slotId` 是 -1（键不知道自己是哪个
  音源开始的）→ 一半事件被静默丢弃，自检里表现是"松开后声音还在"（voices 2 / tail 0.18）。
  改法：`liveHeldNotes` 记 `pitch → slotId`，note-off 先查表；释放动作放在 slot 查找**之前**，
  这样音源被删了也照样松手。
- **不要从消息线程读 voice 池**。`allLiveNotesOff()` 原来遍历 `liveVoices` 给每个音发
  note-off——那是音频线程的状态。改成一条 `LiveEvent::allOff`，由音频线程展开成 note-off。
  顺带的坑：展开时把新事件**追加到 `pendingLive`**，因为 `beginLiveBlock` 的循环读的是
  `pendingLiveCount` 成员，追加的事件同一趟就被处理（不要写成先把 count 存成局部变量）。
- **一个 block 里处理同一个插件两次，不是双倍输出，是双倍 DSP 速率**。走带在跑时插件已经在
  `renderHostedInstruments` 里 processBlock 过，`renderLive` 再处理一次，LFO 和包络会快一倍。
  所以要把 live MIDI **合进同一个 `pluginMidi`**，并记 `transportPluginSlots` 让 live 路径跳过。
- **KeyListener 先于组件的 keyPressed，普通 keyPressed 在后面**。钢琴窗的 `G`（编组）会吃掉
  音符键，所以把 `TypingKeyForwarder` 挂到 panel 上而不是只在 MainComponent 里判断。
  反过来说：`Ctrl` 组合必须先放行，否则 `Ctrl+S` 变成音符。
- **JUCE 没有 key-up 事件**。`keyStateChanged` + `isKeyCurrentlyDown(keyCode)` 是唯一可靠的
  松手检测，而且它**跟父链一起往上走**（`ComponentPeer::handleKeyUpOrDown`），所以子面板
  不处理时会走到 MainComponent。再加 60 Hz 定时器兜底：焦点切走的瞬间松手不会留卡音。
- **MIDI 回调在 MIDI 线程**，不能在那里碰 `RackModel` / `LayerStack`。目标音源存成
  `std::atomic<int> liveTargetSlot`，由 `applyFocus()` 和定时器在消息线程写。
- **`AbstractFifo` 是单生产者**，而这里有两个（MIDI 线程 + 消息线程）。生产者侧加
  `SpinLock`，音频线程照旧不加锁——这条和 P36 的"发布期锁只盖 shared_ptr 赋值"是同一类边界的另一半。
- **设置页不能自己开设备**。P8 的 MIDI 页 `openDevice` 自己开、自己 `stop`，那在"只画监视器"
  时没问题；一旦声音要从这条线出来，两个所有者必然有一个是哑的。设备归 `MidiInputHub`，
  设置页只是视图（`getDevices()` / `setDeviceOpen()`）。

## Phase 40：默认最大化 / 双开关
- **`setBounds(userArea 减 frame)` 只在 100% 缩放下看起来对**。`userArea` 是**逻辑**单位，
  `peer->getFrameSize()` 回来的是**物理**像素（本机 1.25×：titleBar 报 30，换算逻辑约 24），
  两者相减再 `setBounds`，窗口就比工作区矮一截/宽一截——"最大化"了但四周露桌面。
  而且这种窗口只是**长得像**最大化：系统认为它是普通窗口，还原按钮不亮。
  正解是 `setFullScreen(true)`（Windows 上就是 `ShowWindow(SW_SHOWMAXIMIZED)`，标题栏留着），
  还原尺寸用 `centreWithSize` 提前放好。**这类几何问题不要靠截图判断**，跑 `--windowtest` 看数字。
- **同一个概念不要留两套 API**。P39 的 `MidiSource` 三态一旦改成两个开关，`setSource` /
  `getSource` / `usesExternalController` 就必须一起换（`setSources` / `getSources`）；
  留一个 `getSource()` 做兼容，第二天就会有人在它上面写"等于 typingKeyboard 才算开"。
- **"切换来源就释放全部"在双开关下是错的**。开控制器时把键盘的和弦掐掉，用户听到的是
  自己没做的事。规则要按**方向**拆：关掉的来源释放自己的，打开的来源不动别人。
- **设置页两块同屏时，布局必须算一次存下来**（`plan()`）。绘制和命中测试各算一遍，
  迟早出现"看得见点不到"（尤其是行数被裁掉之后）。
- **`Rectangle::expanded` 没有四参数重载**。要按四边扩，用 `BorderSize<int>(t,l,b,r).addedTo(r)`。

## Phase 41：VST 音源不松手（选中外部音源后一直响）
- **"记下来"写在分支里面 = 只有一个分支记得**。P39 把 `pitch → slotId` 写在振荡器分支末尾，
  而插件音源在 `slot->pluginId > 0` 那里就 `return` 了——它的 note-on 从来没被记下，
  note-off 带着 `slotId = -1` 进来，`slot == nullptr`，落到 `if (!event.on) return;` 被丢。
  听感就是"选中 VST 卡片后，按一下电脑键盘就一直响"。规则（每条 note-on 都要记住自己的音源）
  要写在**分叉之前**，写在任何一个分支里都是把规则降级成实现的副作用。
- **卡音会自己"停"在 6 秒后，看起来像另一个 bug**。插件槽有 `tailFrames`（6 s）逼它继续渲染，
  尾音一过就不再 processBlock，但 `numHeld` 还挂着那个键，于是 `renderLivePlugins` 永远不敢
  回收这个槽（回收条件是 `numHeld == 0 && tailFrames <= 0`）——`livePluginEngaged` 永远是 1，
  `hasLiveNotes()` 永远为真，音频回调一直满速跑。所以判断要断**两处**：松手当块是否静音，
  以及尾音过后槽有没有还回来。
- **断言要能咬人，先把它咬一次**。这次把 `if (event.on)` 临时改成 `if (false && event.on)`
  重编译，两条断言立刻 FAIL（peak 0.0396 / 仍然占着），再恢复。新增断言如果不做这一步，
  "PASS" 只证明代码跑了，不证明它测到了东西。
- **`Atomic Test Synth` 让插件路径可测**：无头自检里它是一台真 VST3（`Tools/TestPlugins/TestSynth.cpp`），
  一个正弦 + 闸门，note-off 当块就静音。所以"note-off 到底有没有送到插件"可以用**音频**断言，
  不必给引擎加诊断 API。

## Phase 42：插件窗按键 / 创建自动化母菜单 / 音源参数自动化 / 电平表落零

**一、按键不走父链，走「谁有焦点」**
- `PluginEditorWindow` 是**独立顶层组件**。它的按键事件从自己的 peer 下来，
  一路只往上走到自己的 content —— 永远到不了 `MainComponent`，因为不在同一条父链上。
  所以"给 MainComponent 加个 `KeyListener`"没有任何用，必须**窗口自己装**转发器：
  窗口一层 + `getContentComponent()` 一层（焦点可能在插件自己的某个子组件上，而那个子组件
  未必是窗口的直系，中间只要有一层不往回传就断了）。
- 转发器要**复用同一个键表**，不能另写一份。做法是 `MainComponent::handleGlobalKeyPress()`
  转手调 `keyPressed()`——键表只有一个出处，插件窗口前台和主窗口前台的映射不可能走岔。
- **`juce::TextEditor` 必须放行**，否则在插件界面里输入预设名会变成弹音符。
  判定用 `Component::getCurrentlyFocusedComponent()` 再 `dynamic_cast<TextEditor*>`
  （插件的文本控件未必是 JUCE 的 `TextEditor`，本机 `Atomic Test Synth` 没有文本框，
  所以这一条只验到"JUCE 文本框放行"，第三方插件的自绘输入框未测过）。
- `juce::KeyPress` 没有从 `int` 隐式转换的构造可以拿来当 `const KeyPress&` 用；
  自检里写 `forwarder.keyPressed (juce::KeyPress (juce::KeyPress::spaceKey), nullptr)`。

**二、「创建自动化」是动词，参数才是内容**
- 菜单项名称里带"创建自动化 · XX"的平铺写法，在卡片、通道条、编组、钢琴窗、自动化窗
  五个地方各长一遍，加第三种目标就要改五处。收成 `菜单 ▸ 创建自动化 ▸ XX` 之后，
  **名字只出现一次**，参数（`slotId` / `idBase`）由调用方给。
- 但 id 是扁平的：`PopupMenu` 里所有叶子项共用一个 `result` 空间，母菜单不占 id。
  所以 `addSlotAutomationItems()` / `applySlotAutomationChoice()` 必须**成对**提供，
  **基址必须避开已经用掉的区间**。P42 加了 `kSlotAutoParamBase = 10000`，
  因为卡片菜单已经占掉 `10..13 / 20 / 30..41 / 200+n / 1000+channel`。
  先数一遍现有的 id 再用，别顺手拿 100。
- 子菜单里"没有可选内容"也要留一条**禁用项**（"这个通道还没有音源"），
  空白子菜单在 JUCE 里点开是一片虚无，用户会以为是坏的。

**三、多 lane 之后，「谁最后被读到谁是选中」不再成立**
- P42 把 `SynthSlot` 的 lane 从 2 条加到 3 条（`filter` / `level` / `plugin`），
  读取端原来的规则是"遍历三条，`expanded` 的那条算选中"。三条都写出来之后，
  **最后读的那条恒赢**，于是老工程读回来 target 全变成 `plugin`，回归立刻红。
- 规则：**选中 lane 是状态，必须显式写**（`<Slot autotarget="...">`），
  不能靠元素顺序或某条 lane 的属性**推断**。只有在文件里没有这个属性（v17 及更早）时，
  才回退到旧的推断规则——**版本判断写成"属性缺失"而不是"版本号 < 18"**，
  因为手写/迁移的文件版本号不一定老实。

**四、电平表是读数，不是最大值**
- 所有渲染路径都用 `jmax` 往表里存块峰值（这是对的：块内峰值不能被平均掉），
  问题在**没人负责让它下降**——原来只有"完全空转"那条早退分支衰减，
  于是走带在跑但没声音（光标停在空白处、音符都放完了）的时候，表永远钉在最后一帧。
- 正解：**音频回调开头无条件衰减**，按 `-50 dB/s × 块时长` 用 `std::pow (10.0, -2.5 * dt)`。
  用 dB/秒 而不是"每块乘 0.9"，是因为后者与缓冲大小、采样率都绑死——
  512 帧和 256 帧落得一样快，才说明它真的是一条时间曲线。
- `decayMeters(numSamples)` 取**实际块长**，所以必须从回调里传进来，
  不能像原来那样 `decayMeters(0.90f)` 写死一个因子。
- 重绘要**有条件**：`hasMeters()`（遍历所有表，任一 > 1e-4 即为真）落零就停，
  否则 60 Hz 定时器会把一个静止的窗口永远判成"还在响"，白烧 CPU。
  同时这条也是"电平表已经归零"的可断言证据。
- 窗口侧的门是 `if (audio.isPlaying() || audio.getPlayback().hasMeters())`，
  放在 `if (! audio.isPlaying()) return;` **之前**——放在后面等于只有播放时才更新，
  正好漏掉"停止后还在落"的那一段，而那段就是用户唯一会看的。

**五、一次断言咬两条命，先咬一次再信**
- P42 新增的 7 条里，"曲线真的压参数"和"电平每秒 50 dB"都是**结构性**结论，
  很容易写成"代码跑了就 PASS"。做法同 P41：把 `-2.5` 临时改成 `-0.25`、
  在 lane 发布条件后面加 `|| true`，重编译后立刻 3 条 FAIL
  （`发布参数 -2 / 前后半比 1.00 → 1.00`、`512 0.56284 / 应为 0.00316`），再恢复。
  没有这一步，"412 PASS" 只说明 412 段代码没崩。
- 断言里的**比例**比绝对值稳：插件参数那条断言用"前半段 / 后半段"的比值（1.00 → 0.39），
  不用绝对电平，这样换了测试音源、改了增益都不会误报。

## Phase 43：混音台按键 / 文件栏 / 琴键音源 / 效果器换位置 / 四控件菜单

**一、同一个"规矩"写在两个地方，就一定会在某一刻说两种话**
- "裸按键打向哪台音源"这件事原来有两份实现：`MainComponent::getLiveTargetSlot()`
  （优先 `focusedSlotId`，跳过 sampler）和 `PianoRollComponent::auditionPitchOnActiveLayer()`
  （直接 `routing::layerSlotId`，只看图层）。两份只在"选中了 VST 卡片"这一种情况下不同——
  而那正是用户唯一会去点的地方：电脑键盘弹 VST，钢琴窗琴键弹内置合成器。
- 修法不是"把琴键也改成优先 focus"，而是把规矩搬进 `routing::auditionSlotId(focusedSlotId, layer, rack)`，
  两个调用方都调它。**判断标准**：如果两处代码在描述同一个问题，那么第二处出现的那一刻就已经错了。
- 断言写法：临时把琴键改回 `layerSlotId`，立刻 FAIL `(琴键 1 / 键盘 10 / 图层默认 1)`——
  三个数字并排，"两台设备问同一个问题得到两个答案"是自证的。

**二、顶层窗口的按键：第二批**
- `MixerWindow` 和 `PluginEditorWindow` 一样是独立顶层组件，按键不走父链。P42 的结论
  这次直接照抄：窗口 + `getContentComponent()` 两层装同一个 `KeyListener`，
  转发器复用 `handleGlobalKeyPress()`，`TextEditor` 放行。
- 复用**同一个转发器实例**（`pluginForwarder`）。两个实例就是两张键表，迟早有一张忘了改。

**三、id 空间是扁平的，加一项之前先数一遍**
- 给 `addSlotAutomationItems` 加声相/静音/独奏时，最自然的是 `idBase + 3/4/5`。
  而机架卡片传的 `idBase = 30`，**34 早就是"高亮"**——于是"创建声相自动化"会点亮聚光灯。
  自检当场红：`The rack's 高亮 item is a toggle now (没亮)`。
- 教训：给同一个生成器配不同 `idBase` 时，**新 id 必须落在与 idBase 无关的高位段**。
  P43 用了 `kSlotAutoTargetBase = 20000`（参数段是 10000），并且**先判断高位段再判断参数段**，
  否则 `20030` 也会满足 `>= 10030`，被读成"第 10030 号参数"。

**四、solo 是唯一全局性的那个控件**
- 音量、声相、静音都是"自己的事"，可以写进字节里的一个数（`gainAt` / `panAt` / 闸）。
  唯独 solo 的答案是**全局的**：只要有人 solo，别人就没声。publish 时能冻一个 `anySlotSolo`，
  但一条曲线会让这个布尔量随时间移动。
- 所以 `isAudibleAt(beat, anySoloNow)` 每块问一次，`anySoloNow` 在块首用 `isSoloedAt(startBeat)`
  扫一遍槽位。**不要**为了省这一次扫描而写两条分支（有曲线/没曲线）——两条分支就是两个答案。
- 副作用：`gainAt()` 里不能再乘 `audible`，否则"别人的静态 solo"会和"自己的自动化 solo"打架，
  两个都生效、谁也不知道谁对。`audible` 只留给没有 beat 的地方（live 演奏、试听）。

**五、菜单要能被断言，就不能只在鼠标里存在**
- `showControlMenu` 拆成 `fillControlMenu`（填菜单）+ `showControlMenu`（挂回调并弹出）。
  自检用 `juce::PopupMenu::MenuItemIterator` 读每一项：文本、是否子菜单、是否夹带了
  "通道 / 删除 / 重置"。**"右键菜单里不再有通道菜单项"是一句关于列表的话**，
  用截图只能证明"我看见的那一眼没有"。
- 同理，拖动改顺序的断言落在**音频**上：驱动→低通 与 低通→驱动 是两段不同的 RMS（这次差 9.6%）。
  只断 `getInsert(0).type` 的话，一个"换了数组但没接上渲染"的实现照样全绿。

**六、点击和拖动共用一个按下，只能靠 mouseUp 收尾**
- 插槽标签原来在 `mouseDown` 里直接弹菜单。要同时支持"按下拖动换位置"，就必须改成：
  `mouseDown` 只记录（`pressedInsertSlot`），`mouseDrag` 越过阈值就 `moveInsert`，
  `mouseUp` 若一次都没移动过才弹菜单。
- 拖动时的视觉反馈不能省：给被拖的那一行上色（`insertDragMoved && pressedInsertSlot == slot`），
  否则连续滑过四行看起来像四次互不相关的重绘。

## P46-A：JUCE `AudioBuffer(channels, samples)` 构造函数不清零（实测）

**现象**：混响冲激测试每次运行结果不同——有时正确衰减，有时尾巴爆到 1e14，同一 exe 三次三样。
**根因**：`juce_AudioSampleBuffer.h` 的 `allocateData()` 用 `malloc`（不是 `calloc`），
`AudioBuffer (int, int)` 构造后 `isClear = false`，内容是堆上旧数据。
`pre-max` 诊断在处理**之前**就量到 12468 ~ 1e20 的垃圾，坐实与 DSP 无关。
**规矩**：新建 AudioBuffer 后要么立刻 `clear()`，要么逐样本填满全部采样。
"构造即清零"是错的——别凭记忆信 API，实测。
**顺带**：梳状反馈环会把输入里的任何垃圾放大 `1/(1-fb)` 倍（fb=0.9 时 ≈11 倍），
所以混响测试是全工程最灵敏的"脏内存探测器"。

## P46-B：自检汇总行必须打在最后一个 check 之后

**现象**：咬门禁时 3 条 `[FAIL]` 打印出来，退出码却是 0。
**根因**：汇总行（`ALL CHECKS PASSED` / `N CHECK(S) FAILED`）原本打在 runSelfTest **中段**
（fixture 恢复之前），后面还有 Phase 34+ 的检查块。P46 的混响块插在汇总之后——
FAIL 计入了 `failures`，但汇总早已定稿，P44 的退出码门禁读到的是过期裁决。
P44/P45 的咬测能 EXIT=1 纯属缺陷位置恰好在中段之前。
**修法**：汇总挪到 `return report;` 之前。**规矩**：任何"总结"必须覆盖全部条目；
给测试追加代码时先看总结在哪里。

## P49-A：`cmd //c "app.exe … & echo EXIT=!"` 不等待 GUI 子系统程序

`AtomicDAW.exe` 是 WINDOWS 子系统。`cmd /c "A & echo B"` 不会等 A 退出，退出码先打印、
PNG 后写完。于是：

- `EXIT=0` 可能来自上一次运行（咬测时更危险：真失败也可能读到 0）；
- 紧接着的 `ffmpeg` / `grep` 读的是**上一轮**的产物，「改了代码但画面没变」就是这么来的。

**改法**：用 bash 直接调用（bash 会等子进程），退出码用 `$?`：

```bash
build/AtomicDAW_artefacts/Release/AtomicDAW.exe --snapshot docs/phaseNN.png > docs/phaseNN.log 2>&1
echo "EXIT=$?"          # 真退出码
```

**外加一条**：目检前先 `rm -f` 目标 PNG。同名覆盖写入失败时 mtime 会更新而文件内容
仍是旧的（本次实测：图里的文案没跟着源码变，mtime 却是新的）。删掉再跑，
「文件不存在」就是最强的「没写出来」证据。

`std::cout` 在这种程序里也不可靠（没有控制台，缓冲可能在进程退出时丢掉）——
要拿运行时数值，写文件（`juce::File::appendText`），不要指望 stdout。

## P49-B：自检夹具的块长踩在单位换算的恒等点上

采样块 2 拍 @120 bpm = **正好 1.0 秒**。曲线 x 是归一化 0..1，渲染端按秒求值，
`p.time * blockSeconds` 与 `p.time` 在这种情况下**是同一个数**——把换算摘掉，断言全过。

咬测（植入 `p.time`）暴露后改成 8 拍 = 4 秒，并把「比大小」换成「同一窗口开/关曲线的
比值与曲线应有值对齐」（1.0 s 应 0.250、1.6 s 应 0.400）。**规矩**：凡是测「归一化 ↔ 绝对量」
的换算，夹具的系数必须 ≠ 1，否则测的是恒等式。

## P50-A：`withKerningFactor` 的单位是 EM，不是像素

DESIGN.md 第 5 节写的是「字距 +0.5」，照字面传给
`juce::Font::withKerningFactor(0.5f)`，结果是**半个字高的空隙**——走带读数被拉成
`0 2 . 3 . 3`，一个数字一个字宽。JUCE 的 kerning 因子乘的是 `font.getHeight()`，
不是像素。

**改法**：像素语义封装成 `ui::tracked (designPoints, kerningPixels, style)`，
内部做 `kerningPixels / designPoints` 换算。以后凡是想「拉开 0.5 像素」，走这个函数，
不要直接摸 `withKerningFactor`。

## P50-B：排版断言写在了不会失败的地方（又一次踩恒等点）

「等宽数字」的断言拿**默认 UI 字体**量 1 和 0 的宽度——它本来就是等宽的。
把整段等宽绘制逻辑摘掉，断言照样过。P49-B 的教训在排版层又出现了一次：
**夹具必须落在差异上，不能落在相同上**。

**改法**：加一条用**比例数字字体**（Georgia，实测 1=5.29 / 0=7.56）的断言，
要求 `drawTabular` 把 "111" 画成 3 个格子宽（22.69 = 3 × 7.56）。咬测实测
FAIL 15.88≠22.69，断言这才真的是断言。

## P50-C：空图层没有「内容结束位置」，找空位的函数会先早退

`scope::findFreeBeat()` 原本开头就 `if (contentEnd <= 0) return 找不到;`，
空图层的 `contentEnd` 正是 0——于是「往空图层放渲染结果」永远报「没地方放」。

**改法**：删掉这个早退，改用「已占用区间合并后的空隙」判定，并补
`merged.empty()` 分支返回 beat 0。**规矩**：以「最后一个内容结束」为边界判断的函数，
先想一遍空集合的情况。

## P50-D：自检往 `docs/demo.atomic` 里加图层

插入逻辑原本长在 `MainComponent` 的成员函数里，自检只能拿活会话试，
结果每次跑自检都往 demo 工程里塞一个采样块，`demo.atomic` 越跑越胖。

**改法**：把「往图层尾部/空隙插入采样块」抽到 `scope::insertSampleBlock (stack, rack, …)`
——不碰任何窗口、不碰单例。自检用本地 `LayerStack` + `RackModel` 跑同一份代码。
**规矩**：自检要做修改性操作时，把逻辑抽成不依赖组件的自由函数，不要借活会话。

## P50-E：声相中线画在填充之前 = 被自己的填充盖掉

中线和填充共用 `inner.getCentreY()`：填充的基线正好是那条线，
先画线再画填充，线的像素被半透明洗色覆盖，看起来像「填充的上边缘」而不是参考线。

**改法**：把中线抽成 `drawCentreLine` 局部 lambda，**在 fillPath 之后、strokePath 之前**
调用（无曲线点时也调一次）。绘制顺序是一条规则，不是排版偏好：
**底色 → 网格 → 填充 → 参考线 → 曲线 → 端点**。

## P50-F：切割时右手半段的 sourceOffset 不能按窗口长度推进

块比源文件长时（块 3 s、文件 2 s），右手半段的偏移按
`window * u` 算会越过文件尾，听感上是「切完的第二段从静音开始」。真正的量是
**左半段消耗掉的源秒数** = `min (window, elapsedOutput * speed)`。

**实测**：错误写法给出 0.933，正确写法给出 1.4（也就是用户听到的那一刀）。

## P51-A：需求变了，旧断言要跟着死，不然它们会咬活人

P30 立过一条规矩"块不能比文件长"并写了断言；P51 用户把它推翻了。第一次跑自检
6 个 FAIL 里 3 个是**旧断言还在忠实地测昨天的好主意**（上限两条 + 依赖上限才成立的
"拖回文件末尾 +0.03"一条）。

**改法**：改一条规则的当天，grep 它的断言（按 check 名），能改写的改写成新规则，
测不成的删掉。留着就是下次的假警报。

## P51-B：切割函数里复用切割前的元素指针

`block = layer->findSample(id)` → `splitSamplesAt()`（内部 `addSample` 会扩
vector）→ 再读 `block->lengthBeats`：vector 搬家，指针悬垂，实测读出 0.000 拍。
**改法**：凡是会 add/remove 元素的调用，之后一律重新 find，不要信旧指针。

## P51-C：16 px 的图标要先过"和邻居是不是一家人"这一关

刻刀第一版 = 细三角 + 细柄 + 两段虚线，8 倍放大看就是一道斜杠。第二版改用
**橡皮的构造系**（旋转圆角矩形描边）+ 刀尖三角 + 折断槽——同一族、不同细节，
一眼能分辨。**规矩**：新增小图标先和它左右邻居各画一遍放大图再定稿，
"画了什么"不如"和谁区分得开"。

## P51-D：`getSamples()` 永远是 const-only，批量改块走 findSample

P45 的 const 审计把 `getSamples()` 定成 const。之后任何人想"遍历改所有块"
（机架卡拉伸、未来的批量操作）都必须 `findSample (probe.id)` 拿可写指针——
直接 `for (auto& clip : layer->getSamples())` 编译都过不了，这是设计不是疏漏。

## P52-A：命名空间 / 文件名不能比功能活得久

导出模块叫 `WavExport` / `wav::renderToFile`，是 P25 只导 16-bit WAV 时起的名。
P52 让它写 MP3、24-bit、float 之后，这个名字就在撒谎——读代码的人会以为它只管 WAV。
**改法**：当天改名（`AudioExport` / `bounce::`），sed 全量替换 + 逐处过目。名字比注释更常被读；
注释说"其实也导 MP3"救不了一个叫 wav 的命名空间。

## P52-B：MF 的 SampleTime 必须是绝对帧位置，不能逐块累加

Media Foundation 以 100 ns 计时。逐块 `time += duration` 会把每一块的 llround 误差累起来，
长文件尾帧漂移；用 `llround(frameOffset * 1e7 / sr)` 从绝对帧位置算，误差不跨块。

## P52-C：MP3 编码器不在 JUCE 里，在操作系统里

`MP3AudioFormat` 只解码；`LAMEEncoderAudioFormat` 要随包发 lame_enc.dll（没做）。
Windows 8+ 的 Media Foundation sink writer（`MFCreateSinkWriterFromURL` + `MFAudioFormat_MP3`）
就是系统自带的编码器，链接 mfplat/mfreadwrite/mfuuid 三个库即可，实测本机出标准 MP3。
**别**在 MP3 失败时悄悄改写 WAV——.mp3 后缀装着 PCM 是给下一个程序挖的坑，报错就报错。

## P52-D：WAV 头不能只信自己写的

24/32-bit 的头是手写的（fmt tag 1/3 + fact chunk）。自检必须用 FileInputStream 把
字节 20/22/34（format tag / channels / bits）读回来断言，而不是信 `ExportSettings` 结构体——
"我设置了 24"和"文件里真的是 24"是两回事，中间隔着一次序列化。

## P52-E：禁用行的选中态要"保留位置、收敛亮度"

位深/比特率两行永远都在，一行灰一行活。灰行的选中项如果还用全亮 accent 画，
看起来就像"这一行其实能用"。改法：选中色在 disabled 时降到 0.35 alpha、文字用 inkSubtle——
位置保留（切回格式时选择还在），亮度收敛（不再假装可点）。
