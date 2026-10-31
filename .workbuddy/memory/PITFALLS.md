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
  **双击是例外**（P77）：单击仍然点穿，但双击 ghost 音符/采样块会换成它所在的图层
  （`hitTestGhost()` 倒序扫非活动 + 可见层；组里的成员连组一起打开、选中整次出现）。
  查的顺序照抄画图顺序：活动层的采样块 → 音符 → 组框 → 组名，全问完才轮到 ghost。

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

## P53-A：落块即选中（P33）会掩盖"替换目标"的断言

`addSampleAt` 造出来的块自带选中，所以"替换后选中该块"这条断言在单块夹具上**恒真**——
不管替换的是命中块还是旧选区。要看清"目标是命中块还是选区"，必须两块 + 一个陈旧选区：
选中 A，把卡丢到 B 上，断言 B 变了、选区变成 {B}。P53 咬测 3（去掉目标选中）就是这样漏网的，
补了 6b 才抓到。

## P53-B：hitTestSample 返回第一个命中的块，不是最上层

分派测试（simulateCardDrop）如果落在 demo 排布上，命中哪个块由 getSamples() 顺序决定，
测试控制不了。做法：临时 `stack.addLayer` 开一张探针层（空层，鬼层本就点击穿透），
在层上摆夹具，测完 removeLayer 并恢复 activeIndex 与 scrollBeats。

## P53-C：`ui::px` 返回 int，几何别拿它喂 `Rectangle<float>`

`Rectangle<float>::reduced (ui::px (6))` 走 int→float 窄化，MSVC C4244。几何一律 `ui::pf`，
字号才用 `ui::sp`。（`drawText` 的矩形参数尤其容易顺手写错。）

## P53-D：选中态下点卡片 = 挂载选区，不起拖

RackPanel::mouseDown 的 `selectionActive && onSlotChosen` 分支直接 return，拖卡手势在那里
根本不会 arm。这是刻意的：P23 的"点卡片 = 把选区挂上去"和 P53 的"拖卡片 = 单点挂载"
共享同一个卡片正文，鼠标按下那一刻就分道——点击立即生效（无法撤），拖拽松手才生效。

## P54-A：机架空白的真凶是 scrollY，不是没画卡

`drawCard` 明明被调用（stderr 探针看得见），画面却是空的——因为卡片全在 `getViewArea()`
上方。`rackChanged()` 在槽数增长时把偏移设成 `content - viewHeight`，用的是**当时**的
`getViewHeight()`；会话先建内容、后面板才拿到尺寸时，那次计算得到 0，偏移直接等于整份内容，
之后再也没有人钳制它。`resized()` 原来也没有 `clampScroll()`。

修法：`updateLayout()` 开头钳一次（`resized` 与 `rackChanged` 都走它），`setScrollY` 早就自带钳制。
症状值得记住：**空机架 + 按钮还在 = 滚动跑飞**，不是模型丢了。

## P54-B（P55 已修正）：靠"画得不一样"区分模式是错的

P54 的选区标记刻意画成虚线、画在卡内，怕用户把"我选了这些音符"读成"进入了只看这个音源的模式"。
实际后果：同一张卡在两种状态下像两个不同的物体，而且虚线被读成"次一等的高亮"——用户的第一反应
就是"选中音符时没高亮"。**正确的区分维度是暗幕**（聚光灯会把整张网格压暗，选区标记绝不），
不是环的形状。P55 起两者共用 `drawCardRing`，只在聚光灯同时在场时给选区环降 alpha 到 0.55。

教训：两种状态要在视觉上可分，先找**已经存在的那个差异**（暗幕/节头文字），别再造一个图形差异
去承担这个职责——造出来的那个会被当成"做漏了"。

## P55-A：注释里的 `*/` 会提前闭合块注释

把新段落插进已有 `/** ... */` 块时，末尾顺手写了 `*/`，编译器报的是
`NoteEnvelope.h(6,1): error C2059: 语法错误:"namespace"`——离真凶十几行，全是连带。
动注释块只改中间，不要在替换文本里带 `*/`。

## P54-C：自检里"画笔状态"是全局残留

`hasPaintState` 是会话级开关，P11 那批断言会把它点亮。测"没选中时菜单项置灰"之前必须先
`clearPencilState()`，否则测的是上一块的残留——首次跑就吃过一次 FAIL（bare 0 / armed 1 里的 copy 项）。

## P54-D：`std::cout` 在快照分支里可能丢失，探针用 `std::cerr`

headless 快照运行结束时缓冲区没机会刷出去，`std::cout` 打的调试行在重定向日志里看不见；
`std::cerr` 是无缓冲的。别据此判断"代码没跑到"。

## P56-A：组粘贴分两趟 = 组被切成两半

`pasteEditClipboard()` 原来是"先粘音符（建新组）、再粘采样"，而 `pasteSamplesFromClipboard`
里写着 `clip.groupId = -1`（P33 的规矩：粘贴的块不继承源组）。两条规矩各自都对，合起来就是：
**含采样的组粘贴后，音符在新组里，采样散在组外**。用户看到的就是"组被打散了"。

修法是让"整组"成为一个事务：`pasteGroupFromClipboard()` 一个锚点、一次建组、两类成员都写
`newGroupId`。判据是"剪贴板里是不是整组"（`groupClip.valid`），不是"两类剪贴板都非空"。

教训：两个正确的局部规则可以在组合处产生一个错误的整体行为。判据要落在"用户以为的那个对象"
上（组），而不是"我手上有哪些列表"。

## P56-B：自检断言的文本里裸写中文 → 日志乱码

`utf8()` 是唯一入口（MEMORY 硬规则 1）。写成 `check ("复制到后一小节 puts ...", ...)`（中文在
普通字面量里）会把 UTF-8 字节再编码一次，日志里显示成 `å¤å¶å°åä¸å°è`。参数里的中文
（如 `utf8("...")`）没问题，**第一参数（检查名）最容易漏**。

## P56-C：`snap(beat - 1e-9)` 会把 1e-9 吃掉

想表达"严格晚于 4.0"而写 `nextBarLineAfter (last - 1e-9) → snap ×round` 是无效的：
`round((4 - 1e-9) / 0.25) = 16` → 又回到 4.0，于是"下一小节"跳到 8.0。要"不早于结束"就直接
用 `ceil`：`barLineAtOrAfter(beat) = ceil(beat / beatsPerBar - 1e-9) * beatsPerBar`。
一个用 `floor+1`（严格晚于），一个用 `ceil`（不早于）——别用一个函数加 epsilon 去模拟另一个。

## P56-D：`GroupClip` 是私有嵌套类型，声明要放在用它之前

`addGroupCopy(Layer&, const GroupClip&, double)` 写在 public 区，而 `GroupClip` 的定义在文件
后部（private）→ 报 "缺少类型说明符"。解决：在使用点之前写 `private: struct GroupClip; public:`
（前向声明必须与定义同访问级别，否则 MSVC 报 C4240）。cpp 里的返回类型要写全
`PianoRollComponent::GroupClip`。

## P57-A：组框的 3px 外扩是画出来的间隙，靠"挪线"消不掉

用户报"组的边框与采样的边框之间留了距离"。那个距离不是间距属性，是 `content.expanded(3px)`
——外扩原本是为了让组线不被成员盖住（组框画在成员**之下**，不外扩就被块的填充吃掉）。

所以修法不是把 3px 改小，而是**换层次**：填充继续画在成员之下（`drawGroupBands`），
描边单独画在成员**之上**（`drawGroupFrames`，`drawNotes` 之后），几何用 `content` 本身、
向内缩半个线宽。线压在块自己的边框上，"不留距离"才真的能画出来。

附带教训：`groupFrameOf`、`drawGroupFrames`、`getGroupFrameAt` 必须调**同一个函数**。
自检读的是 `groupFrameOf`，如果绘制另写一份 `content.expanded()`，断言永远绿而画面永远错。
咬测就是把 `expanded(3)` 加回去，看那条"零间隙"断言变红（frame 103.8 vs block 96）。

## P57-B：粘贴复用组后，"失败就删组"的清理会删掉别人的组

`pasteGroupFromClipboard()` 原来在"什么都没粘上"时 `active->removeGroup(newGroupId)`。
一旦粘贴改成一件事"复用源组"，这句就成了"粘贴失败顺手删掉用户的组"。清理必须是
**只删这次新建的**：`if (! reused) removeGroup(...)`。凡是复用型 API，清理路径都要先问
"这个东西是我造的吗"。

## P57-C：两条粘贴入口 = 两套语义

`pasteNotesFromClipboard()` 曾经独立处理"组剪贴板"（建新组），`pasteEditClipboard()` 走
`pasteGroupFromClipboard()`。P57 改语义时只改一处，另一处就会把块留在剪贴板上（音符进去了、
采样没进去）。现在 `pasteNotesFromClipboard()` 开头 `if (groupClip.valid) return pasteGroupFromClipboard();`
——一个动词一个实现。自检也因此刻意用**旧入口**粘一次，否则"转发被删"这个缺陷没有任何断言会红
（咬测验证：删转发行 → "组内现 4 音符 / 1 采样" 两条 FAIL）。

## P58-A：`ui::pf(N)` 的像素值不能除以一个"裸 N"去算拍

自检里把 drop 点定在 `grid.getX() + ui::pf (100)`，然后按 `scroll + 100 / beatWidth` 反算拍号
——差了 **0.625 拍**。原因：`ui::pf(100)` 在 `ui::scale = 1.3` 下是 **130**，分子是缩放后的像素，
分母却是没缩放的 100。断言打印 `新起点 2.708（应 2.083）`，看着像 snapping 的错，其实是量纲错。

规则：凡是"像素 ↔ 拍"的换算，两边必须来自同一处。要么都用 `ui::pf()` 的值，要么都反算：
```cpp
const float offset = ui::pf (100);
const double beat  = roll.getScrollBeats() + (double) offset / beatWidth;   // 同一个 offset
```

## P58-B：`check()` 的说明文字会在条件之前求值（MSVC 从右往左）

咬测 payload 时打印出 `-> -1, -1`，而实际两个变量在条件里刚被写成 `(0, 7)`。
`check (name, cond, detail)` 是一次函数调用，MSVC 的参数求值顺序是**从右到左**：`detail` 先算，
此时 `payLayer` 还是初值 -1。诊断信息因此在"最需要它的时候"撒谎。

规则：需要读取"条件里刚写过的变量"的说明文字，先把那个调用算到 bool 里：
```cpp
const bool parsed = groupDragTarget (payload, payLayer, payGroup);   // 先算
check ("...", parsed && payLayer == 2, juce::String (payLayer));     // 文案才准
```
纯查询函数（`startOf(...)`、`getGroupOccurrences`）不受影响，它们无副作用。

## P58-C：组的"出现"必须是一个数，不能靠"同组即同体"

用户两条投诉是同一个根：粘贴把副本塞进**源组**，于是两处内容共用一个括号，拖哪处都一起动。
只要模型里没有 `occurrence`，任何"复制一份"都只能落在"同组"这个筐里。

凡是 `groupId >= 0` 的比较，现在都要问第二个问题：`groupOccurrence` 是不是这次要的那一处。
P58 里踩到的具体点：`getGroupContentBounds`（框会把两处都框进去）、`captureGroupClips`
（拖动抓到两处）、`drawGroupLabels`（只画一次标签）、`isWholeGroupSelected`（选择判断）。
新增任何"遍历组员"的代码，先决定"全部出现"（`occurrence < 0`）还是"某一次"。

## P58-D：拖动与折叠共用一个按下事件时，折叠必须延后到 mouseUp

编组栏的行 body 既是"点一下折叠"，又是"拖出去放一处"。原实现在 `mouseDown` 里折叠，
于是**每次拖走一个组，都会顺手把那一行折叠起来**。改成：按下只 `dragMayStart = true`，
`mouseDrag` 超过 4px 就 `startDragging` 并清掉 `foldOnRelease`，`mouseUp` 里 `foldOnRelease`
仍为真才折叠。折叠/展开是一个动作，判断它的地方只能有一个。

## P58-E：`selectedTrimEdgeAt` 必须排在 `hitTestSample` 之前

`hitTestSample` 只能回答**一个**块，而它按"最后添加 = 画在最上层"倒序遍历，所以指针下有重叠时
它返回的是**盖在最上面的陌生人**。原来的 mouseDown 拿到它就直接改写选区，再按它算 trim band：
"选中三块拖其中一块的尾巴"于是变成"拖了别人"，用户看到的就是"拉伸有时失败"。

修法两层：①`hitTestSample` 本身改倒序（最上层优先，与绘制一致）；②按下时先问
"这一像素是不是某个**已选中**块的边缘"（`selectedTrimEdgeAt`），是就以它为准，并且**不改选区**。
自检断言必须走两步合一的口子（`getBlockGrabAtForTest`），只测 hitTestSample 会一路绿着穿过这个 bug。

## P58-F：`docs/demo.atomic` 的组 id 会随自检建组次数变化，别把它当回归信号

跑一次 `--snapshot`，`docs/demo.atomic` 里的 `<Group id="28">` 会变成 `27`，自动化轨的
`ownerId` 同步跟着变。看着像"组 id 漂移"，其实是 `LayerModel.cpp` 里的
`int& groupIdCounter()` ——**文件内静态、跨所有 `LayerStack` 共享**，只在 `addGroup` 自增、
`restoreGroups` 用 `jmax` 抬升，**从不清零**。演示会话的组 id 因此取决于"本次进程里此前
建过多少个组"，而自检的每个 fixture 都会建组。

判据（P58 实测）：**连跑两次，第二次不再变** → 只是收敛到"自检建组次数"对应的值，不是 bug；
如果每次跑都 -1，那才是真的在删组重建。P58 从 28→27 的原因是 `duplicateGroupToNextBar`
不再 `addGroupCopy`，自检净少建了一个组。

推论：不要把 `docs/demo.atomic` 的 id 当回归指标；要断言 id 就在自检里现场建工程断言。

## P59-A：边缘判定必须有"在块内"这一条，否则 band 是半条射线

`x >= right - band` 在数学上是一个**向左张开的半平面**，不是"右边缘附近的窄带"。少了
`x >= left && x <= right` 这半句，凡是**在块右边**的像素都会被它认领为"这块的尾巴"；
`x <= left + band` 对称地认领**块左边**的一切。

P59 用户报的"多选拉伸只有最右边那块有效，其它有时失灵"，就是这两条射线打架：
选中 A/B/C 三块相邻，按 B 的尾巴（B.right - 2px）——
B 的尾巴带命中（对），但倒序遍历先问 C，`x <= C.left + band` 对**左方一切像素**成立，
于是返回"C 的头"。拖动 C 的头 = 移动它的 source offset，而 offset 已经贴底 → 夹住 → **什么都不动**。
用户看到的是"按了没反应"。

规则：任何"离边缘多近"的判定，先问"在不在这个矩形里"。band 只描述距离，不描述范围。

## P59-B：两个块的公共边界是同一个像素，判定要按"最近的边"

包含判定之后仍有一处重叠：A.right 和 B.left 是同一个 x。按在边界像素上时两边都是 0 距离。
解法是取**距离最近**的那条边（并用倒序遍历 + 严格小于让最上层赢平局），而不是"谁先被遍历到"。

## P59-C：`isCollapsed` 的行里不要复用展开行的矩形

编组栏折叠行的 dB/声相读数用的是**和组名完全相同的矩形**（`x=46, w=width-106, y+4, h=18`），
于是每个折叠组都在组名背后印一行 `0.0 dB · C`。用户的原话是"在组名背后显示不相关的文字"。

规则：一个矩形只写一段文字。要在同一行放第二段信息，先把宽度切出来（`withTrimmedLeft/Right`
或明确分成左右两个矩形），否则两段文字一定重叠——而且折叠行往往是最短的那一行，
最没有空间容纳"另一个也画在这里"的东西。

## P59-D：镜像状态要"推导 + 推入"，不要存第二份 flag

编组栏的选中高亮没有自己的 `selectedGroupId`：`PianoRollComponent::getSelectedGroups()` 从
选区**现算**（每个组、每个出现，问 `isWholeGroupSelected`），主窗体在 `onSelectionChanged`
里推给面板。存一份 flag 就意味着"删除组 / 撤销 / 切图层"三条路径各自要记得清它，
漏一条就是"高亮挂在已经不存在的组上"。

推论：判定"整组选中"必须逐 occurrence 问（P58 的语义），否则选中一处出现时，
组里另一处没选，`isWholeGroupSelected(gid,-1)` 恒为 false，行就永远不亮。

## P60-A：一个按下事件只能有一个点击含义

编组栏的行 body 同时是"拖出去放一处"和"点一下折叠"。P60 用户要求"编组框要可以被选中"，
于是同一个 body 又被要求承担第三种含义。三个动作不能共用一个 `mouseUp`。

定案：**body 的点击 = 选中**，折叠下沉到左侧 chevron（`Part::collapse`）和右键菜单。
硬规则：一个命中区域上的一个按下事件只有一种"点一下"的含义；要加第二种，先给它一个自己的矩形
（chevron 就是这么来的），否则后加的那个一定吃掉先加的。

## P60-B：样式一致性是图，不是调用

用户说"编组框的选中样式要跟采样音源框和 VST 音源框一致"。照抄一份同样的四个常数**不算一致**——
它们是三份会各自漂的代码。做法：把机架卡片的 `drawCardRing` 提到 `Source/UI/CardLook.h` 的
`cardlook::drawSelectionRing()`，两边都调它，机架那份退化成一行转发。

断言也必须读图：`createComponentSnapshot()` + `getPixelAt()` 扫行左边缘那几像素，算与
`theme::accent` 的 RGB 曼哈顿距离。P60 实测：选中行 **21**、未选中行 **397**；把实现换回
组色描边时是 **284**——阈值定在 `<60` 就能精准分辨"accent 环"和"组色环"。
"它调了共享函数"这种断言证明不了任何事，只有像素能。

推论：凡是"两个东西要长得一样"的需求，都要有一个能被读回来的共同来源，
否则下次改其中一处你不会有任何提示。

## P60-C：只增不减的时间线不是"没有 bug"，是半个功能

P11 让滚动越过末尾就加长 `timelineBeats`（一次一块，最多四块），但从来没有反向路径：
用户滚进空白再滚回来，标尺仍然那么长、滚动条滑块仍然是一截短棍。用户的原话是
"当滚动条移回左边时，要把右边没有内容的钢琴窗区域去掉，而且要恢复滚动条的长度"。

修法是给增长配一个对称的回收（`trimTimelineToView()`），并且**挂在同一个入口**上
（`clampScroll()` 与 `setScrollBeats()`），不是散在 wheel / 拖 thumb / 缩放各处。
三个锚点（内容末、可见窗口、播放头）一个都不能少，见 REFERENCE「钢琴窗的时间线长度」。

凡是"按需增长"的东西，都要问一句"它什么时候缩回去"。没有答案，就是半个功能。

## P60-D：`createComponentSnapshot` 在 headless 自检里是可靠的

`panel.createComponentSnapshot(panel.getLocalBounds(), true)` 在 `--snapshot` 模式
（无 peer、无桌面）里能正常出图，`getPixelAt` 读到的就是绘制结果。给面板一个 `setSize()`
就够了，不需要加到任何父组件上。

注意：扫描窗口要留出抗锯齿的余量（P60 扫 `plateX - 3 .. plateX + 7`），
并用"最接近的像素"而不是"某个固定坐标"——线宽 1.8px 落在像素边界上时，正中间那格可能是半透明的。

## P61-A：拿"马上要被 clamp 掉的值"去判断需求

P60 给时间线配了回收，P11 的增长也一直在，但**缩放**这条路上增长会晚一档：把视口拉宽到
超过标尺时，那一档不涨，下一档才涨，中途右边是一条没有网格的死带。

根因在 `clampScroll()` 的那一行判断：

```cpp
if (scrollBeats + visibleBeats > timelineBeats) ensureTimelineExtendsTo (...);
```

`scrollBeats` 在这里**常常是负的**：缩放以某一拍为锚，锚点落在 beat 0 左边时
`scroll = anchor - (anchorX - gridX) / beatWidth` 直接算出 -16.6 这样的数，两行之后才被
`jlimit(0, maxBeats, …)` 夹回 0。负的那部分把需求算小了：`-16.6 + 74.5 = 57.9 < 64`，
于是"64 已经够长"——而真正要显示的是 `[0, 74.5]`。

修法是一行：**用夹过下界的值去问**。

```cpp
const double wanted = juce::jmax (0.0, scrollBeats) + visibleBeats;
```

规律：判断"够不够"时用的那个变量，必须和即将生效的那个值是同一个。顺序是
「先夹下界 → 再问需求 → 再夹上界 → 再回收」，任何一步用了还没夹的原始值，都会得到一个
"差一档"的行为——它不会报错，只会看起来慢半拍。

咬测：把 `jmax(0.0, …)` 删掉，自检立刻给出 `74.52视/64长`（正确是 `74.52视/128长`），
正是用户看到的那条死带。断言按**每一档**检查 `visible <= timelineBeats`，
不能只查最后一档——差一档的 bug 在终点是完全正常的。

## P62-A：复制"整组"的判据少了一句"哪一处"，粘贴就会把组弄丢

用户的原话是"复制粘贴编组的时候，有时候会取消已经编组的组"。那个"有时候"有一个数得清的原因：

```cpp
if (groupId >= 0 && isWholeGroupSelected (groupId, true))   // 旧
```

`isWholeGroupSelected` 的 `occurrence` 默认是 **-1 = 这个组的全部成员**。一个只有一处出现
的组，选满它当然满足；**一旦这个组在时间线上有两处**，选其中一处永远凑不齐"全部成员"，
于是复制落到"散音符"分支：音符留着 `groupId`（粘贴时悄悄粘回源出现）、采样块被
`clip.groupId = -1` 扒出组（这是 P56 修的 bug 从另一个门回来）。用户看到的就是"粘贴出来
的组散了"。

修法两行：
1. `const int occurrence = getSelectedGroupOccurrence();` → `isWholeGroupSelected (gid, true, occurrence)`；
   "-1 时退化成整组"这条约定正好让两种情况都对。
2. 散音符粘贴**显式** `note.groupId = -1`——注释里早就写着"散的就是散的"，代码却没有做。

规律：凡是"选中的东西算不算一个整体"这类问题，都要先问**哪一份**，再问"全不全"。
一个默认参数省下的两个字符，代价是一个只在"组被用过第二次之后"才出现的 bug。

## P62-B：同源同步里，"整处"和"局部"必须是同一个判据

同源编组（一处改、处处改）最难的不是复制属性，是**什么时候不该复制**：
拖一整处、删一整处，是"对这份副本的决定"，兄弟不能跟着动；拖它里面的一个音符、删一个音符，
是"形状变了"，兄弟必须跟着变。

两处实现必须是同一个判据，否则会出现"拖整处把兄弟也拖走"或"删一个音符把整处删掉"：

- 拖动：`wholeGroupOccurrencesInDrag()`（拿 `dragOriginals` + `dragClipOriginals` 的成员数
  和该出现的成员总数比）。
- 删除/解组：~~`isWholeGroupSelected (gid, true, occ)`~~ → **P63 改成了 `isWholeOccurrenceSelected (gid, occ)`**，
  原因见 P63-A：前者问的是整个选区，多组一起选时永远 false。

**写第一版时踩过的坑**：`collectTwinsForEdit()` 一开始是"收集兄弟成员，然后按**兄弟**的出现号
过滤整处选中"——兄弟当然不在被选中的那一处里，于是过滤永远不生效，删一处 = 删所有处。
自检当场抓到（"整处删除只删这一处"报 `0 处`）。正确做法是**按源出现号**决定要不要收兄弟，
所以现在是"先把选区按 (groupId, occurrence) 分组，再逐组问一次"。

## P62-C：出现的位置必须是存下来的数，不能每次推导

`occurrenceStart(gid, occ)` 第一版是纯派生的："这一处最早成员的 startBeat"。看着很干净，
实际会漂：删掉最早那个成员，"位置"立刻变成第二个成员的拍，于是**下一次同步把整处往右拖**；
每编辑一次漂一格，用户看到的是兄弟副本自己走动。

所以位置要么物化一次存起来（`NoteGroup::occurrenceStarts`，第一次被问到时写入，此后只在
整处移动时改），要么就不该存在。三处必须钉住：建组、粘贴/放一处、读工程。

**"派生优于存储"的例外**：当派生值会被自己的编辑影响时，它不是派生量，是状态。

## P62-D：`check ("中文…")` 会在日志里变乱码

`check()` 收的是 `juce::String`，裸 `"中文"` 走 Latin-1 解释，日志里就是 `åæº`。
规则（MEMORY.md 硬规则 1）早就写了，但 P62 加断言时又犯了一次——顺手用正则把全文件
19 处裸中文 `check ("…")` 都包成 `utf8 ("…")`，其中 11 处是以前留下的（P51/P52 那批）。
以后加断言，先写 `utf8 (`。

## P63-A：`isWholeGroupSelected` 问的是**整个选区**，多组一起选时永远 false

P62 给这个函数加了 `occurrence` 参数，看起来已经是"这一处全不全"的判据了。不是：

```cpp
for (int id : selection)
    if (note->groupId != groupId) return false;   // ← 选区里还有别的组，直接 false
```

`members` 和 `chosen` 是按**这一处**数的，但循环判的是**整个选区**。所以：

- 单组复制 / 单组删除 → 正常；
- 选中两个组再复制 → `copyNotesToClipboard` 的 `isWholeGroupSelected` 为假，落到散音符分支，
  粘贴出来是一堆**散音符**（实测：组数 2、音符 8、`groupId` 全是 -1）；
- 选中两个组再删 → `collectTwinsForEdit` 认为"没有一处是整处"，于是把兄弟全收进来连删。

用户报的是"粘贴下来变成一个组包含两个组的内容"，我实测到的第一步是"散开"。**两种症状是同一个根**：
多组选区下"整处"判据永远返回 false，谁用谁错。

新判据 `isWholeOccurrenceSelected (groupId, occurrence)` 只数这一处：成员数从**模型**里数，
选中数从选区里数，两边相等且大于 0。用它的地方：复制切片、`collectTwinsForEdit`。
`isWholeGroupSelected` 保留给"这个组我是不是全选了"的单对象问句。

## P63-B：剪贴板描述"一个组"时，它的成员也不能放在全局列表里

P56–P62 的 `GroupClip` 只存组的**属性**（名字、颜色、挂载、电平、曲线），**成员**放在
`noteClip` / `sampleClip` 里——那是整个选区共用的一份列表。

于是"两个组一起复制"必然只有一个结果：第二个组的成员要么被并进第一个组，要么掉出来当散音符。
`captureGroupClip` 多调用一次也没用，成员列表只有一个。

所以 `GroupClip` 现在自带 `notes` / `clips`，`noteClip` / `sampleClip` 只装**不属于任何整处**的散货。
配套改动两个地方，漏一个就出问题：
1. `copySelectionToClipboards()` 原来在 `copyNotesToClipboard()` 之后再调
   `copySamplesToClipboard()` ——后者会把**所有**选中块重新填进 `sampleClip`，包括整处已经认领的。
   现在一次填充三个剪贴板，第二趟取消。
2. `cutNotesToClipboard()` 原来只在 `groupClip.valid` 时删 `sampleClip` 里的块。现在整处的块不在
   `sampleClip` 里了，必须改成删**所有选中的块**，否则剪切会留下组的音频。

## P63-C：N 个对象一个锚点，否则"间距"这件信息就丢了

粘贴的锚点算一次（`clipboardFirstBeat()` = 剪贴板里所有东西的最早拍），然后**所有对象共用同一个
delta**。若每个对象各算"自己起点 → anchor"，两组本来隔 4 拍，贴出来会叠在同一拍上——看起来
正好像"合并成了一个组"。

咬测办法：把 `insertGroupMembers` 的 delta 换成 `anchor - object.startBeat`，自检的间距那条
立刻报 `20.00 / 20.00`。这条用例存在的意义就是防这个。

## P63-D：`isGroupClipboard()` 在菜单里的位置

`fillEditMenu` 的"粘贴目标"是 if / else if 链：`canPasteNotes() && canPasteSamples()` →
`canPasteNotes()` → `canPasteSamples()` → "剪贴板为空"。

P63 之后，一个"只有音符、没有块"的组会让 `noteClip` 和 `sampleClip` **都为空**（成员在
`groupClips[i].notes` 里），于是整条链走到底、菜单显示"剪贴板为空"，而粘贴其实是有效的。
把 `isGroupClipboard()` 提到链首（`canPasteAutomationCurve()` 之后）就好了。
规律：状态行是"哪一条 if 先命中"，新增一种状态就要检查它会不会被更早的分支吞掉。

## P64-A：「推走」和「归还」的判据重叠 → 60fps 横跳（表现出来的就是"闪屏"）

用户报：「当播放头播放到标尺右边某个位置时，会发生钢琴窗闪屏的 bug」。

`setPlayheadBeat()` 里有两条分支，谁都以为自己只做一件小事：

```cpp
// 归还：播放头回到"推走前抓拍的那个窗口"里
if (followScrolled && beats >= userScrollBeats && beats <= userScrollBeats + visible) { ... }
// 推走：播放头越过窗口 75%
if (beats > scrollBeats + visible * 0.75) { userScrollBeats = scrollBeats; ... }
```

问题在于**推走时抓拍的窗口本来就含当前播放头**：播放头在 51.8 拍越过 51.75 的触发线，
推走后 `userScrollBeats` 记的是 0，窗口是 `[0, 69]` —— 而播放头 51.8 **就在里面**。
于是下一帧归还（视图退回 0），再下一帧又推走（视图跳到 41.4）……

实测（探针打印每次滚动跳变）：

```
[51.8:+41.4] [51.8:-41.4] [51.8:+41.5] [51.9:-41.5] [51.9:+41.6] [51.9:-41.6]
```

220 拍里"整窗重绘 265 帧 / 滚动跳变 526 次"，也就是说 60fps 下每帧把画面横移 60% 窗口再来回。
用户说的"闪屏"就是这个，而"标尺右边某个位置"= 窗口 75% 那条触发线（不是标尺末端）。

修法只有一行：**归还必须要求播放头在往回走**（`beats < previous - 1e-9`）。
前进的播放永远不归还；loop 跳回、scrub、seek 都是回退，照常归还。修完 220 拍里只剩
5 次跟随、每次 +41.4，回退 0 次。

**教训**：两个条件描述的是同一个状态的两侧，写其中一条时必须把另一条的条件一起读一遍。
"回到原处"这类判据尤其危险——抓拍时刻的状态天然满足"已经回来了"。
另外：**别用"看起来在闪"去猜绘制问题**。闪屏第一嫌疑是布局/滚动每帧翻转，
用探针把每帧的 `scrollBeats` 打出来，一秒钟就能看见横跳。

## P65-A：多选之后，"点其中一个"不代表"只做这一个"

`armGroupMove()` 的第一行是 `selectGroup(groupId, occurrence)` —— 点组标签 = 选中这个组。
单组时完全正确；多选时它把用户刚框出来的选区**缩成一个组**，于是"选中两个组、拖其中一个"
变成"只拖这一个"，另外那个在指针动起来的瞬间掉队。用户报的就是这个。

判据不是"这个组被点中了"，而是"**这一处是不是已经整处在选区里**"：

```cpp
if (! isWholeOccurrenceSelected (groupId, occurrence))
    selectGroup (groupId, occurrence);      // 没整选 -> 才缩成这一个
```

同一处还有第二个半截：`captureGroupClips(groupId, occurrence)` 只抓**这一组**的块。
音符走 `captureDragOriginals()`（所有选中的音符），块只抓一组，于是多组拖动时音符全动、
采样块只动一组。现在 `captureGroupClips()` 就是 `captureSampleDragOriginals()`（所有选中块）。

**规律**：任何"点中 X 就把选区替换成 X"的地方，都要先问一句"X 是不是已经整个在选区里"。
`isWholeGroupSelected` 不能用在这里——它循环的是整个选区，多选时永远 false。

## P65-B：删掉一个控件，编译不会告诉你行尾空了一块

采样块的自动化框底部原来是"目标标签 + 启用开关"。开关删掉后，如果只删绘制和命中，
`getSampleTargetArea()` 里那行 `withTrimmedRight(... toggle.getX() ...)` 还在，行尾就永远留着
26 px 的死区——点上去什么都不发生，也没人报错。

所以这个删除配了一条**几何恒等式**断言：

```cpp
getSampleTargetAreaForTest (i, id) == getSampleMenuAreaForTest (i, id)   // 宽与右边界一致
```

咬测：把 target 改成 `withTrimmedRight (ui::pf (30))` → 立刻 518.8 vs 557.8 报错。

**规律**：删控件不只是删代码，还要删掉"为它预留的空间"，并且这个"没有空间"要被断言，
否则下次有人加回一个按钮时，谁也不知道那里本来是不是空的。

## P66-A：翻页和贴边是两种跟随，别拿"跳几次"当判据

P64 的用例断言 `jumps < 40`——它防的是"每帧来回横跳"。P66 把跟随从**翻页**改成**贴边**之后，
同一段 220 拍里滚动次数变成 **5970 次**（每帧一小步），这条断言会误报。

判据要跟着语义走：

- 翻页式（触发 3/4、落点 15%）：看**跳几次**、每次多大。
- 贴边式（触发边界、落点边界）：看**跟随开始之后，播放头离边界多远**。一帧 0.03 拍，
  所以 `worstGap < 0.1` 是硬的；翻页式落地在窗口中间，这个数会是 `19.55`（窗口 23 拍）。

咬测验证：把 `rightEdge` 改回 `scrollBeats + visible * 0.75`、落点改回 `beats - visible * 0.15`，
两条用例一起挂，detail 里 `离右边界最远 19.55 拍` 把症状说得比任何断言都清楚。

**顺带一条**：贴边式每帧都 `repaint()` 整窗，这是它的正常成本，不是性能回归。
真正的性能问题要看帧率，不要看到重绘次数变多就当成 bug 去"优化"。

## P66-B：标签被夹窄之后，画在它右端的把手会跑出去

`getGroupLabelBounds()` 加了 `jmin(width, 组宽)` 之后，一个 5 px 的标签仍然会调用
`getGroupAutomationHandleBounds()`，而那里写死了 `pill.getRight() - pf(12) - pf(1.5)` ——
返回的矩形比标签还宽、位置在标签左边外面，于是绘制代码在**组框外面**画了一个 12 px 的按钮，
`chevron` 更是画到了半个标签的左侧。

修法给两边都加同一道闸：宽度不足 `12 + 6` px 时返回空矩形；绘制处 `if (! handle.isEmpty())`。
命中测试不用改——空矩形 `contains()` 永远是 false。

**规律**：几何函数的输出一旦从"足够宽"变成"可能很窄"，所有**从这个矩形派生**的子矩形都要重新审一遍。
这次只有把手一个派生，下次可能是下划线、图标、徽标——它们都是 `pill.getRight() - 常数` 的形状。

## P67-A：「先夹后扩」会安静地把拖动锁死在旧标尺末端

`scrubRulerTo()` 里顺序错一步就没有症状，只有手感不对：

```cpp
const double beat = juce::jlimit (0.0, timelineBeats, snapBeat (raw));   // ← 先夹
...
setScrollBeats (beat - visible);      // 这一次才可能触到 ensureTimelineExtendsTo
```

指针拖到 80.5 拍时 `beat` 被夹成 64，随后 `setScrollBeats(41)` 正好把 64 铺满窗口，
**扩展条件 `41 + 23 > 64` 不成立**，标尺停在 64 不动。继续往右拖，指针一路远离，
播放头却钉死在 64 —— 看起来像"拖到头了"，其实是被自己的夹子绑住。

正确顺序：`raw > timelineBeats → ensureTimelineExtendsTo(raw)`，然后才吸附、才夹。

**咬不住的原因**（值得记）：首版断言是「播放头在右边界」+「标尺 ≥ 播放头」。
缺陷版播放头 = 64，新右边界也是 64，两条都成立，用例照样 PASS。
必须再加一条「播放头**越过**拖动前的标尺长度」（`rightHead > startTimeline`）才咬得住。

**规律**：夹取和扩展同时存在时，"扩展"必须是第一个动作。夹取把输入变成合法值的那一刻，
关于"用户想要多远"的信息就丢了，后面所有判断都在一个被修剪过的世界上做。

## P67-B：拖拽期间有两个写者抢 `scrollBeats`

标尺拖拽时 transport 并没有停（locate 是 seek，不是 stop），所以 `setPlayheadBeat()`
仍然会被播放推着每一帧进来。此时 `scrubRulerTo()` 刚按指针放好窗口，播放跟随分支
下一帧就按播放头再放一次：

- 指针拖到 100 拍，窗口 `[100, 123]`，播放头 100；
- 播到 123 起，跟随分支把窗口推到 `[123, 146]`；
- 指针一动，`scrubRulerTo()` 又把它拖回来。

结果就是按住拖的时候画面在指针和播放头之间来回。修法是两条跟随分支各加一道
`dragMode != DragMode::rulerScrub`：**按住期间窗口归拖拽所有**，松开后一切照旧。

咬测：把这两处换成 `true`，用例「拖拽播放头期间，播放跟随不抢视口」挂成
`0.00 -> 177.00`（= locate 到 200 拍、窗口宽 23）。

**规律**：两个来源写同一个视图状态时，问的不是"谁对"，是"这个手势期间归谁"。
`dragMode` 就是那个所有权凭证，别用时间戳或"上一次是谁改的"去猜。

## P68-A：用循环下标判线条等级，只在步长没变时成立

旧画笔：

```cpp
if (step % 16 == 0)      // bar
else if (step % 4 == 0)  // beat
```

`step` 是从 0 数起的 1/16 索引，所以 `% 16` 恰好是小节、`% 4` 恰好是拍。P68 让步长随缩放
爬到 1 拍之后，`step` 变成一个"拍"的索引：第 4 拍是 `4 % 16 = 4` → 被判成普通拍线，
小节线全丢，整个网格变成一片小平线，没有任何东西能拿来定位。

改法是问**这根线是什么**，不是问它是第几根：

```cpp
if (onUnit (beat, beatsPerBar)) bar
else if (onUnit (beat, 1.0))    beat
else                            subdivision
```

咬测：把 `beatsPerBar` 换成 `1.0e9`（永远匹配不上），断言从 `0.160 / 0.075 / 0.030`
掉成 `0.075 / 0.075 / 0.030`。

**规律**：一个"单位 = 另一个单位 × 固定倍数"的假设，写进取模是安全的；但只要有第二种单位
可能出现在同一个循环里，取模就变成了对单位的隐式承诺。承诺一旦破了没有编译错误，只有画面变平。

## P68-B：闭环自洽的用例测不出"两边一起错"

网格和吸附都用 `visibleGridStep()`，于是有一条用例是这么写的：**吸附结果必须落在
`collectGridLines()` 产出的某根线上**。这条断言很好看，但它是**闭环**的——把
`visibleGridStep()` 的阈值改成 0（步长永远 1/16）之后，它照样 PASS，因为两边一起回到了旧行为。

所以真正的判据是**绝对值**：

- 步长在小缩放下必须等于 `1.0`（不是"和吸附一致"）；
- 视野 0..8 拍里的线数必须是 `9`（不是"比上一次少"）；
- 吸附 `2.3` 必须得 `2.0`（不是"落在某根线上"）。

三条都写成绝对数，缺陷版才挂得住（`0.25 / 0.25 拍，33 / 33 根线`、`2.25 / 6.00`）。
闭环那条留着当**不变量**用（它抓的是另一类错：两份实现漂移），但不能当唯一判据。

## P69-A：上限和步长乘在一起才是像素，只改一个等于没改

"能放大到 1/64 拍"要同时满足两件事，分别藏在两个文件的两个常量里：

```cpp
maxBeatWidth = 768.0f;              // 能放大到 768 px/拍
double step = finestGridStep;       // 最细一档是 1/64 拍（0.015625）
```

只改第一个：`visibleGridStep()` 从 `gridDivision` 起爬，最细只有 0.25 拍，768 px 也是白设。
只改第二个：步长想细，但 `beatWidth` 卡在 180，0.015625 × 180 = 2.8 px，永远过不了 5 px 的
阈值，循环继续往上爬，1/64 依然不出现。

判断"能不能显示"的式子只有一个：`step × beatWidth ≥ minPixels`。看到其中一个数变了，
必须立刻问另一个数是多少。咬测里这两条分别挂 4 条和 5 条，症状都是"步长比期望粗一档"。

## P69-B：数值选点恰好重合，断言就白写了

吸附用例最初写的是 `snapBeatForTest (0.31) → 0.3125`。看着很具体，咬测时**没挂**：

- 1/64 网格：`0.31 / 0.015625 = 19.84 → 20` → `0.3125`
- 1/16 拍网格：`0.31 / 0.0625 = 4.96 → 5` → `0.3125`

两种网格给同一个答案。换成 `0.30` 才分开：`19.2 → 19` → `0.296875`（1/64）对 `0.3125`（1/16）。

**规律**：测"档位不同"的断言，先手算两档的答案，确认不相等再写进代码。这一步三十秒，
能省掉一整轮"编译—运行—发现没咬住—改断言—重编译"。和 P68-B 是同一条根（判据必须能
区分对错两种实现），但触发方式不一样：P68-B 是两边一起错，这里是数值刚好落在同一个吸引域里。

## P69-C：可见步长不能直接当"最短长度"

`visibleGridStep()` 在缩小时会爬到 1 拍甚至 4 小节。拿它当音符最短长度，12 px/拍时一条
音符都画不到一拍以下——用户只是缩小了看全局，模型里的能力却退化了。

```cpp
double minimumNoteLength() const noexcept
{
    return juce::jmin (gridDivision, visibleGridStep());   // 只往下让，不往上长
}
```

**规律**：凡是"由视图推导出来的限制"，都要问一句"缩小时它会变成什么样"。视图是**放大**
时的问题，缩小时通常是另一个问题（这里是"别把下限抬高"）。

## P70-A：夹取会掩盖移位，夹具必须让两种实现落到不同答案上

P70 一轮里踩了三次同一个坑，症状都一样：**断言在缺陷版上也 PASS**。

**第一次（P0-1，图层当前自动化轨）**。修复有两条路：写侧落盘 `autotarget`，读侧改成
"循环结束后一次性决定"。咬测只植入"写侧不写属性"，断言照样过——因为读侧的回退逻辑
（带 `expanded="1"` 的那条）给了同一个答案 `volume`。真正的根因在读侧，必须植入
"循环里每条 `setAutomationTarget`"才咬得住（`pan` / `pan`）。

**第二次（P2-5，`removeLayer` 的 `activeIndex`）**。三层、active=2（最后一条）、删 index 0：
- 正确实现：`2 - 1 = 1`
- 缺陷实现：`jlimit(0, 1, 2) = 1`

两边相等。改成四层、`setActiveLayerIndex(1)`、删 0：正确 `0`，缺陷 `1`，才分得开。

**第三次（P2-8，选中通道夹取）**。植入"夹到 127"之后断言仍然过，因为 `mixerChanged()`
又兜了一层把 20 夹成 7。夹具必须**绕开兜底**：直接 `setSelectedChannel(100)` 在 8 通道的
console 上，缺陷版给 100。

**规律**：写断言之前，把"正确"和"缺陷"两条路径**手算一遍**，两者数字相等就重写夹具。
这一步三十秒，能省掉一整轮"编译—运行—发现没咬住—改夹具—重编译"。
和 P68-B（闭环自洽）、P69-B（数值恰好重合）是同一条根的三种外壳。

## P70-B：加载路径上"只在不同时才写"＝把上一个会话当成基线

```cpp
for (int ch = 0; ch < numChannels; ++ch)
    if (parsedOutputs[ch] != masterChannel)          // ← 目标为主输出的通道一条都不调
        mixer->setChannelOutput (ch, parsedOutputs[ch]);
```

看起来是"省掉无意义的写入"，实际是"**新工程说了什么，只对那些不是默认值的通道生效**"。
新工程里 `ch0 → 主输出`（默认值）时一行都不执行，`ch0` 就保留上一个会话的 `1`。

第二层伤害更隐蔽：`setChannelOutput()` 会判环，而判环读的是**当前**拓扑。逐条应用时，
文件里正确的 `1 → 0` 会被旧会话残留的 `0 → 1` 判成环而**拒掉**——坏数据被"保护"逻辑
自己制造出来。

修法固定成两步：**先整体重置到默认，再逐条应用**（`restoreRouting()`，末尾一次 `notify()`）。

**规律**：凡是"加载/初始化/替换"语义的入口，第一个动作必须是"清成默认"，不是"逐条设置"。
任何 `if (新值 != 默认) 写入` 的写法都在偷偷假设"当前状态已经是默认"。

## P70-C：shared_ptr 的最后一个引用，析构位置不可控

`getSchedule()` 返回 `std::shared_ptr` 的**副本**。发布侧换掉成员之后，音频线程手里那份
可能就是最后一个引用 —— 构造函数返回时 `PublishedSchedule` 在**实时线程**析构，
几十个 note 加上每个 insert 的 `shared_ptr<PluginDescription>` 一起 `delete`。
每块还要自旋抢一次锁。这两件事都不该出现在回调里，但代码里没有任何一行看起来是错的。

修法是换所有权模型，不是加锁：

```cpp
std::atomic<const PublishedSchedule*> publishedSchedule;      // 读者：一次 acquire load
std::shared_ptr<const PublishedSchedule> schedule;            // 发布者持有
std::vector<std::shared_ptr<const PublishedSchedule>> scheduleRetired;   // 只保两个
```

发布 = 旧的 `move` 进 retired → 新的进 `schedule` → `store(release)`；retired 超过两个就
`erase(begin())`，**释放永远发生在消息线程**。读者只拿到裸指针，有效期一个块。

**规律**：实时线程上出现 `shared_ptr` 拷贝，就等于"析构时机交给调度器"。要么读者不持有
所有权（裸指针 + 发布者保命），要么一开始就别共享。

## P70-D：边界判据写成两种形式，两侧就会各漏一半

note-off 的旧代码：

```cpp
if (offOffset <= (double) chunkStart || ...) continue;      // 跳过
const int off = (int) std::llround (offOffset) - chunkStart;
if (off >= 0 && off < chunkLen) addEvent (noteOff, off);    // 发送
```

一个用**浮点原值**判跳过，一个用**取整后**判发送。音尾正好落在块尾时：
- 本块：`off == chunkLen` → 不发；
- 下一块：`offOffset == chunkStart'` → `<=` 成立 → 跳过。

**两块都不发**。宿主 VST 的持续音因此一直响到 stop。

修法是把判据挪到同一层：先算 `on` / `off` 两个整数，跳过条件改成 `off < 0 || on >= chunkLen`。
这样同一个边界在两块里分别是 `chunkLen` 和 `0`，一进一出，恰好接上。

**规律**：一个事件的"开始/结束"如果会被相邻两个块各判一次，两个块必须用**同一个表达式**
判断同一件事。混用浮点与整数、混用 `<=` 与 `<`，边界上就会出现一条谁都不负责的缝。

## P71-A：AsyncUpdater 合并的是"一个消息循环"，不是"一个动作"

`modelChanged()` 里的 `triggerAsyncUpdate()` 看起来已经是"把多次通知并成一步"，注释也写着
"dragging a note fires a notesChanged every mouse move, and each of those is NOT an undo step"。
但它只挡住**同一个**消息循环里的连发（`layersChanged` + `notesChanged`）。拖拽的每一次
`mouseDrag` 都是一个新的消息循环，于是 60 次移动 = 60 个快照，撤销要按 60 次。

时间上的"帧"和用户意义上的"动作"是两个量，任何"靠时间去猜边界"的方案都会在慢拖（每帧一次）
和快拖（一帧多次）之间给不出同一个答案。边界只能**由手势本身**给出：

```cpp
GestureWatcher : juce::MouseListener        // 挂在 Desktop 上，见下
{ mouseDown → beginUndoGesture();  mouseUp → endUndoGesture(); }

void commitHistory() { if (undoGestureActive) return; ... }   // 手势里一概不落
```

**为什么挂 Desktop 而不是面板**：混音台（`MixerWindow`）、插件编辑器（`PluginEditorWindow`）、
`AutomationWindow` 都是独立顶层窗口，不在这棵树里。`addMouseListener(l, true)` 只覆盖本组件子树，
所以每条路径都要各自登记一次——四十个组件里漏一个，那个面板就还是老行为。

**顺序正好合用**（`juce_Component.cpp` 的 `internalMouseUp`）：`mouseUp(me)` 先跑，
`MouseListenerList::sendMouseEvent` 后跑。也就是说"松手时最后一次写入"发生在
`endUndoGesture()` 之前，捕获到的就是最终状态。反过来（listener 先跑）会漏掉收尾那一笔。

**还有一条**：`beginUndoGesture()` 必须先 `commitHistory()` 再置 `undoGestureActive = true`，
否则它自己也会被守卫拦住，"上一个手势"永远结算不了。

## P71-B：停止标志消费得太早，会被"还没起来的预览"吃掉

`stopAudition()` 设一个 `auditionStopRequest` 标志，`renderAudition()` 消费它。第一版把消费
写在函数开头（serial 比较之前）：

```cpp
if (auditionStopRequest.exchange (false) && auditionVoice.active)   // ← active 还是 false
    auditionVoice.played = kAuditionLength;
const int serial = auditionSerial.load();   // 这一段才把 active 置 true
```

按下鼠标到第一次渲染之间，`active` 还是 `false`：标志被 `exchange` 拿走了、什么也没做，紧接着
serial 分支把 `played` 归零、`active = true`，正弦以满音量响满 1.1 秒。"停止"变成了"从头开始"。

修法是把消费挪到 serial 分支**之后**。判断"顺序是否重要"的方法：问标志服务的是**已经起来的
声音**还是**即将起来的声音**——后者就必须排在"让它起来"之后。

## P71-C：断言盯错了量，缺陷版照样 PASS

"插件键交还"这条第一版断言 `!hasLiveNotes()`，缺陷版（死槽不回收）却照样过。原因在实现里：

```cpp
for (auto& live : livePluginSlots) {
    if (slot == nullptr || slot->pluginId <= 0) continue;   // 缺陷版
    ++engaged;                                              // 计数在这之后
}
livePluginEngaged.store (engaged);                          // 于是记成 0
```

`hasLiveNotes()` = `voices || engaged || queued`，engaged 被记成 0 就落回 false——**槽没还，但
"有没有声音"这个量看不见它**。真正被污染的是另一件事：八格表被占了。

两条都要补：
1. 加 `getLivePluginSlotCount()`，断言盯住"表里还有几条"；
2. 让事件在槽**还是插件时**就 `beginLiveBlock()` drain，真的建出条目（第一版是在改回内置
   合成器之后才 drain，事件走了振荡器分支，压根没建槽——断言测的是空场景）。

**规律**：断言要盯住"被缺陷改变的那个量"，不是"最顺手的那个量"。顺手的量往往是被另一条
路径兜住的（同 P70-A、P68-B）。
\n
## P72-A：配对靠"序号"，而序号来自一个会被这次编辑改掉的字段

Phase 62 的形状同步是这么配对的：把一个出现里的成员按 `clipShapeLess` 排序，
按**序号**去认另一个出现里的兄弟。前提是两个出现的成员顺序一致——这在"只改宽度、
只改长度"时成立，因为排序键是 `startBeat` 然后 `pitch`，而这两种编辑不动它们。

Phase 72 让下边缘拖动改 `pitch`，前提当场破掉：

```
组 A（occ 0）：A0(pitch 60) B0(pitch 48)     排序 B0, A0
组 A（occ 1）：A1(pitch 60) B1(pitch 48)     排序 B1, A1

拖 A0 的下边缘到 40
occ 0 排序变成 A0(40), B0(48)
occ 1 排序还是 B1(48), A1(60)

序号 0 配对 → A0 ↔ B1     ← 名字、路径、audio 指针全被写过去
序号 1 配对 → B0 ↔ A1
```

用户看到的不是"块变高了"，是"两个副本互换成了对方的音频"。而且它**只在拖过同组
另一块的高度时**发生，正好是最容易被当成"偶然"的那类 bug。

规则：**配对键只能写形状编辑自己不会动的字段**。起点 + 名字足够区分成员，完全重复
的成员本来就互换无害。`noteShapeLess` 也有同样的隐患（拖动音符改 pitch 之后，
`syncGroupShape` 还会被 `mouseUp` 叫一次）——没改，因为音符那条路要改的断言更多，
先记在这里。

## P72-B：两条新带子会和"拖动整块"抢地盘

块高度是一行行给的，一行就 `keyHeight`（约 18 px）。上下各留 `kResizeHandleWidth`
（约 8 px）的带子，中间只剩 2 px——用户想拖块本身的时候，几乎必然抓到某条边，
块就再也挪不动了。

所以高度带是 `jmin (band, height * 0.25f)`：一行高的块上下各 4.5 px，中间还剩 9 px。
**长度带**不需要这个处理，因为块通常有几十上百像素宽，而高度是"行数 × 18"。

顺带一条：四选一的边判定必须是**同一次命中**（`edgeOfClip` 返回最近的那条），
不能让"头/尾"和"上/下"各自判一次——角落像素会得到两个答案，按下时按一个走、
光标按另一个画，用户就会看到"光标是上下箭头，拖出来的是长度"。

## P72-C：给缺陷补的守卫自己成了缺陷，断言反而咬不住

写 `applySampleSpanDrag` 时我担心"组同步已经把副本设成绝对高度，再按 delta 加就是两倍"，
于是加了一条跳过：同组、不同出现 → `continue`。然后写了第 9 条断言（选择里含同组两个
副本，变高只落一次）——它 PASS。

把守卫改成 `if (false && ...)` 再跑，**还是 PASS**。原因是：

- delta 是从 `primary`（手势起点快照）算出来的**增量**；
- 组同步写进副本的是 `primary` 的**绝对值**；
- 而 `sibling.laneSpan + delta` 里 `sibling` 也是手势起点快照。

三条加起来，不管有没有那道 `continue`，副本拿到的都是同一个数。守卫是死代码，
断言测的是"两条路恰好等价"，不是"守卫有用"。

真正能咬住的是另一个缺陷：把 `sibling.laneSpan + delta` 写成 `target->laneSpan + delta`
（加到**当前值**上）——这时组同步刚写进去的 4 行再被加 2 行，`70+6 / 70+6`。这才是
断言该守的东西：**delta 加在起点上，不加在当前值上**。

**规律**：断言写完之后，要问自己"这条断言守的是哪一行代码"。如果答案是"我自己刚写的
那段防御"，先把那段防御删掉再跑——跑得通，说明它不是防御，是装饰。同 P70-A、P71-C。

## P72-D：上限夹错了变量，块会长到网格外面

加高一行原本只夹 `laneSpan <= 128`。而块是**站在**某一行上向上长的，pitch 120 的块
最多只能有 8 行；夹 128 会让它长到 `topPitch = 247`，画出来的部分全在网格外，
用户看到的是"加高没反应"（其实是长到看不见的地方去了）。

上限要按"还剩多少行"算：`min (128, numPitches - pitch)`。这条数字第一次写的时候
只在拖动路径上生效是看不出来的——拖动的目标行本来就被 `pitchAtScreenY` 夹过，所以
`jmax(bottom, lane) - bottom + 1` 自己就不会超；只有"菜单一步一行"那条路才会撞上去。

## P73-A：把事件拆开才能测——但拆开的第一个代价是两种坐标点

`mouseDown` 里同时住着两个点：`e.getPosition()`（`Point<int>`，事件自己的整数像素）和
`e.position`（`Point<float>`，给几何用的）。后者是前者的 float 视图，平时混着写没有代价。

拆成 `handlePress (Point<float> pos, ModifierKeys, int clicks)` 之后，所有 `Rectangle<int>::contains`
就编不过了——JUCE 的 `Rectangle<T>::contains` 只接受 `Point<T>`，`Point<int>` 和 `Point<float>`
之间**没有转换构造函数**（JUCE 8 只有 `(x, y)` 和 copy，跨类型要走 `toFloat()` / `roundToInt()`）。

所以在函数顶部做一次 `const auto ipos = pos.roundToInt();`，工具条/滚动条/标尺/键盘/网格这几个
整数矩形用它，其余继续用 `pos`。

**收益**：自检第一次能走真正的按下路径。之前所有"点击"断言都只能调按下时调用的 helper
（`armGroupMove`、`simulateNoteResize`），那是在测 helper，不是测按下。P73 的七条断言全部
经 `simulatePressForTest` / `simulateDoubleClickForTest`。

**代价**：三个 `handleXxxAutomationMouseDown` 的签名也得跟着从 `const MouseEvent&` 改成
`(Point<float>, ModifierKeys)`，否则 `handlePress` 里没有 `e` 可传。这类"顺手改签名"要一次做干净，
不然会出现"函数体里 `e` 未声明"的一串错误。

## P73-B：选择性断言会骗人——"选中了整组"不等于"抓住了整组"

P73 的核心承诺是：没进入编辑时，按组里的音符只能整体拖。第一版断言检查的是选择集
（`isSelected(a) && isSelected(b)`）。

这条断言守不住它该守的东西，因为一次按下里有两处独立写入：

```cpp
selectGroup (groupId, occurrence);   // 写选择集
captureDragOriginals();              // 写起拖集合
```

一个"选择对了、但起拖只抓了那一个音符"的实现，能完全通过只看选择集的断言——而这正是
用户会遇到的 bug（高亮整组，拖动却只走一个）。

所以补了两个钩子，让断言盯**起拖**：

```cpp
int  getDragOriginalCountForTest() const { return (int) dragOriginals.size(); }
bool pressArmedNoteResizeForTest() const  { return dragMode == resizeNote
                                                  || dragMode == resizeNoteLeft; }
```

第 7 条断言于是变成"关门时按边缘 = 2 个 / 不缩放；开门后 = 1 个 / 缩放"。删掉锁守卫，
这条和选择集那条一起挂。

同族错误见 P71-C（`hasLiveNotes()` 看不见死槽）、P72-C（守卫是死代码）。

## P73-C：失效判据写成"东西还在吗"，撤销会让它答错

编辑状态（`editingGroupId`）什么时候失效？第一版写的是"每次按下时检查这个组还在不在"：

```cpp
if (editingGroupId >= 0 && layerIndexOfGroup (editingGroupId) < 0)
    editingGroupId = -1;
```

它看上去很稳，实际上答的是错误的问题。组 id 是**序列化**进工程文件的，撤销把整个工程恢复到
上一份快照时，同一个 id 会原样回来。于是：

1. 用户在组 G 里编辑，删掉一个成员；
2. Ctrl+Z，G 和它的成员回来了（id 还是 G）；
3. 存在性检查答"在"，`editingGroupId` 仍然是 G —— **用户从没双击过的组，自己打开了**。

正确的位置是 `layersChanged()`，而且是无条件清：

```cpp
void PianoRollComponent::layersChanged()
{
    editingGroupId = -1;   // 加载 / 撤销 / 重做 / 切层都会到这里
    ...
}
```

代价是"改个图层名也退出组内编辑"，这比"编辑器以为自己还在里面"要便宜得多。

**规律**：判断"某个外部 id 是否还指向同一个东西"时，先问 id 是不是会被重新创建。卸载/重建
型的模型里，`id` 是名字不是地址，存在性永远答 yes。

## P73-D：视觉标记不能用"更粗"，因为它已经被占用了

想表达"这个组已打开"，第一版画了四个 L 形角标 + 加粗边框。两个问题：

1. **加粗已经意味着 hover/focus**（`focused ? 1.4 : 1.0`），再加一档只是"更亮"，用户分不出
   是"我在编辑"还是"鼠标在这儿"。
2. **角标会被组名 pill 吃掉一个**。pill 长在组框左上角，正好压住左上角标；组越短 pill 占比越大，
   越需要提示的组越看不见提示。

改成：实线框保留，外面再画一圈**虚线**（`drawDashedLine` 四条边，dash 4 / 3.5，外扩 3 px）。
虚线绕一圈，四边都看得见，pill 只能盖住顶上的一小段。

**规律**：一个新状态要找一种**没用过**的视觉维度（形状、线型、位置），不要复用已有的
（粗细、亮度），否则两个状态会互相冒充。

## P74-A：选择的安全网要装在手势上，不要装在 `setMixedSelection` 里

需求只有一句："采样和音符在编组的情况下被选中即是选中其所在的编组"。
最省事的写法是让 `setMixedSelection()` 自己把成员扩成整组——所有入口一次覆盖。

它会静默毁掉 P62：

`collectTwinsForEdit()` 靠 `isWholeOccurrenceSelected()` 决定这一处要不要复制给兄弟。
判据是"选中的成员数 == 这一处的成员数"。一旦底层写入永远给出整组，用户就**再也选不出
"一部分"**，于是"改一个成员 → 其余兄弟跟上"永远不触发，形状同步整个失效——而且不报错，
只是从某天起副本不再跟着变。

正确的位置是手势层：点击（`armGroupMove`）、框选（`expandToWholeOccurrences`）、
加选（`addOccurrenceToSelection`）。底层保持精确集合，"我选了什么"仍是可回答的。

**规律**：底层写入是**记录**，手势是**表达意图**。把意图的补全写进记录，记录就不再可信。

## P74-B：一个成员被选中，不能只靠"点击"这一条路

第一版只改了点击，框选漏了。漏掉的后果不是"框选时没扩散"这么温和——是
"框到七个音符里的一个 → Delete → 组里少了一个成员"，然后下一次编辑又把少掉的那个
当成"这一处被删了"，从其余副本里一起删掉（P62 的形状同步规则）。一处误操作扩散到全组。

框选这一路还有第二个坑：`mouseUp` 里 `simulateMarquee(marquee)` 读不到修饰键了
（松手时 Shift 早就抬起来）。所以 `marqueeAdditive` 必须在 `mouseDown` 的空白分支就记下，
和 `dragMode = marquee` 同一个地方。

**规律**：一个新规则要问的不是"点击怎么走"，而是"**有多少条路能到达这件事**"。
把入口列出来（点击 / 框选 / 加选 / 面板 / 粘贴 / 撤销恢复），逐条看，别等自检告诉你漏了哪条。

## P74-C：Shift 的语义是"加"，不是"换"

`armGroupMove` 原来无条件 `selectGroup()`：Shift 点第二个组，第一个组从选择里消失。
用户看到的是"Shift 没反应"——因为结果和普通点击一模一样，只有拖起来之后才发现少了一半。

加选必须把**整处**加进去（不是那个成员），否则 P74-A 的队形问题在同一次操作里又回来了。

**规律**：凡是"选中"的函数，都要问一句"多选时它做什么"。`selectGroup` 是替换式命名，
调用点却分布在带有各种修饰键的分支里——名字不说的东西，就是会漏的东西。

## P74-D：`+ juce::String (gid) + "）"` 在中文长串后面报 C2059

一行里连续拼接中文提示 + `juce::String(int)` + 全角括号，MSVC 报了
`SelfTest.cpp(11792,99): error C2059: 语法错误:"）"` ——列号指着行尾的 `)`。
同一模式在别处（P73 的断言）能编过，只有这一行的中文串更长。

拆成中间变量（`const juce::String detail73 = ...;`）就过。

**规律**：自检的失败信息一旦长到要拼四五段，就拆变量。这不只是可读性问题——
编译器在这种行上给的错位信息会把人带到完全无关的地方浪费十几分钟。

## P75-A：组框有两条身份，进入组之后它只剩"空白"这一条

P73 把组框（`getGroupFrameAt`）做成了"这整块东西"的把手，P73 又让组可以被打开。
两条放在一起就打架：组打开之后，框里除了成员就是空白，可框还是抢在 marquee 前面
把那一击接住，于是"点空白取消选择"变成了"重新选中整组并起拖"——用户想放掉手上的
选择，结果全选了一遍。

判据只能一条：`frameGroup != editingGroupId`。编辑中的那个组**落下去**，走 marquee。
不要在前面再补一个"清空选择然后 return"的分支：那样单击能取消选择，但拖拽框选没了，
而且又是一份新的清空逻辑（`mouseDown` 的空分支本来就在做这件事）。

**规律**：一个把手在两种状态下有两种意思时，让它在"另一种状态"里**不被认成把手**，
比让它"被认成另一种东西"要稳。

## P75-B：命中测试答的是"画在最上面的"，不是"你想按的"

`hoveredSampleId = hitTestSample(pos)` 是倒序遍历（后画的在上面），回答的是像素问题。
用户的"我在按这个块"是**选择**问题。两者在一个情况下分家：两块叠在同一拍同一行。

复制粘贴到自己身上就会叠，于是：

1. 选中 A 和 C（A 被 B 压着，B 没选中）；
2. 按 A 的中间——命中测试答 B；
3. `edgeId < 0 && ! isSampleSelected(B)` → `setSampleSelection({B})`，A、C 掉了；
4. 拖起来只有 B 动。

用户说的是"选中多个之后，有时候只有一个跟着动"。这跟高度算法一点关系都没有，
所以怎么调 `applySampleSpanDrag` 都不会好。

修法是在**选择层**补一问：四条边都没命中时，再看点在哪个**已选中**块的矩形里，
有就返回 `(它, none)`。`none` 让调用方读成"抓住了块、没抓住边"——保留选区、起拖整选。

**规律**：凡是"点到了什么"和"你选了哪些"可能给不同答案的地方，先问选择，再问画面。
P58（已选块的边缘优先于命中拾取）是同一条规则在长度上的版本。

## P75-C：写一块同步一块，等于让两块轮流当模板

`syncGroupShape(groupId, occurrence)` 的语义是"以这一处为准，其余多退少补"。多选拖高度
时对每个块都调一次，就成了：

```
写 A → 以 occ0 为准抄给 occ1   （occ1 的当前值被改掉）
写 B → B 用的是"按下时快照 + delta"，写回 occ1
```

两处出现的形状**本来一致**时两条路恰好等价，怎么看都是对的——所以这个 bug 只在
"两处形状本来不一致"时才现形（老工程、撤销回来的快照、成员数不同的出现）。
那时的结果取决于循环顺序：`60+5 / 60+7`，指针停在 5，其中一处停在 7。

正确形状是：循环里只写各自的绝对值，**全部写完**再对涉及的每组同步一次。
那时候值已经一致，谁当源都一样——"谁当源都一样"才是不依赖遍历顺序的定义。

**规律**：一个函数如果"以某一处为准"，那它每调用一次就选一次源。循环里调 N 次，
就是选了 N 次源。要么只调一次，要么先让所有候选变成同一个值。
（P72-C 说"delta 加在起点上"，说的是同一个道理的另一个面。）

## P76-A：双击进组顺手把选区改小了

用户的原话是「组内调整多个采样的高度有时候要动两次其他的才联动一次」。方向第一直觉
全指向加权算术：`applySampleSpanDrag` 的 delta、`jlimit` 上限、`syncGroupShape` 的选源。
于是先写 9 种手工配置（同处多选上/下、两处各选一个、成员数不同、初始高度不同、三个副本、
跨处不同成员、A0B0B1、四个全选）逐帧 dump，再写 216 组合模糊检查——**全部同步**。
算术是干净的。

问题不在"算得对不对"，在"拖的是谁"。真凶：

```cpp
// 改前
enterGroupEdit (clip->groupId);
setSampleSelection ({ clip->id });      // ← 无条件
```

双击的第一击在**闭组**状态下走 `armGroupMove` → `selectGroup`，把整次出现选好了（P73/P74）；
第二击 `handleDoubleClick` 进组，顺手把这个选区收窄成一个成员。于是后续拖动的一次写入里
只有一块在选区中，"三块一起长"当然不成立。用户重新框选一遍后才正常，于是被记成"有时候
要动两次"。

改后：**打开不是选择**。

```cpp
enterGroupEdit (clip->groupId);
if (! isSampleSelected (clip->id))
    setSampleSelection ({ clip->id });
```

音符侧同一处、同一改法。和 P65 给组名 pill 定的规则是同一条：落在已选中的东西上的手势，
不能替换"已选中的东西"。

**规律**：用户说"有时候"的时候，先分清是**算术有时错**还是**输入有时不同**。前者靠模糊
检查证伪，后者只能靠问"这一下到底拖住了谁"。选择区和"会动的东西"是两份状态，写入点
越分散越难对齐——凡是"打开/进入/聚焦"这类手势，默认不碰选区。

**连带的两条夹具教训**：

1. 用屏幕坐标的模糊测试必须先过 `getGridBounds().contains(rect.getCentre())`。第一版
   在 `basePitch 40` 上全部报 `not armed`，看着像缺陷，实际是块落在网格外、`handlePress`
   的 `getGridBounds()` 直接 return。夹具的错误信息写成 `not armed (...)` 而不是"断言失败"，
   就是为了让这种区分在一眼之内做完。
2. 断言要问"谁会被缩放"，不是"有几个会动"。`getDragOriginalCountForTest()` 数的是
   `dragOriginals`（拖起来会移动的东西），多选时它有 2、3 个是**正确**的；P25 早把长度
   缩放锁死在 `dragResizeNoteId` 上。为此加了 `getResizeNoteIdForTest()`，用例从此问的是
   那条规则本身。

## P77-A：四条都是自检的问题，不是实现的问题

**一、测不到的守卫就是会烂的守卫。** `hitTestGhost()` 里 `i == activeIndex` 这一跳，
第一版咬测（删掉它）**零条挂**——因为 `handleDoubleClick` 里活动层自己的命中测试排在前面，
守卫永远走不到。两条路：删掉，或者给一个能直接问它的钩子。选了后者
（`getGhostLayerAtForTest()`），因为它守的是这个助手自己的契约，将来换个调用点就靠它兜底。
**教训**：咬测打不中的那个 if，要么删，要么问给它一个入口——不能留着当"保险"。

**二、用例撞像素就变成空断言。** 第 5 条（眼睛关掉不跳层）第一版拿 `plain` 的位置去双击，
可第 4 条刚在同一个像素上给活动层加了 `shadow`，活动层先回答，于是"跳不跳"根本没被问到。
含糊的用例会以 PASS 的形式撒谎：**每一条断言都要挑一个只属于它要测的那个东西的位置**。

**三、场景会串味。** 第 3 条（双击组里的 ghost）第一版直接接在第 2 条后面，可第 2 条已经把
活动层换到 0 了，`grouped` 就不再是 ghost，走的是普通路径——失败信息看起来像缺陷
（`0 / 组 484 / 选中 1 个`），其实是夹具忘了退回去。**每条用例开头显式设好前置状态**
（这里 `setActiveLayerIndex(1)`），不要依赖上一条留下的局面。

**四、"这一刻像素上有 ghost"要按层问，不要按整屏问。** 4b 那条最初写成
`getGhostLayerAtForTest(shadow) == -1`，实际答 0 是对的——那个像素上**另一层确实有音符**，
钩子只是拒绝回答"你正在编辑的这一层"。要问"会不会把自己当 ghost"，必须找一个只有活动层
有东西的位置。

## P78-A：顺序不是风格问题，是答案问题

`handleDoubleClick()` 是一个 if 链，**先写的先答**。采样开框那一问放哪儿，等价于
"这一像素上谁在说话"：放在 `hitTestActiveLayer` 之前，压在块上的音符就再也轮不到
——用户想开音符的包络，拿到的是块的曲线框。

第一版我把它放在音符之前，理由写的是"块是 `drawSamples` 先画的"。这句话字面成立但
结论反了：**先画 = 在下层**。正确顺序永远是"后画的先回答"，而 `drawSamples` 在
`drawNotes` 之前（`paint` 里就是这么排的），所以块排在音符之后。

**规律**：判断命中顺序时不要背"谁重要"，去看 `paint()` 里那几行的先后。渲染顺序是
唯一不会撒谎的事实来源，P77 的 ghost 分支也是这么定下来的（ghost 画在所有东西下面，
所以它排在最后）。

**夹具又栽在同一处**：ghost 采样块第一版放在 pitch 48、第二版放到 84，两次都落在
可见网格之外——`rect 96 59.8 192 36.4` 的中心 y=78 正好压在 `grid` 的上边界（`grid 96 78 …`）上，
`getGridArea().contains()` 卡在边界。这是 P76 记过的老坑（块落在网格外 → `not armed`）
第三次出现。**用屏幕坐标的用例，先打印 rect 与 gridBounds**，一次就能看见，比猜
pitch 快得多。改成 78 之后立刻通过。

**咬测的价值在这一条上很直接**：把分支提到前面（BITE 2）挂的不是"有没有打开框"，
而是"打开的是谁的框"——`-2 / 框 开着（应为 1 / 关着）`。如果断言只写
`isSampleAutomationOpen()`，这个缺陷会以 PASS 的形式活下来。

## P78-B：把「窗」做成了「条」——需求里的名词就是判据

用户原话是「双击采样也要能打开采样包络**窗**」。我做的是把双击接到已有的内联曲线框
（P49 那个贴着块的窄条）上，理由是"采样只有这一个编辑器"。功能上确实能改曲线，但
用户随即发来一张截图——音符包络窗，标题、身份行、输出、大画布、预设、底部提示——说
"我是想让采样块被双击后打开**这样的**编辑框"。

教训不是"要问清楚"那么笼统，而是：**当需求里的名词在代码里找不到对应物时，那通常是
要新建东西的信号**。当时 `NoteEnvelopePanel` 就在旁边，`SampleEnvelopePanel` 不存在；
"没有"这件事本身就是答案，我却拿一个近亲去顶替。近亲能省掉一天，也能让整个交付作废。

写下来的规则：**同一个动词要落在同一类对象上**。双击音符得到窗口，双击采样块就该得到
窗口；让同一手势在两种东西上给出两种量级的反馈（一条窄条 vs 一个窗口），用户会认为
其中一个是 bug——事实上也确实报成了 bug。

## P78-C：`EnvelopeEditor` 复用的三个坑

采样窗直接复用了音符窗的曲线编辑器，省掉大部分绘制代码，但有三个地方必须显式处理：

1. **空态文案写死在编辑器里**（"没有可编辑的音符"）。给 `setEmptyText` 而不是复制一份
   编辑器——复制会让两边的鼠标逻辑迟早分叉。
2. **绑定必须是模型的引用，不能是副本**。`setEnvelopeSource` 每次解析
   `&sample->activeCurve()`；任何"先拷一份再编辑"的写法都能画出正确的图，而音频一个
   采样点都不会变。这类缺陷**没有视觉症状**，只能靠 `getEditor().getEnvelope() ==
   &model->volumeCurve` 这种指针断言咬住（咬测：换成 static 副本 → 2 条挂）。
3. **两条曲线共用一套形状语言，但中性点不同**（音量 1.0 / 声相 0.5）。预设按钮「直线」
   若统一写 1.0，在声相上就是硬右——名字和结果对不上。同理右上角必须有 caption 报名
   （"声相 0.50"），否则用户分不清自己调的是哪条。

## P78-D：构建时 exe 被用户开着的程序占用

`LNK1104: 无法打开文件 AtomicDAW.exe`。`tasklist` 显示一个 05:44 启动的实例，
命令行没有 `--snapshot`——是**用户手动打开的 GUI**，不能杀（未保存的工作）。

Windows 允许**重命名**正在运行的可执行文件，所以：`mv AtomicDAW.exe
AtomicDAW.running-0544.exe`，再构建就写出了新的 exe，用户的进程继续用它已经映射的
旧文件，互不干扰。之后提醒用户关掉旧实例再重开，别去删那个改名文件（他还占着）。

## P79-A：`addChildComponent` 加进来的组件是**隐藏**的

`NameEditor` 由三个面板 `addChildComponent()` 加入（省掉空闲面板的一次绘制）。`begin()` 里
`editor.setVisible (true)` —— 只显示了里面的 `TextEditor`，父组件仍然是 hidden，于是
**一个像素都不画**：数据改了、`isEditing()` 是真、自检全绿、屏幕上什么都没有。

这一条特别毒的地方在于**它的症状是"全对"**：模型对了，断言过了，截图还是一行普通文字。
发现它靠的是 `--snapshot` 截图 + 放大看名字那一行，而不是任何断言。

修法：`begin()` 里 `setVisible (true)`，`close()` 里 `setVisible (false)`（可见性归组件自己管，
宿主只 `addChildComponent`）。**并且**把 `isVisible()` 加进那三条"双击打开改名框"的断言里
（咬测：删掉 `setVisible(true)` → 3 条挂，`可见 0`）。

## P79-B：`--snapshot` 传绝对路径不会写文件

`Main.cpp` 用 `getCurrentWorkingDirectory().getChildFile (args[i+1])`。传
`F:/PLSMStudio/Atomic DAW/docs/p79.png` 时拼出来的路径是错的，`createOutputStream()`
返回 nullptr，程序**静默成功**、退出码 0，旧图原样躺着。

症状是我盯着旧图看了四轮，还以为"框画不出来"。判据：`ls -la` 看 mtime，或者先 `rm` 再跑。
正确用法是 `cd` 到项目根目录传相对路径：`./build/.../AtomicDAW.exe --snapshot docs/p79.png`。

## P79-C：改名框记行号 = 改错人

编辑器开着的时候列表会动（删一层、删一张卡、组被剪掉）。用 `getLayer(index)` 提交，删掉第 0 层
之后这个 index 指向的是**原来第 1 层**，名字就落到别人头上。三个面板都存 id：
`indexOfLayerId` / `indexOfSlot` / (层 id + 组 id)，提交和自愈（行没了就 `close(false)`）
都先换回索引。

## P80-A：`--snapshot` 写图是**追加**，不是替换

`createOutputStream()` 在一个已经存在的 PNG 上会**从末尾接着写**。于是 `docs/p80.png`
里堆了 **18 个 PNG 头**，而 `PIL.Image.open` / 一切看图工具只读**第一张**——第一张是最早
那次还没有速度行的图。代码、模型、打印、`row 0 = 工程速度` 全对，屏幕上是旧画面。

这一条和 P79-B 是同一个入口的两种死法：一个没写进去（路径错），一个写了但排在旧图后面。

判据（可信的、不看像素的）：`open(png,'rb').read().count(b'\x89PNG') == 1`。
修法：`Main.cpp` 里写图**之前** `pngFile.deleteFile()`——一次运行一张图。

这个坑把我困了整整一轮：`PRINT` 探针、红色标记块、`PAINT tracks=7` 全都"证明代码对"，
因为代码确实对，是文件不对。**先数文件里的 PNG 头，再看像素。**

## P80-B：paint 时打印的探针，不是"写图那次 paint"的读数

为了查 P80-A，我在 `drawTrackRow` 里加了打印。它顺带编出一个错误结论：探针说
`tracks=7`、`row 0 = 工程速度`，而截图里表头写「6 轨」。两者其实说的是**不同的两次 paint**
（窗口在快照前重绘过好几轮），探针报的是最后一次。

教训：像素级判据要写在**写图之前**那一刻的状态里，或者干脆换成"数文件里的字节"。
拿"paint 期间打印的变量"去核对"文件里的像素"，中间隔着一次你不知道的绘制。

## P80-C：74 设计单位的列表列，`速度 60–240 BPM` 被裁成 `速度 60–24`

自动化窗左侧名字列宽是固定的 74 设计单位，`速度` 这个短标签本来就是为它挑的；把量程
一起塞进去就溢出——而且**溢出不是省略号，是硬裁**，读起来像另一个数字（`60–24`）。

量程属于画布，不属于列表：列表只写「速度」，`60 / 120 / 240 BPM` 画在该行的画布右上角。

## P80-D：量程标注贴 row 的右缘，等于贴到视野外

`row` 是一条**横跨整条时间线**的条（64 拍 ≈ 3072 px），不是"窗口那么宽"。`.withRight(row.getRight())`
把文字送到 3072 px 处，而窗口只有 1160 px 宽——标注画了，一个像素都看不见。

要对齐的是**看得见的那条边**：`.withRight((float) getWidth() - (float) ui::px (8))`。
凡是"贴在行尾/条目右侧"的装饰，先问一句这一行是不是比窗口宽。

## P80-E：咬测的还原**不能按字符串换回**

第一版脚本植入缺陷后，用 `text.replace(new, old)` 撤销。植入文本一旦与原文件里**已有的某行**
重合，`replace` 就命中第一处——而那多半不是被改的那一处：

- 「反函数用线性解」把两行换成一行 `return a.beat + dt * a.bpm / 60.0;`，而平坦分支本来就有
  一模一样的一行（只是缩进不同），还原把**平坦分支**换回了指数解 → 两条分支互换，
  编译得过、行为全错。
- 「积分退化成直线」同样：还原后 `secondsAtBeat` 里平坦分支拿到 log、常速分支拿到线性。
- 还有一条把 `const double bpmHere` 的声明和使用一起换掉，只换回后半段 → `C2065: 未声明的标识符`，
  构建失败、`restored` 照样打印。

症状：`FINAL: 688 PASS / 3 FAIL`，而代码"看起来"已经还原了。

修法（已写进脚本）：**咬之前读原始字节，咬之后写回原始字节，并逐字节比对**，
`raise SystemExit` 不一致；锚点带足上下文保证 `count == 1`；`new` **不要复刻文件里已有的行**。
七次还原全部 `restored ok`，收尾 691/0 才算数。

## P80-F：demo 曲线的拐点在 125 px/拍下全在视野外

`openDemoTempoAutomation()` 原本把断点放在 0 / 16 / 32 / 64 拍（"bar 5"、"halfway"）。
自动化窗跟着钢琴窗缩放，快照时视野只有约 **9 拍**——图上只看得见"升上去"，
看不到峰和回落，一条"先升后降"的曲线在截图里变成一条上坡。

演示曲线的作用是**证明形状**，所以断点必须落在视野里：改成 0 / 2 / 4 / 64。
这不是功能缺陷，是"用来当证据的那张图"没说服力——同样是"代码对、图上没有"的第三种死法。

## P81-A：`AudioEngine::start()` 从来没向系统要过输入通道

`deviceManager.initialise (0, 2, nullptr, true)` 从 P1 起就长这样——两个参数是**输入/输出通道数**，
一直是 `0`。所以"这个软件能录音吗"这个问题在引擎里根本没有入口：哪怕插着声卡、系统里麦克风
权限全开，`inputChannelData` 也永远是空指针。

修法不是加界面，是先把 `(2, 2)` 要过来，失败再回落 `(0, 2)`：

```cpp
lastError = deviceManager.initialise (2, 2, nullptr, true);
if (lastError.isNotEmpty())
    lastError = deviceManager.initialise (0, 2, nullptr, true);   // 没麦的机器也要能开
```

**回落这一步不能省**：不写它，任何人只要没有可用输入设备，整个引擎（包括播放）就起不来。
凡是"这功能要有硬件"的判断，先看引擎有没有向系统要过这件硬件——界面上的灰按钮骗人，
`initialise` 的第一个参数不骗人。

## P81-B：在消息线程里问走带位置 = 把队列延迟算成走音

`MidiInputHub::onNote` 跑在 **MIDI 线程**。而采集器 `TakeCollector` 持有 `std::vector`，
只能在消息线程动，所以事件必须过桥。第一版自然的写法是这样：

```cpp
// 错的：拍号在 lambda 里取
juce::MessageManager::callAsync ([this, pitch, velocity, on]
{ takeCollector.noteOn (pitch, velocity, audio.getPlayback().getPositionBeats()); });
```

`callAsync` 是**排队**，不是立刻执行。排队期间走带在推进，于是位置读数比事件晚一个队列延迟
（实测几十毫秒量级，48 kHz 下每拍 0.5 秒时约 5 ms 就是 1% 的音高错位感；连续弹奏时它表现为
"整体往后飘一点"，而不是某个音错——所以很难听出来是 bug）。正确写法是**在哪发生就在哪读**：

```cpp
const double beatNow = audio.getPlayback().getPositionBeats();   // MIDI 线程里读
juce::MessageManager::callAsync ([this, pitch, velocity, on, beatNow]
{ ... recordNoteAt (pitch, velocity, on, beatNow); });
```

`getPositionBeats()` 是 `std::atomic<double>`（relaxed）——MIDI 线程读它是安全的，这一点是这条
写法成立的前提。`recording` 同理必须是 `std::atomic<bool>`，因为 MIDI 线程要读它。

## P81-C：`RecordBuffer::finish()` 先 disarm 再读 `writePos`，交回半块

`getNumSamples()` 的语义是"`armed` 时看 `writePos`，停了看 `frozenSamples`"。所以 `finish()`
里两步的顺序就是**契约**：

```cpp
frozenSamples = writePos.load (std::memory_order_acquire);   // 先快照
armed.store (false, std::memory_order_release);              // 再停
```

写反（先停再读）在"读 `writePos` 那一刻还没人再写"的情况下也常常侥幸对，但音频回调随时可能在
两步之间进来把 `writePos` 再推一格——于是交回的样本数比真实的多一帧，WAV 尾部多一个样本，
块宽算出 2.0001 拍。这类差一帧不会崩，只是让断言偶尔红一次。

同理 `append()` 满了要**自己停**（`armed.store(false)` 后 return），不能回绕：回绕会把开头盖掉，
用户按满 5 分钟之后拿到的是一段"从 4 分钟开始"的音频。

## P81-D：`AudioBuffer (2, 24000)` 在 48 kHz 下是 0.5 秒，不是 1 秒

自检里造测试音频时写了 `juce::AudioBuffer<float> voice (2, 24000)`，注释写"1 秒"，
然后用它断言采样块宽 2 拍。48 kHz 下 24000 帧 = **0.5 秒** = 1 拍（120 BPM），于是
`8.00 + 1.0000 拍` 报了 FAIL——看起来像 `commitTake` 的块宽算错了。

诊断路径是加一个 `getPlaybackForTest()` 探针，直接印 `秒@8 / 拍@5 / len`：`len=0.500` 一出来
就清楚了，是测试的算术错，不是产品的。**断言里凡是出现"秒↔拍""帧↔秒"的手算常数，先把它打印
出来**；探针用完要删（这一轮删了两个访问器）。

## P81-E：demo 的音高落在视野外，还被组自动化框压着

`openDemoRecording()` 第一版把录进来的音符放在音高 84–96（"听着亮一点"）。快照上什么都看不到：
钢琴窗快照视野约 4 个八度且以中音区为中心，96 在顶上之外；就算在视野里，它也被随后打开的
**组自动化框**盖住（框画在组的上方）。

演示数据的职责是**证明形状**，所以它必须落在"这张图看得见的那一小块"里：改成
`{ 67, 71, 74, 79 }`（G3–G5），落在 demo 自己那段 8–24 拍音符的左边（4–7 拍），
并且捏一段**先升后降**的曲线——平的一条在图上等于没有。

## P81-F：`juce::File` 相对路径的基准是**当前工作目录**，不是 exe 也不是工程

demo 第一版把录音文件夹设成 `docs`（相对路径），快照跑起来 cwd 是
`build/AtomicDAW_artefacts/Release`，于是 WAV 落到 `build/.../Release/docs/录音 1.wav`——
一个仓库里的垃圾文件，而且 `docs/` 这个名字长得像"工程根目录的 docs"，很容易被 `git add .` 带进去。

落盘位置必须显式：demo 用 `File::getSpecialLocation (tempDirectory).getChildFile ("AtomicDAW-demo-takes")`，
自检用 `temp/atomic-p81-take` 并在结尾 `deleteRecursively()`。**任何"写文件"的测试都要先问
一句"cwd 是谁"**。

## P81-G：走带停着时 `stopTransport()` 是空操作（P32 规则），自检别指望它收尾

"停止键要结束录音"这条断言，第一版写成：`beginTakeForTest(4.0)` → 录几个音符 → 调
`host.stopTransport()` → 断言组已建好。红了。

原因不是新代码：P32 定下"停止时若已停着就什么都不做"，而 `beginTakeForTest` 是纯数据入口
（它不开走带），所以那一刻走带是停的，`stopTransport()` 直接 return，`finishRecording()` 一次都没跑。

夹具要先把状态摆对：`host.playTransport (false)` 再开始录。**测"某个键会收尾"就得先让那个键
真的有事情可做**——否则测的是提前返回的分支。

## P81-H：只写一个点的曲线，`loadPoints` 会把它撑成横贯全工程的直线

`AutomationLane::loadPoints` 的既有约定是"至少两点"，单点会被补成一条贯穿全工程的直线
（读侧必须有个"前后都取这个值"的意思）。录制侧如果照直写——推子只动了一次、或只收到一个 CC——
用户就会看到自己的组音量突然被一条直线钉死到工程末尾，而且**撤销之前它一直是生效的**。

所以 `applyRecordedControls` 里写 lane 前先数：

```cpp
if (points.size() < 2) continue;    // 一次推子不算一条曲线
```

同一条道理往上追一层：`decimateRecordedCurve` 的第一趟漏斗会把"值没变"的点全丢，
只按值差（1%）和音量差**同时**判断就会把慢速扫弦削成一个点——两趟漏斗 + 首尾钉住才稳。

## P81-I：断言的**消息参数**是无条件求值的，它比断言本身更容易崩

`check (cond, msg)` 里 `cond` 会短路，`msg` **不会**。于是：

```cpp
check (utf8 ("两百个 CC 点稀疏到 30 个以内…"),
       ! points.empty() && (int) points.size() <= maxRecordedCurvePoints && …,
       juce::String (points.size()) + utf8 (" 个点 / 末值 ")
         + juce::String (points.back().value, 3));      // ← 空 vector 上照样执行
```

植入"CC 7 不映射"之后 `decimateRecordedCurve` 返回空 vector——这**正是**这条断言本该红的时候。
`std::vector::back()` 在空 vector 上是 `*(end() - 1)`，而没有分配过的 vector `end()` 就是 `nullptr`，
于是读 `0xFFFF…F8` → **段错误**。

后果比"少一条断言"严重得多：

- 自检进程崩在"报红"这一步，一条 FAIL 都没打印出来；
- Windows 错误报告在无头会话里把进程挂住，`ps` 上只剩一个活着的 exe，看上去是**死循环**；
- 第一版咬测脚本没有 `timeout`，于是卡在第一次植入上 **26 分钟**没有输出。

修法两条一起用：

1. 消息里不要直接下标取值，走一个带边界检查的取值器：

   ```cpp
   const auto pointAt = [] (const std::vector<AutomationPoint>& p, int index)
   { return index >= 0 && index < (int) p.size() ? p[(size_t) index] : AutomationPoint {}; };
   ```

   同一个段落里的 `noteAt` / `controlAt` 是同一件事（`take.notes[0]` 在"一个音符都没录到"
   的时候同样越界）。
2. **咬测脚本必须给每次运行一个超时**，并把 `timeout / 非 0-1 退出码` 单独记成一档
   （`TIMEOUT` / `CRASH rc=`）：崩溃不是"红了"，它连红了几条都答不出来。

一句话：**断言要先能报，再谈判对错。** 一条会在失败路径上崩溃的断言，等于在失败时删掉自己。

## P81-J：`ramp` 遮蔽 `ramp`——P80 留下的 C4456，这一轮才被看见

P80 自检里原本有一条 `TempoMap ramp;`（算好的图），后面又写了 `TempoModel ramp;`（会画曲线的
模型）。内层同名只是 `C4456` 警告，行为完全正确——所以它从 P80 一直活到 P81，直到这一轮重编
`SelfTest.cpp` 才在输出里露头（STATUS 里"零 warning"这句话因此是假的）。

改成 `rampCurve`，并在注释里写清两者不是一回事。
**教训不在命名，在"零 warning"这句话怎么核对**：增量构建只重编改过的 Translation Unit，
没被重编的文件里的警告不会出现在 `tail` 里。要断言"零 warning"，得看**全量**构建输出，
或者至少在你改过那份文件的那一次看全。

## P82-A：`createOutputStream` 是追加，不是覆盖——快照看的是**上一次运行**的图

P82 花了整整一轮在"两个下拉明明画了却看不见"上，最后发现**代码一直是对的，图是旧的**。

`docs/p81-*.png` 有两套写法：主图（`probe82.png` / `p82.png`）在写出前调 `pngFile.deleteFile()`，
旁边的二十多张（`writePng("-groupauto", …)` 那个 lambda）**没有**。而
`File::createOutputStream()` 是**追加**，不是截断——于是每一轮快照都把新图接在旧图后面，
文件一路涨到 2.5 MB。PNG 读取器只认第一个 `IEND`，也就是**第一次运行时**那张图。

症状长得完全像渲染 bug：探针打印 `curves=4`、`name='手绘'`、`target=-1`（代码是对的），
而图里那个格子只写 `组音量`（第一轮的旧像素）。于是开始逐像素考古、在 `drawMenuCell` 里涂红、
把整个 `paint()` 涂成红——**连满屏红色都不出现在图里**，这才意识到"画的不是这一张"。

修法：lambda 里同样先 `target.deleteFile()` 再 `createOutputStream()`。文件从 2.5 MB 掉到 158 KB，
一次就对了。

**教训**：任何一个"改了代码但画面没变"的现场，**先比对 PNG 与 exe 的 mtime、再看文件大小有没有
逐轮变胖**，再谈渲染。再补一条：不要用"往画面里涂一个不可能出现的颜色"去验证绘制路径——
那次满屏红色没出现，被误读成"paint 没被调用"，其实 paint 被调用了，只是进了一张没人读的图。

## P82-B：拿字符串切 XML，切中的是"第一个看起来像的地方"

复现 v21 老文件时，第一版夹具用 `xml.indexOf("target=\"pan\"")` 找 `<Curve target="pan">` 的
起点、再用 `lastIndexOf("</Curve>")` 找终点，把中间那段挖掉。断言一直红：`target 0 rev 0`
——pan 曲线还在。

原因很朴素：`indexOf` 找到的那个偏移**不保证属于那个元素**（注释、别处同名属性、属性顺序都会挪位），
`lastIndexOf` 更是不带"从哪开始"的重载。切出来的区间和以为的不一样，挖掉的是别的一段。

修法：**DOM 手术**——`parseXML` → 广度/深度找到 `<Sample>` 元素 → 逆序
`removeChildElement` 掉所有 `<Curve>` / `<CurveState>` → 补一个真正的 v21 形状 `<Envelope>` →
`toString()`。夹具由此变成"这个文件真的是 v21 的样子"，而不是"这个字符串碰巧缺了一段"。

**教训**：断言夹具的形状要用**能表达结构的东西**去改。字符串切分的失败方式是静默的（它不报错，
它只是切在别处），而静默的夹具会让人怀疑被测代码。同一轮里另有一条断言数整个文件里
`name="手绘"` 的 `<Curve>`，得到 2——因为**编组**里也有一条同名曲线，扫描没限定在 `<Sample>` 内。
同一个毛病：范围没说清楚。

## P82-C：「空」不能写成一个词

`CurveSlot::target = -1` 表示"画着，但不驱动任何东西"。往磁盘上写的时候，很自然想写成
`target="none"`——**这是错的**，而且错得安静：`routeFromText` 不认识 `none`，回落到默认，
于是"空"读回来变成 `level`，用户的曲线在打开工程之后开始**悄悄推推子**。

正确写法是**根本不写 `target` 属性**（`route.isNotEmpty()` 才写），缺属性 = 空。读侧同理：
没有 `target` 就保持 -1，不要"猜一个默认"。

咬测证实了这条的价值：把写侧改成"空的就写 level"，只有那一条 v26 往返断言挂
（`hasTarget 1`），其余全绿——说明这条规则**只有这一处守卫**，非常容易被下一个人"顺手补全"。

**教训**：凡是"没有值"的状态，磁盘上要表达成**缺席**，不要表达成**一个词**。
一旦它是个词，它就得进枚举、进解析、进兼容表，而任何漏掉的地方都会静默变成一个真实的默认值。

## P82-D：`NoteEnvelope` 的默认构造是**启用的 ADSR**

用 `curve` 成员做"空替身"时，`NoteEnvelope()` 给出的是 `makeADSR()` 的结果——**启用**、
带起音衰减的斜坡。拿它当"没有这条曲线"的返回值，等于让"看一眼"变成"听一下"：一个没画过
曲线的块会突然有包络。

替身只能用 `NoteEnvelope::makeIdle()`（扁平、`enabled = false`）。
`AutomationLane` 相反：默认构造就是 `enabled = false`，所以那里直接 `AutomationLane{}` 即可。
两个类型的默认语义**不一样**，而它们现在是同一个 `CurveBank<CurveType>` 的两个实例化——
模板不区分，写的人必须区分。

## P82-E：给"当前是哪条曲线 / 哪个目标"建一个缓存字段，就是造第二份真相

`PianoRollComponent` 里原本有一个 `int expandedGroupTarget`：打开框时写一次，绘制时读它。
P82 把目标这件事挪到 `CurveSlot::target` 之后，这个字段立刻变成**旧值**——绘制读它、
菜单写模型，两处不同步，画面上写着"音量"而模型里是"空"。

处理办法不是"记得两处一起改"，而是**删掉字段**：`getExpandedGroupTarget()` 每次去问
`group->curveTarget()`。同理 `groupLaneFor(group, target)` 整个删掉，换成
`activeAutomation()` / `findRoutedTo(target)`。

这是 P73 那条（`editingGroupId` 失效判据写在 `layersChanged` 上）的同一类问题：
**同一条事实存在两份，就一定会分叉**。区别只在于分叉被谁看见。

## P82-F：demo 的守卫条件写错，功能"静默不生效"

`openDemoGroupAutomation()` 里给截图加第二条曲线，守卫写成 `if (bank.getNumCurves() < 3)`。
而 `NoteGroup` 的构造函数**已经建了三条**（level / pan / plugin），条件恒假，那段代码永远不执行。
没有任何报错，没有崩溃，只是截图里左格写着"音量"而不是"手绘"——而当时恰好又在看旧图（P82-A），
两件事叠在一起，白花了很多时间。

顺带：`CurveBank::add()` 的上限是 `maxCurves = 8`，所以"再加一条"的门槛是**常量**，不是硬编码。
写 `< bank.getNumCurves() + 1` 这种条件时要问一句"这一步在这一刻是第几条"。

**教训**：给截图/演示摆状态的那段代码，判据要写成"我要的状态是否已经成立"
（`find(name) == nullptr`），而不是"数量对不对"。数量是别人的不变量，会随构造函数变化。
\n
## P83-A：补偿放在源头，等于让它等着还没干的活

第一版想法很自然：既然 A 比 B 慢，那把 B 往后推不就完了——在 B 的**入口**加延迟。
错在"入口"这个词：入口在 B 自己的插入链**之前**。B 的信号被按住 500 个采样，然后才进
`processInsertChain`；如果 B 自己也有 300 的插入延迟，它出口落在 500+300，而这个位移在
它跟父通道汇合时又要再算一遍——参与两次、算两次。

正确的落点是**汇合点**，而且只有汇合点：`entry[父] - 自己的结束`。一个信号从源头到汇合之间
不该有任何东西碰过它的时间轴；延迟线是"让它等"，等的是**别人已经发生的事**，不是"还没发生的自己"。

顺带一条几何上的事实，第一版没意识到：需要**两条**线。入口那条存的是"直达的声音"（要等一个
慢孩子），出口那条存的是"处理完的声音"（要等一个更慢的兄弟）。它们在同一个 block 的**不同时刻**
被写——一条线同时当两个用，第二次写就把第一次的历史覆盖了，症状是"只有某些块错位"。

## P83-B：测试不到的分支等于没写

P83 的最大风险不是算错，是**默认情况下这段代码从来不跑**：没有 VST3 插入就没有延迟，所有表是 0，
`if (maxPdcLatency > 0)` 一次都不进。无头自检又加载不了第三方 VST3（不跑插件扫描）。

于是这一阶段的实现一度是"数学有断言、渲染没断言"。三处咬测把它戳穿了：把 `solve` 的总延迟强改成
`max(1, …)`，**723 条里有 7 条音频测量断言一起挂**（块音量、声相、混响尾巴……）。这说明两件事：

1. 补偿路径确实在真实渲染链上（不是死代码），位移一个采样就会被测出来；
2. 但那 7 条是**别的功能的断言**在替这段代码报警——它们不该承担这个职责，而且只在有人咬的时候才报。

修法是把不确定性收进一个明确的口子：`PlaybackEngine::setInsertLatencyForTest(channel, samples)`，
发布期折进 `own[]`，音频线程完全不知道它存在。有了它才写得出真正的端到端断言：
**给一条静默通道强加 32 采样延迟，整个混音必须逐样本后移 32**，两条路径（bypass 总线 / 过通道）各一条。

咬测证实两条断言各自守着一条分支：关掉 bypass 的延迟 → 只挂第一条；把 `joinDelay` 写死 0 →
只挂第二条。互不掩护。

**教训**：一段"平常不跑"的代码，必须有一条**只属于它**的断言，而且要能说出"这条断言是因为它才绿"。
用别人的测量去间接覆盖，等于把两条不变量绑在一根绳上：断了才知道，而且不知道断的是哪根。

## P84-A：咬测咬在缓存上，比不咬更糟——它会给你一个"已覆盖"的假象

给 P84 设计第四处咬测时，我想验证「恢复出来的会话必须仍算未保存」，于是把
`recoverFromFile` 里的 `dirtyCache = (history.front() != savedStateText);` 改成
`dirtyCache = false;`。跑出来 **742/0，一条都没挂**。

第一反应是"断言写错了"。不是。`hasUnsavedChanges()` 的实现是：

```cpp
auto now = captureState();
if (now == history[historyPos]) { dirtyCache = (now != savedStateText); return dirtyCache; }
```

`dirtyCache` 只是一个**预算好的答案**，被问的时候会当场重算。改它等于改一个马上会被覆盖的变量——
所以那条断言正确地在说"真相没变"。

真正的坏法是把**真相**改掉：`savedStateText = history.front();`，也就是"恢复完顺手当成已保存"。
这才是会丢工作的 bug（用户恢复回来，关窗口，一声不响没了）。这么咬，一条精确命中，detail 是 `clean`。

**教训**：咬测咬不中，先问"我改的是真相还是它的缓存"。项目里这类成对结构不少
（`dirtyCache` / `savedStateText`、`editingGroupId` / 组的形状、`expandedGroupTarget` / `curveTarget`），
凡是看到一对，咬那个**没有它也能算出来的**那个，才有意义。

## P84-B：一个会删用户文件的默认值，在无头运行里就是定时炸弹

崩溃恢复的第一版想的是"默认开启，窗口启动时不用特别做什么"。看起来无害——直到想起
`renderHeadlessReport()` 的**第一件事**是：

```cpp
component.saveProjectToFile (pngFile.getSiblingFile ("demo.atomic"), saveError);
```

而"保存成功"在新设计里意味着"删掉恢复副本"。于是每跑一次 `--snapshot`，就会把用户当时真正
未保存的工作删掉一次。而这条路径**每天跑几十次**，且删的时候非常安静。

修法不是加判断，是把默认值反过来：`bool recoveryEnabled = false;`，只有 `Main.cpp` 的
`MainWindow` 构造里 `setRecoveryEnabled (true)`。理由写进注释：这是整个编辑器里**唯一会写、
会删用户目录**的代码，而每一个自动化运行都会构造同一个类。

顺带一条同类的验证：改完之后专门看了一眼 `Documents/AtomicDAW/Recovery` 是否存在——
无头跑完**没有创建**，说明早退真的生效了，而不是"创建了空目录但没写东西"。

**教训**：给"有破坏性的副作用"的功能定默认值时，想的不是"正常用户会怎样"，而是
"**这个类还会在哪些别的场景被构造**"。自检、快照、导出、命令行——它们都构造同一个 MainComponent。

## P84-C：恢复副本不是工程，别让它沾上"当前工程"这个身份

`saveProjectToFile(file)` 的最后一行是 `currentProject = file;`。而自动保存要写的那份东西
和保存工程**看起来一模一样**（都是整篇会话的 XML），最省事的实现就是复用它：

```cpp
saveProjectToFile (recoveryFile, error);   // ← 灾难
```

后果不是"多存了一个文件"，是 `currentProject` 变成了恢复目录里的那个副本。这之后用户按 Ctrl+S，
文件选择器不再出现（因为"已经有一个路径了"），**歌被存进 Rescue 文件夹**，而磁盘上那个真正的
工程文件从此停在旧版本。用户不会立刻发现，因为标题栏还写着工程名（那是 `getFileNameWithoutExtension`，
而副本名字的前缀恰好也是它）。

所以 `recovery::write` 是一条**完全独立**的路径：它只写文件，不碰 `currentProject`，不更新
`savedStateText`，不进撤销历史。恢复方向同理——`recoverFromFile` 把身份**从副本里读回来**
（`recoveryOf`），因为文档里根本没有"上次打开的是哪个工程"这种记忆。

**教训**：两个东西内容相同，不代表它们是同一个东西。写之前先问一句"这一行会不会改掉
别人对'我是谁'的认知"。

## P84-D：`dirtyCache` 是"上一次问的时候的答案"，不是"现在的答案"

P84-A 说的是"别咬缓存"，这一条是同一件事的另一面：**别在设计里依赖缓存的时点**。

`autosaveTick` 的第一版写的是：

```cpp
if (! dirtyCache) { dropRecovery(); return; }   // ← 省一次序列化
recovery::write (... captureState () ...);
```

看着很合理：`dirtyCache` 已经是"与磁盘不同"的预算结果，不必为了发现"没变"而序列化整个工程。
问题是它由 `commitHistory()` 写出来，而 `commitHistory` 挂在 `AsyncUpdater` 上，**下一个消息轮**才跑。
于是最典型的场景正好撞上：

1. 用户停手五分钟（这期间每次 tick 都 `recoveryDirty == false`，早退，什么都没发生）；
2. 用户改了一个音符 → `modelChanged` 把 `recoveryDirty` 置 true；
3. 3 秒后 tick 到来 → 读到的 `dirtyCache` 还是**五分钟前**那个 `false`（新一轮的 commitHistory 还没跑）；
4. 于是 `dropRecovery()`，并且把 `recoveryDirty` 清成 false —— 那一笔编辑没有被保存，
   而且直到下一次编辑才会重试。

修法不是"提前刷 dirtyCache"，是**别用它**：一次 `captureState()` 同时回答两个问题
（是不是仍未保存、跟已写的是否不同）。代价是编辑后每 5 秒一次序列化——只在有编辑时才发生，
和"漏掉最该保存的那一笔"比，这个代价可以忽略。

顺带挖出第二处同类的洞：`undo` / `redo` 走 `applyHistoryPosition`，那里 `restoringHistory` 是抬起的，
`modelChanged` 在第一行就早退，所以 `recoveryDirty` 得在那儿手动置位。不置位的后果是
"编辑后又撤销回保存点"这一路不删副本——下次启动拿一个用户已经撤销掉的版本去诱惑他。

**教训**：缓存是为了省一次计算，不是为了让别人依赖它的新鲜度。凡是在**定时器**里读缓存，
先问一句"它是不是在另一个时机被写的"。定时器不排队、不等你。

## P85 — 拆 13,125 行：脚本比手抄可靠，但脚本也会骗人

### P85-A 函数名的"起点"不是函数名那一行

机械切分靠正则抓 `PianoRollComponent::(\w+)`，抓错的地方有四种：

- **返回类型里也有 `::`**：`PianoRollComponent::ClipEdge PianoRollComponent::edgeOfClip (...)`、
  `PianoRollComponent::GroupClip PianoRollComponent::captureGroupClip (...)` —— 取第一个匹配
  得到的是返回类型，函数会挂到别人的文件里。**取最后一个匹配**。
- **析构函数的 `~` 不在 `\w` 里**：`PianoRollComponent::~PianoRollComponent` 加 `~?\w+` 才抓得到，
  否则 `~PianoRollComponent` 那段块无名、被当成"未分配的块"。
- **返回类型可以单独成行**：`std::pair<juce::Rectangle<float>, juce::Rectangle<float>>` 一行，
  函数名在下一行（`splitCurveRow`）。块的首行没有函数名，要从**整块文本**里提名字，
  不能只看首行。
- **文件级 `static` 辅助也在顶层**：`static float groupValueToY (...)` 没有 `PianoRollComponent::`，
  得靠后备正则 `[\w:<>,\s\*&]+?\b(\w+)\s*\(` 提名字，并显式分配（它只被编组自动化那三个函数用）。

兜底不是猜，是**断言**：分组表里每个名字必须恰好出现一次，解析出来的每一块必须有归属，
否则脚本直接报错退出。316 个函数，靠眼看是看不完的。

### P85-B 生成脚本的输入就是它自己的输出

`PianoRollComponent.cpp` 既是切分的源、又是十一个输出之一。第一次跑完，源文件从 13,125 行
变成 1,233 行；第二次跑时 `roll_segments.json` 里的行号全部指向别处，报错是
`StopIteration`（找不到 `namespace` 那一行），看着像脚本语法问题，其实是数据错位。

两条防线：**每次跑之前 `git checkout -- Source/UI/PianoRollComponent.cpp`**；脚本开头
`assert len(lines) > 13000, 'SRC already split - git checkout it first'`。
"再来一次"应该是幂等的，不幂等的生成脚本要在第一行把自己拦住。

### P85-C "编译过了"不等于"一个字没丢"

internals 头是从两段匿名 namespace 生成的。第一版从 `namespace` 那一行开始取内容，
于是段**前面**的引导注释被吞了——`// Phase 82: one row, two buttons` 这种解释"这两个下拉
为什么长这样"的话，正好在 `namespace` 上面。

编译器当然不报错。发现它的是**逐行比对**：把 HEAD 版本的原文按非空行拆成多重集，
和新文件（十一个 cpp + internals）的非空行做减法，允许两种规范化（internals 加了
`inline ` 前缀、两处 `namespace` 包裹换成了 `namespace roll`）。剩 5 行差异，全是预期的。
10,493 个非空行，丢一行都看得出来。

拆分类重构的验收标准不是"能编译、断言全过"，是**这一条**。断言保的是行为，
行为可以完全正确而注释、空行、`//====` 分隔线已经错位到别处。

### P85-D CRLF 让咬测静默失效

`MainComponent.cpp` 是 CRLF（`SelfTest.cpp`、`MainComponent.h` 是 LF，仓库里两种并存）。
python 里 `s.replace('    if (...)\n        return false;', ...)` 对 CRLF 文件**匹配不到**，
`replace` 不报错、返回原文——于是咬测跑的是**没改过的代码**，结果"全过"，看着像
"这条断言咬不住"。前后浪费两轮。

判据很简单：替换完 `print(t != s)`，或者立刻 `grep` 一眼。**改文件前先问它是哪种行尾**；
用 Edit 工具改 CRLF 文件是安全的，用脚本改就必须带 `\r\n`。

---

## P86 — 采样路径相对化：三次"看着对"的错

### P86-A 加载失败时，相对路径留在了模型里

`ProjectIO` 原来的读法大致是：从文档取 `path`，试着 `loadSampleFile`，失败就把那个字符串
留在 `SampleEvent::path` 上。加了相对路径之后，这个"失败"的后果从"路径不存在"升级成
"**模型里存着 `media\kick86.wav`**"——一个没有基准的相对路径，违反 P86 的第一条规矩。

咬测三（把 `resolveExisting` 的旁路回落全去掉）跑出来的诊断是 `media\kick86.wav`，
一眼看出模型被污染了。修法：读盘时**先无条件** `sample.path = audioFile.getFullPathName();`
（`audioFile` 是 `resolveExisting` 的返回值，找不到也是绝对路径），再尝试加载。

教训：**"回落"和"身份"是两件事**。判定用哪个文件可以层层回落，但那个答案一旦非空就必须
是绝对的，不能让"候选路径"顺着错误分支流进模型。

### P86-B `juce::File::isAbsolute` 不存在

五处编译错。JUCE 8 里只有静态的 `juce::File::isAbsolutePath (juce::StringRef)`，
没有成员函数版本。而且它对**空字符串**和默认构造的 `File` 要额外想一下，
所以 `SamplePath.cpp` 里包了一个匿名 namespace 的 `isAbsoluteFile(const juce::File&)`
（空 File 答 false）。

### P86-C 断言的"必要条件"被写成了充分条件

`工程下面的采样写成相对路径` 这条原本只写 `resolve (inside, project) == wav`。
这条件在**所有路径都退回绝对**的实现里照样成立——相对化功能完全没生效，断言全过。

补上 `&& ! juce::File::isAbsolutePath (inside)` 才算指名了要哪一种形式，
咬测才从"挂 3 条"变成"挂 4 条"。

顺带一个容易想反的点：**工程外面的文件也写成相对路径**（带 `..`），
条件是 `resolve` 解回来是同一个文件。所以"绝对 vs 相对"的分界线不是"在不在工程目录里"，
是"**解不解得回来**"。跨盘、符号链接、解析后落到别处的 `..` 链才退回绝对。

规律：**"往返一致"是最弱的一类断言**。它证明不了选对了形式，只证明没有把数据弄坏。
凡是有"两种正确写法"的地方，断言必须**指名**要哪一种。

---

## P87 — 循环区间：四处咬测，五处假象

### P87-A `PlaybackEngine` 的默认采样率是 44100，不是设备采样率

断言 3 挂出来是 `10.177 / 8.354 / 10.531`。原因：引擎默认 44100，测试往
`renderNextBlock` 里塞了 48000 采样一块 = **2.177 拍**，于是"播到 8 就回绕"变成了播到 10.177。

修法只在测试侧：先 `prepareToPlay (48000.0, 48000)`。**任何数拍的音频断言都必须先
`prepareToPlay`**，否则你测的是"一块有多长"，不是"循环在哪回绕"。

### P87-B 断言本身写错了，于是"夹没夹"都过

想测"区间越过工程末尾时被夹住"，原写法：从 60 起播一块，看位置是不是 64（工程长 64）。
但一块 48000 采样是 2 拍，从 60 播到 62 —— **恰好不等于 64**，夹住和不夹住都过。

改成从 **62** 起播：夹住了答案 60（`wrapStart` 被 `jlimit(0, length)` 压到 60），
没夹住是 64。重咬之后正确挂 2 条。

规律：**断言的两个分支必须给出不同的数**。写出"如果坏了也会得到同一个答案"的断言，
等于没写，而且它会一直占着 PASS 让人误以为覆盖了。

### P87-C 循环带把标尺上的时间文字压掉了

第一版是标尺底部 `ui::px(12)` 加一个"循环"字样。截图一看，`0:06.0` 被盖住半行。
标尺上每一个像素都在说话（拍号、小节线、时间），一条新带子只有"贴着底边"这一种活法。

改成 `ui::px(7)`、去掉文字，把"这是一个循环"的表达挪到**车道底色 + 两条竖线**
（`drawGridLines` 开头），那里是空的。断言 `>= ui::px(8)` 相应放宽到 `>= ui::px(5)`。

### P87-D 关闭状态也画括号：18/29 张快照凭空多一个虚框

`drawRuler` 里原本只判了 `! bar.isEmpty()`（标尺可见），没判 `loopRegion.isUsable()`。
于是**每一个空白工程**的标尺上都出现了 0–4 拍的循环括号——因为 `LoopRegion` 的默认值
`startBeat=0, endBeat=4` 是合法的，只是 `enabled=false`。

视觉回归从 20/29 掉到 2/29 才发现。修法是加 `&& loopRegion.isUsable()`。

规律：**"默认值合法"不等于"默认值可见"**。默认构造出来的东西是"还没被用户说过话"，
界面上必须一个像素都不占。

### P87-E `-loop` 快照放在序列中间，把它之后拍的每一张图都染上循环底色

`createLoopSnapshot` 会改会话状态（打开循环、设区间），所以它之后的快照全部受影响。
挪到快照序列**最后**（`writePng("-loop", …)` 紧挨着 `.log` 那行之前），视觉回归从 13/29
涨到 20/29 逐字节相同，剩下 9 张就是已知抖动集（`mixer` / `recovery` / `selectmark*` /
`sampler` / `groupauto` / `slotparam` / `mixer-narrow`）。

规律：**会改状态的快照函数必须排在最后**。这一条和 P84 的恢复、P86 的重定位弹窗是同一类
问题的三种表现——无头运行的副作用顺序就是回归结果的顺序。

### P87-F CRLF 又一次让咬测"没有真的写进去"

想咬 `setRange` 的排序，写的替换 anchor 是
`juce::jmax (lo + LoopRegion::minLengthBeats, …)`——但文件里在类作用域内，写的是
`minLengthBeats`，没有命名空间前缀。`assert s.count(old) == 1` 挡下了这次（这次我写了
assert），可**上一次**同样的错误没有 assert，于是"782/0"是未改动代码跑出来的假通过。

这是 P85-D 的第二次复发。现在的固定动作：**替换前 `assert count == 1`，替换后
`grep -n` 一眼**。CRLF 文件再加一条：`io.open(..., newline='')` 读写保持原样，
但**插入的新行必须自己带 `\r\n`**——用 `\n` 插进去会制造一行"孤儿 LF"，
之后 `git diff` 会把整个文件算成改动。

## P88 — 节拍器与预备拍：断言得先能咬住自己

### P88-A 一条"在更宽的错下也成立"的断言，等于没写

3/4 拍那条原本写的是：

```cpp
check ("3/4 拍：第 3 拍是重音", isAccent (clickAt (3)), …);
```

看着没问题，咬测时把重音判据改成恒 `true`（每拍都重音），**它照样过**。
因为"第 3 拍是重音"在"每拍都是重音"的实现下是成立的——它只否证了"第 3 拍不是重音"，
没有否证"第 1 拍也是重音"。

改成同时比同一个 3/4 小节里的第 1 拍：

```cpp
check ("3/4 拍：第 3 拍是重音，同一个 3/4 小节里的第 1 拍不是",
       isAccent (clickAt (3)) && ! isAccent (clickAt (1)), …);
```

重咬后正确挂 2 条（4/4 那条也一起挂，因为 `perBar` 被写死成 `true`）。

规律：**断言必须有一个"错得更宽"的相邻实现**。凡断言形式是"X 具备性质 P"，
先问一句"有没有一种明显更糟的实现也让 X 具备 P"。有，就把那个实现的反面也写进去。

### P88-B "挂了 2 条"不等于"挂的是这 2 条"

第 12 处咬测（删掉 `setMetronomeEnabled()` 里的 `applyMetronomeSettings()`）跑出来
显示挂 2 条，但那是**上一处咬测**（重音判据）留下的读数——`Source/MainComponent.cpp`
是 CRLF，而 anchor 写的是 LF 版本，`assert s.count (old) == 1` 当场挡下、补丁没落盘，
编译出来的是没改过的代码。

这次拦住了，所以只白跑一轮；**前两次（P85-D / P87-F）没写 assert，直接出了一份
假通过的 782/0**。

除 assert 之外，还差一道：**看挂的那几条叫什么名字**。判据不是"有没有挂"，
是"挂的是不是我刚改的那一行管的那几条"。这一轮之后固定成三步：
`assert count == 1` → 替换 → **先读失败清单再数条数**。

### P88-C `juce::AudioBuffer<float>` 两者都没有：`fill(float)` 和 `add(float)`

写"用 1.0 填满一块"时顺手写了 `buffer.add (1.0f)`，C2039。`AudioBuffer` 只有
`clear()`、`applyGain()`、`addFrom()`、`copyFrom()`。

要填一个常数：

```cpp
juce::FloatVectorOperations::fill (buffer.getWritePointer (ch), value, numSamples);
```

（`juce::AudioBuffer` 在 P23 之后的记忆里一直有"没有 add"这一条，但"没有 fill"是新的：
`juce::AudioSampleBuffer` 时代也没有，只是从来没需要过一次填满。）

### P88-D `const` 成员函数里不能问引擎

`getCountInBeatsLeft() const` 里写 `return audio.getPlayback().getPreRollBeatsLeft();`，C2662：
不能把 `const MainComponent*` 转成 `PlaybackEngine&`（`getPlayback()` 是非 const）。

两个选项：给 `getPlaybackForTest()` 那样的 const 重载，或者去掉 `const`。
**去掉 `const`**，因为它本来就不是"只读这个对象"——它问的是实时音频线程的状态，
把这种访问标成 const 是个谎。声明与定义一起去掉，别只改一边。

### P88-E 一个上窄下宽的外框加一根斜线，读成 λ

第一版节拍器字形：梯形机身 + 从底边中部斜向右上的臂。8 倍放大截图一看，是个 **λ**。

缺的是**配重**。Maelzel 节拍器那根摆杆上有个能上下滑的铅块，那个圆是整个图标里
唯一"不是轮廓"的部件——有它，读者才认出"杆上挂着东西，所以这是个节拍器"；
没有它，剩下的就是"一个字母加一条斜杠"。

补 `armEnd` 处的 3.2px 实心圆，同时把 `halfBase` 6.0 → 6.4、机身放到 `ui::px(34)` 宽。
三次才过。规律：**图标能不能读出来要靠放大看，不能靠想象的描述**——
`--cursor-preview` 那条工具链和 8 倍截图就是为这个准备的。

### P88-F `-metronome` 又踩了一次"会改状态的快照必须排最后"

`createMetronomeSnapshot` 会 `setMetronomeEnabled(true)` + `setVolume` + `setCountInBeats`
+ `startPreRoll`。P87-E 才刚把 `-loop` 挪到最后，这一轮又添了一个同样会改状态的。

现在 `Main.cpp` 的快照序列尾部是 `… → -loop → -metronome → (.log)`。
**新增任何"演示某个状态"的快照，都往队尾放**；默认行为是排队尾，不是排在"主题相近的位置"。

### P88-G 533 个 png 让 `rm -f *.png` 直接报 Argument list too long

P87 那一轮咬测产物攒了 533 个文件，`rm -f p87bite*.png p87final*.png …` 的通配展开超过
Windows 的 32 KB 命令行上限，整条命令失败。更麻烦的是它**是和"重编 + 跑自检"串在一条
`&&` 链里的**，于是自检根本没跑，而我第一眼看的是"自检没输出"。

清理用 python 的 `glob` + `os.remove`：

```bash
python -c "
import glob, os
for p in glob.glob('build/AtomicDAW_artefacts/Release/p8[78]*bite*.png'): os.remove(p)
"
```

规则：**清理动作不要和验证动作串在同一条 `&&` 链里**。清理失败会伪装成验证失败。

## P89 — CI：一条管道就能让闸门永远开

### P89-A `| Tee` 之后 `$?` 是 tee 的，坏构建会绿着过去

想要"留日志 + 不吞输出"的人都会这么写：

```powershell
cmake --build build --config Release 2>&1 | Tee-Object -FilePath build.log
```

管道本身成功（tee 写成了文件），于是这一步的状态是成功的，**即使 cmake 失败了**。
GitHub 的 pwsh 包装层会 `exit $LASTEXITCODE`——`$LASTEXITCODE` 恰好还留着 cmake 的码，
所以这条**碰巧**能用；但同一条纪律在 bash 里就是 `pipefail` 没开的经典坑，
而且把 `run:` 写成多行脚本、末尾还有别的命令时，那个 `exit` 就不再是最后一个。
唯一稳的写法是当场判：

```powershell
if ($LASTEXITCODE -ne 0) { throw "build failed (exit $LASTEXITCODE)" }
```

后果比"没闸门"更隐蔽：一个编译器错误会被下游报成"self-test: not built: …exe",
而下一个人会去查构建产物路径。

### P89-B CI 跑在仓库根，本地一律跑在 exe 目录里

四个月来每一次 `--snapshot` 都是 `cd build/AtomicDAW_artefacts/Release && ./AtomicDAW.exe`。
CI 从仓库根跑，cwd ≠ exe 目录——**这一条从来没被验证过**，而它正好压在两类断言上：
`addDevPluginPaths()` 找那两枚测试 VST3，以及自检顺手 `saveProjectToFile("demo.atomic")`。

好在 `addDevPluginPaths` 是拿 `File::currentExecutableFile` 往上走四层，与 cwd 无关；
出图落在**参数 png 的同级目录**。本地实测仓库根运行仍是 810/0（VST3 发现 1+1）。

规律：**凡是 CI 与本地"启动形态"不同的地方，都要在本地真跑一遍那种形态**。
不跑就会在 CI 上第一次知道，而 CI 的第一条红信息通常指向错误的地方。

### P89-C 一条会喊狼的闸门，最后一定被人关掉

视觉回归在 CI 上做逐字节比较很诱人（数据都有），但 8 张图**本来就会抖**：
电平表读数、播放头位置、恢复时间戳。红的理由永远不是缺陷。
第一次红了查十分钟、第二次红了查五分钟、第三次就有人加 `continue-on-error`。

所以 CI 里只判那四件**确定是缺陷**的事（退出码 / FAIL 数 / 标记 / 断言下限），
30 张图当 artifact 传上去给人看。P87-E 那份"已知抖动集"清单不是给 CI 豁免用的，
是给 CI **不碰**用的。

同一条也适用于 `/WX`：工具链更新会在 JUCE 头文件里加一个新警告，那不是本仓库的缺陷。

## P90 — 量化：位置也会跟着形状一起走

### P90-A 出现原点是**存下来的数**，不是成员的最小值（算出来的）

`Layer::occurrenceStart` 只在缺失时从成员materialise，之后就一直用存的那个数（P62 定的，
为的是"删掉最早那个成员不许让整个出现搬家"）。所有**拖动**都会同时挪原点
（`applyOccurrenceOriginsForMove`），所以平时看不出来。

量化是第一个"改了成员位置但没挪原点"的操作，症状非常安静：`syncGroupShape` 用
`副本原点 + 源码相对形状` 落位，源码的原点还指着成员的旧位置，于是一份副本整体平移了
**源码第一个成员的量化误差**。看绝对值像是"量化得挺准"，看相对形状也完全正确——
只有把锚点在量化前后各打印一次才看得见。

规则：**任何不经过拖动路径的成员位移，都要重锚定**
（`setOccurrenceStart(groupId, occurrence, 该出现的最小起点)`）。

### P90-B 写 `.cpp` 补丁时，`newline=''` 读出来的是 `
`

`PianoRollSelection.cpp` 是 CRLF。脚本里 `assert s.count(old) == 1` 用的是含 `
` 的字面量，
一个都匹配不上——但**断言会当场报错**，不会静默写入，所以这次只是白跑一轮编译和一次自检
（跑的是旧二进制，报的是上一轮的 FAIL）。固定动作：读进来先 `old.replace('
', '
')`，
写回时保持 `newline=''`。

### P90-C "咬不住"的兜底行，是多余的兜底

写 `apply` 时先写了 `const double amount = std::min (1.0, strength);` 再判 `amount >= 1.0`。
咬测把 `std::min` 删掉，**一条都没挂**——因为早退先接住了所有 `strength >= 1`。
留着它等于留一句"这里好像需要夹一下"的错觉。改成直接用 `strength` 判早退，
再咬 `>= 1.0` → `== 1.0`，立刻挂一条（0.14 被外推到 0.36）。

**判据**：删得掉且没有断言挂，那行就不是防御，是噪声。

## P91 — 拍号：一个值推给三个读者，和一个不会重编的还原

### P91-A `shutil.copy2` 还原会带回旧 mtime，MSBuild 于是不重编

咬测脚本的标准形态是"备份 → 打补丁 → 编译 → 跑 → 还原"。`copy2` **连 mtime 一起复制**，
所以还原后源码的时间戳比刚才那次编译产出的 `.obj` 还旧，下一轮 `cmake --build` 判定
"源文件没变"、直接跳过。症状：**失败数一轮比一轮多**（上一咬的变异全在里面），
每一次都看起来"命中得太多了"，看不出哪一条是自己的。

修法：还原之后 `os.utime(path, None)`。判据不是"FAIL 数 > 0"，是**每一咬挂的正好是
该挂的那几条**；数在累加就说明变异没被撤掉。

### P91-B 同一件事写了两遍兜底，咬测只会碰到其中一遍

读 `<Transport beatsPerBar>` 时写了两层默认：查表**之前**无条件
`*beatsPerBar = defaultBeatsPerBar;`，以及 `getIntAttribute ("beatsPerBar", defaultBeatsPerBar)`
的默认参数。第一版咬测删的是**属性存在性判断**（外面包一层 `hasAttribute`），**一条都没挂**
——因为外层那个无条件赋值已经把答案填好了。

两层里只有一层承重：**元素不存在**的时候（v28 里一首没循环的歌，根本没有 `<Transport>`），
`getIntAttribute` 不会被求值，救场的只有外层赋值。所以断言要分两种老文档：
**属性缺失**（元素在）与**元素缺失**。前者咬 `getIntAttribute` 的默认参数，
后者咬外层赋值；只写前者，承重的那行永远没被咬到。

### P91-C 角落那一击要自己 `return`

标尺左上角那格（键盘宽 × 标尺高）几何上不属于标尺、也不属于键盘，但在 `handlePress`
的分支顺序里，它会**落进标尺那一支**——于是点一下要拍号，播放头也顺带被拖到 0。
顺序上放最前，并且 `return`。

自检不只断言"菜单回调响了"，还断言**播放头没动**：删掉 `return` 时前者依然通过，
后者才会挂。

### P91-D 同一个菜单里两组 id 撞车，撞输的那一项是死的

给节拍器菜单加「每小节拍数」时顺手读了一遍回调：音量那组是 `100 + percent`
（100/125/150/175/**200**），预备拍那组是 200/201/202。**200 是两组共用的**，而回调里
音量那支写的是 `result >= 100 && result <= 200` 且排在第一——于是点「预备拍 ▸ 关」
把音量设成 100%，预备拍一动不动。这一项从 P88 起就是死的，只是没人点过它。

修法是 id 挪到 210/211/212（不是 190/191/192：那三个落在音量区间里），并且把音量那支
改成 `>= 100 && < 200` 加一条显式的 200。**菜单 id 没有命名空间**——同一个
`showMenuAsync` 回调下的每一组都要各占一段，加新组之前先把旧组扫一遍。

## P92 — 拖放有两个动词时，答案由指针底下是什么决定

### P92-A 空栈没有活动图层，前四条断言会"通过"在错的原因上

`LayerStack::addLayer` 不设 `activeIndex`，新栈的 `getActiveLayerIndex()` 是 **-1**。
`getGroupReplaceTargetAt` 第一条守卫就是"源组必须在本图层"，于是自检里前四条
（成员上是谁 / 框里空白是谁 / 自己组的音符不算目标 / 别的图层不是目标）全部读到
`(-1, -1)`。三条该"通过"的没通过，而**第四条该失败的却"通过"了**——它期望 -1。

发现它是因为第二条（跨图层不是目标）通过了、第一条（成员上是它）没通过：两条用同一个
-1 却给了相反结论，说明 -1 不是答案。

修法：测试里显式 `stack.setActiveLayerIndex (0)`。**别依赖后续动作的副作用**——
第五步的 drop 会顺手把活动图层设成 0，但那是 `placeGroupAppearance` 的行为，
断言挂在别人的副作用上，改那个函数时这一整个块会集体失灵。

### P92-B 组框只为**看得见**的成员生成，看不见就是空框

`getGroupContentBounds` 只把 `intersects(getGridArea())` 的成员并进并集。默认
`scrollY = pitchToContentY(84)`，620 高的窗只显示到大约 pitch 57——把测试音符写在
48 和 55 上，框**全为空**，`getGroupFrameAt` 与一切命中测试都答 -1，"没命中"看起来
像"逻辑写错了"。

自检里的音高全部落在 **84..57** 这个可见窗口内，并且把这条写进了注释。判据不是
"断言过了"，是**诊断里打印出来的那两个数**：-1 / -1 说明连框都没找到。

## P93 — 一个度量、两套数字、一个没打算开声卡的进程

### P93-A 快照进程在 `Main.cpp:258` 之后是真的在跑音频

`--snapshot` 不是为了出图才开声卡的：`component.startAudio()` 那一行是给
「设置 ▸ 音频」那一页填内容用的（设备名/采样率/缓冲大小没有设备就是空的）。代价是
**从那行往后，这个进程就是一个真的开着声卡的会话**，音频线程在跑，负载表在动。

后果有两条，都别忘：
- 拍出来的每张整窗图里，负载读数**每次运行都不一样**（实测同一次运行里从 76 涨到 90）。
  它是永久抖动区，和 `recovery` 的时间戳同性质，比对时按 bbox 遮掉。
- 这也解释了那 90 次丢音不是装饰：出图那几十秒里消息线程在疯狂渲染 PNG，
  引擎在同一个进程里被挤。**判据是"空闲时它是不是 0"**，那要用户开着真窗口看。

### P93-B 预算来自 `numSamples`，所以"回调了"不等于"算一块"

`BlockClock` 的预算是 `numSamples * 1000 / sampleRate`。`numSamples == 0` 时预算
是 0，`perf::advance` 直接返回——**这条回调不进统计**。

自检第一版按"三个出口都算一块"写了 5+5+1=11，跑出来 6：那个 1（0 采样）根本没记。
这不是 bug，是规则本身的样子（没有预算就没有"用了几成"），但断言写的是另一种理解。
改成 5+5=10，并**单独断言那条 0 采样的回调确实不算**——把边界钉住，别让它哪天变成除零。

### P93-C 收敛断言要把 0.2 的滤镜代数先算出来

`smoothing = 0.2` 的指数平均，n 块之后的误差是 `0.8^n`。40 块是 **1.3e-4**，不是 1e-6；
写 `< 1.0e-6` 会挂，而且挂了以后看着像"规则错了"。同理"峰值 400 块之后落到当前负载"
也是错的：400 × 10 ms 只有 4 s，扣掉 1.5 s 保持期只剩 2.5 s × 0.5/s = 1.25 的降幅，
从 2.5 落到 1.25，**还没碰到底**。断言写成"落到底"就得给够时间（800 块 = 8 s）。

先算代数再写容差；容差不是"随便松一点"，它应该是那条公式的具体答案。

## P94 — 补偿是两条链，而我自己先埋了两个坑

### P94-A 自检的严格判据把"合理解释"当成"多咬了一条"

第一版咬测脚本的判据是"挂的正好是这几条，多一条都算 WRONG"。M4（偏移里漏掉监听链）挂了
四条，预期三条，于是被判 WRONG。但多出来的那条 `偏移换算成秒用的是回调的采样率` 是真的该挂：
偏移从 350 变成 300，换算成秒当然跟着错。

**连锁失败不是误报。** 判据应该是"预期的那几条都在"，再加一条人工看过的排除；
写成"精确相等"只会逼自己一次次改预期，最后把脚本改成只认结果。

### P94-B 咬测补丁打不上：同一个仓库里两种行尾

`MainComponent.cpp` 在工作区是 CRLF，`Source/Audio/*.cpp` 是 LF（P85 拆文件时留下的）。
模式写成 `"...\n..."` 在 CRLF 文件里**一次都不匹配**，脚本报 BAD-PATCH，看着像"变异点不存在"。

三个变异点就这么白跑了一轮。修法不是把文件统一行尾（那会造出一个全文件 diff），
而是**按文件实际行尾改写模式**：读到 `\r\n` 就把模式的 `\n` 也换成 `\r\n`。
还原仍然写回原始字节，一个字都不动。

### P94-C `getRecordOffsetSamples` 一读监听开关，就把要补的那一项抹掉了

第一版写得很"讲道理"：监听没开，监听链就不该算。然后 `finishRecording()` 是
**先关监听、再 `commitTake`**——于是监听链那一项永远是 0，而"监听链上挂线性相位插件"
恰恰是这条公式存在的唯一理由。

**偏移描述的是 take 弹的时候那条链，不是文件落地那一刻的开关。** 三个项全部无条件相加。
自检里那条断言（关掉监听后 350 不变）就是钉这个的；变异 M4 咬的就是它。

### P94-D 面板上那 6 张"回归"其实是功能够了

P94 的视觉回归比 P93 多出 6 张差异，bbox 只在块的边缘和组框描边上，看着像渲染出了问题。
逐层回退才定住：引擎单独改 → 26/33 相同（差异全在抖动集）；主界面一加回来 → 6 张立刻出现。

真相是那句"录音 1.wav"就是**一路真 take**，补偿按这台声卡报的延迟把它整体左移了 3–4 px
（左边缘 112→109、275→271）。**是功能正确工作的样子，不是渲染回归。**

但它带来一个副作用要记住：**这 6 张图从此带本机声卡延迟**。换台机器拍出来不一样，
所以判"是不是回归"要先看块的位置有没有动，而不是看张数。

教训：**"多出几张不一样"永远先问"哪一张、差在哪个 bbox、那块东西是什么"**。
三问里任何一问答不出来，就去缩小改动范围重拍一次——比盯着像素猜快得多。

### P94-E 落点上那个 `jmax(0.0, ...)` 是死代码

`clip.startBeat = juce::jmax (0.0, startBeat)` 看着像"最开头的 take 别跑成负拍"的守卫。
把它变异掉，**一条断言都没挂**——因为 `TempoMap::beatAtSeconds` 对负数已经答第一段的起点了。

一个不能咬的守卫比没有守卫更糟：它让人以为零点是它兜的。删掉，把那句断言留着
（它照样能被"完全不做补偿"咬中），并在注释里写清零点到底在谁那儿。

## P95 — take 容器：一个判据量错了，一条指针悬空了

### P95-A 覆盖率按「短的那一段」量，正好把要防的事放进来

第一版写的是"按短的量"，理由还挺像话：重录成 8 小节和重录成 8.5 小节应该一样对待。
然后自检第一条就红了：**8 小节里补 2 小节 = 2/2 = 100%**——短的那段自己就是分子分母，
任何碎片都能拿满分。真按这个规则跑，补录两小节会把另外六小节全部静音。

改按**长的**量：同样的两个例子仍然都进（0.94 / 1.00），补录掉到 0.25 被挡在外面。
"对称"这个诉求原来是"稍微长一点和稍微短一点都算重录"，按长的量同样满足；
它挡住的只有"只弹了一小截"。

教训：**在写下"用 min 还是 max"之前，先各自跑一遍最坏的那个例子**。
这里最坏的就是补录——它是唯一会删掉用户工作的分支。

### P95-B 跨一次录音缓存 `NoteGroup*`，五条断言一起红

`Layer::groups` 是 `std::vector<NoteGroup>`。自检里这样写：

```cpp
auto* g1 = layer->findGroup (pass1);      // 第一遍之后
const int pass2 = playPass (...);          // ← 这里 push_back，可能 realloc
check (g1->getTakeIndex() == 1, ...);      // 悬垂
```

五条断言一起失败，报的值还很像"模型坏了"（`第 2 遍`、`0 个出声`、`1 / 1`）。
真相只有一个：**指针在第一遍之后就已经不指向那个组了**。

规则写进 MEMORY 第 28 条：跨一次模型改动**一律按 id 取**。
顺带一个观察：这类 bug 的表现永远是"好几条一起错，而且错的数看着像另一个功能的"——
先怀疑测试里的引用，再怀疑被测的代码。

### P95-C 只把新 take 标成当前，旧的那遍没关

`joinTakeSet` 第一版结尾是 `incoming->setAsTake (setId, nextIndex, true);`——新的一遍标记成
当前，然后返回。旧的 active 还在。这是"两遍同时出声"那个 bug 的镜像：修了一半。

**"恰好一个"这种不变量，必须由一个能看见全体的函数来保证**：`setActiveTake` 遍历整个 set，
`joinTakeSet` 的末尾直接调它，而不是自己动手改那个字段。字段是公开可写的，
但写它的路只有一条——这是本轮唯一一处"看起来多余的一次调用"。

### P95-D `0.9` 这个边界用 15.2 测不出来

`shouldJoin ({8,16}, {8,15.2})` 期望"刚好 0.9 也算"。实际算出 `7.199999999999999 / 8 =
0.8999999999999999`，差一个 ulp 掉到线下——**测的是浮点，不是规则**。

换成 10 与 9：`9.0 / 10.0` 与字面量 `0.9` 是同一个 double，`>=` 才真的被测到。
**边界测试要先确认那两个数在 double 里能算出想要的那个值**，否则红的是测试自己，
而调试它的代价远高于换一对数。

## P96 — 接缝：判据选错了对象，肩点漏了就是整块

### P96-A 拿渲染里的跳变判接缝，咬不动

接缝的第一版自检是渲染出来的："直流材料本身一步也不跳，所以 ±4 ms 内最大的一步就是
接缝有没有处理好"。看着天衣无缝，**咬测一跑就打脸**：把 `fade` 直接改成 0，最大跳变只从
`0.00040` 变成 `0.00046` —— 因为**采样器自己在块的边沿就有一小段包络**，硬切被它顺手削圆了。

结论：这条判据测的是采样器，不是拼接。改成看**块本身**：进来那一段有没有比接缝早
`fadeBeats` 开始，三块贴在接缝那一端的音量曲线值是不是 0。改完咬测立刻 4 条红。

**判据要选那个"只有我要测的东西能改变它"的量**。渲染看起来更真，但它前面站着一个
会主动修复问题的环节时，它反而更瞎。

### P96-B 淡化少了肩点，4 ms 变成整块

`addEdgeFade` 一开始只做"把曲线上每个点乘上斜坡"。平线（`makeFlat` 是 0.7 的一头一尾
两个点）乘完之后成了 `(0, 0.7)` 与 `(1, 0)` —— 两个点之间的插值就是**整块从 0.7 一路斜到 0**，
中点是 0.35。中间那一段两头都淡，于是两头 0、中间 0，渲染出来是静音。

必须在 `fraction` 处**插一个肩点**，取那条曲线原本在该处的值，斜坡才有终点。咬测 M2
（去掉肩点）一次红了 5 条，连渲染电平都掉到 0.0000 —— 一个漏掉的点，是"淡化"与"静音"
的差别。

**"在一段曲线前面加个斜坡"这句话里，真正的信息是那个斜坡在哪结束**，而不是它在哪开始。

## P97 — 力度 lane：一条几何问三次，四个状态忘了还

### P97-A 新 TU 根本没被编译：`CMakeLists.txt` 是显式列表，不是 GLOB

`PianoRollVelocity.cpp` 写好了、头文件声明也加了，编译出来的却是满屏「未声明标识符 /
LNK2019」。原因很简单：`CMakeLists.txt` 里 `Source/UI/` 是一个一个列的（P85 拆分时就是这么
列的，因为要保证顺序与显式），**新建一个 .cpp 必须手工加进列表**，否则 configure 都看不见它。

这不是"忘了保存"那一类错误：文件在盘上、`git status` 里也在，只是没进编译单元。
新 TU 的检查清单只有两条——加进 `CMakeLists.txt`、跑一次 configure/build 看有没有它的名字
出现在编译输出里。

### P97-B 快照夹具改过的状态会被后面的帧继承

`-velocity` 插在序列第 26 位（后面还有 `-blank` / `-loop` / `-metronome`）。它要拍三样东西：
新加的一层音符、打开的 lane、对准的镜头。三样都是**全局状态**，于是后面三帧拍出了：

1. **一片空网格**——钢琴窗只画活动层，而夹具把活动层换成了自己那层，音符全在 pitch 60–67、
   beat 0–3.5，正好在视野外。
2. **音符丢了白边**——选区被换成了夹具挑的第一个音符。还原时先 `setSelection(notes)` 再
   `setSampleSelection(blocks)`，**后者清掉前者**（这是"一次点击只选一种"的实现），所以要用
   `setMixedSelection` 一次给。
3. **循环带挪了位置、网格矮了 56 px**——镜头与 lane 开关没还。

判据：**抖动不算回归，继承算**。同一份二进制跑两遍，`-blank` 是逐字节相同的，可它和上一版
不同——那说明改的是"谁的状态"，不是"渲染"。修法写在 `createVelocityLaneSnapshot` 末尾：
活动层、混合选区、`scrollBeats`、`topPitch`、lane 开关，五样都还。

顺带补了一个入口 `setTopPitch` / `getTopPitch`：滚动的两条轴以前只有横向能从外面定位，
所以"保证某个音高在画面里"只能靠猜。

### P97-C 在 `MainComponent` 上测撤销，测不到——那一层根本没有那条历史

力度夹具原本写的是：拖完 > 0.5、`host.undo()`、再断言回到 0.5。结果 `findNote` 返回 -1，
按 id 遍历所有层也找不到（`undone == -1`）。原因是**撤销是整工程 XML 快照**，一次 undo
把整个会话从 XML 重建一遍，活动层索引也可能变——按 id 在旧对象里找，找的是已经不存在的那份。

改成**存盘往返**：0.42 写盘、读回还是 0.42。这条断言不比撤销弱：它同时论证了「lane 没有
新数据通路」——力度就是 `NoteEvent::velocity`，所以 `formatVersion` 不用动（仍 31）。

### P97-D 起步电平挑太低，两根柱子一起撞天花板

"选中两根、拖一根、两根一起动"的夹具一开始用 0.9 与 0.85 起步。拖 10 px 两根都到 1.0，
**差被压平了**，断言"差值保持"变成断言"两个都是 1.0"。改成 0.7 与 0.3：0.4 的差在夹紧之前
走完整个拖动。

**测"相对量保持不变"时，起点必须落在量程中间**——量程两端是夹紧的地盘，那里任何"保持"
都会被它自己吃掉。
