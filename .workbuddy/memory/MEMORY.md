# AtomicDAW — 项目长期约定

JUCE 8 音频工作站：图层叠加工作区 / 音源机架 / 音符级微观包络。分阶段交付，
每阶段只做一件事，必须能编译并跑通视觉或数据逻辑。

**当前阶段、自检基准数、构建命令看 `STATUS.md`**；**为什么这么做在 `REFERENCE.md`**（按主题分节）；
陷阱与咬测记录在 `PITFALLS.md`。本文件只写**不能违反的那一句 + 去哪里找它**，理由不在这里重复。

## 硬规则

1. **中文一律 `atomic::utf8("...")`**，含自检失败信息。裸中文 = 乱码。
2. **几何走 `ui::px()/pf()`，字号 `ui::sp()`**，唯一密度旋钮 `ui::scale`。不缩放的三处：
   `numPitches` / gutter / 内联自动化框最小宽度。
3. **路由只有 `Model/Routing.h`**：`resolve()`（音符）、`resolveSample()`（采样，不看组）、
   `layerSlotId()`（兜底跳过 sampler）。UI 高亮与引擎调同一份。
4. **音频线程不读 Layer / NoteEvent / RackModel**。消息线程 `publishSchedule()` 产出不可变
   `PublishedSchedule`，发布期拍→秒；锁只覆盖 shared_ptr 赋值。
5. **改模型后必发 `layersChanged()` / `notesChanged()`**——`pruneOrphanSamplerSlots()` 挂在
   通知上，不是回调。撤销步合并靠 AsyncUpdater，一趟一次。
6. **唯一入口**：会话重置 `buildSession(withEmptyLayer)`；删槽 `removeSlotAndItsClips()`；
   撤销 = 整工程 XML 快照、**粒度是一次鼠标手势**（`beginUndoGesture()`/`endUndoGesture()`
   包住按下与释放）；试听分岔 `MainComponent::auditionPreview()`。
7. **组的形状只按「起点 + 名字」配对**（`clipShapeLess`）——配对键不能是形状编辑自己会改的字段。
   改了宽度/窗口/曲线/`laneSpan`，一律 `syncShapeOfClip()` / `syncGroupShape()`。
8. **编组默认是对象，不是容器**（P73）：`enterGroupEdit()` 之前组内按下只回答「整个出现」。
   状态只有 `editingGroupId`，在 `layersChanged()` 里**无条件**清——撤销/加载会把同一个 id 带回来。
9. **按下的话事权归「你以为自己在按的东西」**（P75）：两块叠在同一拍同一行时，按在已选中块的
   中间必须是那块（`clipEdgeAt` 答 `(id, none)`）。组编辑态下组框内的空白不是把手。
10. **选中一个成员 = 选中它所在的编组**（P74）：放进选择的手势先扩成整次出现
    （`armGroupMove` / `expandToWholeOccurrences` / `addOccurrenceToSelection`）。**扩散只在手势层**，
    不写进 `setMixedSelection`——底层必须是精确集合。
11. **打开不是选择**（P76）：双击进组只改 `editingGroupId`，不重写选区；按在已选中成员上不能
    替换选区（`else if (! isSelected)`）。判据是「谁会被缩放」（`dragResizeNoteId`）。
12. **双击 = 打开，顺序照绘制顺序**（P78）：采样那一问排在 `hitTestActiveLayer` **之后**（块在
    音符下面）。机架卡上**名字是改名、其余是打开**（S/M/推子/caret 排除）。只有插件槽有窗，
    内置合成器答 false。
13. **名字是一份数据，不是一个副本**（P79）：三个名字各只有一处字段，入口都是**双击那行名字**
    就地改（`NameEditor`，位置 = `getNameBounds()`）。例外只有 `SampleEvent::name`。改名按 **id**。
    `namesChanged()` 默认转发 `layersChanged()`，钢琴窗 override 成纯 `repaint()`。
14. **速度一旦会变，时间就是积分**（P80）：唯一换算是 `Model/TempoMap`，`secondsPerBeat` 已从
    全工程消失。段内 BPM 线性 ⟹ 求秒取对数、反解取指数；**平坦分支走线性形式**。
    `renderNextBlock` **每段**重算 `beatsPerSample`。曲线值到 BPM 是 `60·4^v`。速度图不进
    `MixerModel`（`resetToDefault()` 不该抹掉它）。
15. **录制的拍号在事件发生的地方取**（P81）：回调里读 `getPositionBeats()`，**beat 随事件一起**
    `callAsync` 过桥。音频线程只往预分配 buffer `copyFrom`，满了自己停不回绕；`finish()` **先读
    `writePos` 再 disarm**。写 lane 前必须 `decimateRecordedCurve`（上限 32 点），**少于两点不写**。
16. **一条曲线 = 一个形状 + 一个去处**（P82）：`Model/CurveBank` 三条不变量——**目标至多归一条
    曲线**、**列表永不为空**、**用 id 不用下标**。**const 读永不创建**（`idleCurve`/`idleLane`）。
    **「空」在磁盘上是「没有 `target` 属性」**。
17. **两条路径汇到一处，慢的那条不能被拉快**（P83）：`pdc::solve` 是唯一延迟表，补偿只在
    **汇合点**做，绝不延迟源头。**没有插件报延迟 ⟹ 每张表都是 0 ⟹ 渲染逐位等于 P83 之前**
    ——别加"至少 1"这类兜底。延迟线是记忆体：locate / 停止 / `resetMixState` 都必须清。
18. **恢复副本是「一份副本」，不是「那个工程」**（P84）：**写它的路绝不能改 `currentProject`**。
    不变量：**副本存在 ⟺ 有未保存编辑且进程没能正常收尾**。启动只 offer **严格更新**的那份。
    写入走 `File::replaceWithText`（TemporaryFile），**不要 appendOpen**。恢复出来的会话
    **仍算未保存**。身份只来自它自己的 `recoveryOf`。**默认关闭**，只有窗口路径打开。
19. **一个类可以住在多个 TU 里，但共享的辅助必须 inline**（P85）：`PianoRoll*.cpp` 拆的是
    **文件边界不是逻辑**。共享层 `UI/PianoRollInternals.h`（`namespace atomic::roll`，每个定义
    `inline`）。**进不进 internals 的标准是"几个 TU 用"**。验收是**逐行比对**，不是"能编译"。
20. **模型只存绝对路径，相对是文档的形式**（P86）：`Model/SamplePath` 是唯一路径规则层。
    **相对路径必须解得回原文件才写**；**读盘时把修好的位置写回模型，找不到也写绝对**。
    **重定位不许自动发生**。恢复副本的相对路径以 `recoveryOf` 指向的工程为基准。
21. **循环是一个区间，不是一个标志**（P87）：`Model/LoopRegion`，不变量全是关于**顺序与最短**
    （0.25 拍）。**开关与数字分开**，关掉不许忘记位置。关闭时渲染退化成 `[0, length)`。
    **`isUsable()` 是唯一判据**；`loopMove` 每次从 `loopDragOriginal` 重建。
22. **点击是监听，不是声音，也不是定时器**（P88）：`Audio/Metronome` 混进 `renderNextBlock` 的
    **每个 segment**，`renderInto`（导出/分轨/渲染成采样）里没有——"导出拿不到点击"是**结构性的**。
    按拍与每块 `beatsPerSample` 驱动。分句 `ceil(fromBeat - 1e-9)` 让块边界那一拍**只算一次**。
    音量是它自己的，重音跟**工程拍号**（→ 24）。**预备拍不改 `positionBeats`**，同一个块里交接。
23. **量化是形状的改动，不是位置的搬家**（P90）：`quantize::apply` 是唯一规则——`round` 不是
    `floor`、`>= 1.0` 早退即夹紧、0 与网格 0 都逐位退回。**只改起点**，音符与采样块都动；
    `moved == 0` 不重绘也不产生撤销步（判"动没动"用 `1e-9`）。编组先重锚定出现原点成该出现的
    最小成员，再**无条件** `syncGroupShape`。出现原点是**存下来的数**，任何不走拖动路径的位移
    都要自己挪它。
24. **拍号是一个值，不是一个 `constexpr`**（P91）：`Model/Meter.h` 是唯一规则，`MainComponent`
    持有它，钢琴窗与节拍器是**被推**的视图。**`<Transport>` 永远写**（否则「4/4 无循环」与
    「3/4 无循环」是同一份文档）；**读侧先无条件赋默认再查表**；`setBeatsPerBar` 不调
    `modelChanged()`。
25. **拖放的下一个动作由指针底下是什么决定**（P92）：`getGroupReplaceTargetAt` 是唯一判据，
    预览与松手都调它。命中顺序照按下顺序：成员（音符→采样块）优先，框在后；**自己组的成员不算
    目标**；**只认同图层**。锚点是**目标的出现原点**。替换的是**那一处**，不是整个组。
    **不许用 A 替换 A** 写在函数里，不靠调用方记得。
26. **一个度量只有一个来源**（P93）：`perf::advance` 是唯一的负载/峰值规则，平滑系数 0.2。
    **丢帧不自己数**（`getXRunCount()` 已含设备原生计数与 JUCE 的估算）。**峰值不夹到 1.0**。
    **预算来自 `numSamples`**：0 采样的回调不算一块，率 0 什么都不记。计时器是 **RAII scope guard**
    （回调有四个出口）。采样率用**回调真正用的那个**。顶栏 `bar.beat.tick` 读 `meter::positionAt`，
    **不许再写字面量 4.0**。
27. **一个 take 有两半，就有两条链、两个偏移**（P94）：监听**默认关**（不加询问就活的输入是
    啸叫）。补偿在 `commitTake` **门口一次做完**。`getRecordOffsetSamples` = 输出 + **输入** +
    监听链；`getMidiRecordOffsetSamples` = 输出 + 监听链——**键盘没走过采集缓冲**，用错一个就把
    同一手的两半拆开。**三个项都不看监听开关**（`finishRecording()` 在 `commitTake` 前就关了
    监听）。换算**走秒不走拍分数**；零点由 `TempoMap` 兜，**不许在落点再加一个死的 `jmax`**。
28. **一遍录音 = 一个容器，只有当前那一遍出声**（P95）：take 集合是**派生的**（成员各带
    `takeSetId`/`takeIndex`/`takeActive`），没有 set 实体。**入组只有一个判据** `Layer::joinTakeSet`，
    覆盖率按**长**的那一段量（0.9 起）。**提升是隐式的**（普通组被第二遍撞上才变成 take 1）。
    **`setActiveTake` 是唯一写 active 的地方**，`joinTakeSet` 末尾必须调它。发声门在发布
    （`GroupMix::takeSilent` → `gainAt` 一处，音符与块共用）；**它不是 mute**。存盘 **没有
    `takeset` 就是普通组**、**没有 `takeactive` 就是当前**；加载后 `normaliseTakeSets`。
    **`groups` 是 vector**：不许跨一次录音缓存 `NoteGroup*`，按 id 取。
29. **拼接是「把这段换成那一遍的材料」，不是一个分段表**（P96）：`Model/Comp.h`
    （`namespace comping`）是唯一规则。**不存 provenance 是设计**。**第一次拼接先分一份**
    （`NoteGroup::comp`，v31 `takecomp`，只在是 take 时写）；第二处接缝改同一份，不再分。
    **接缝要的是重叠，不是前后相接**（进来那段比接缝早 `fadeBeats` 开始、晚 `fadeBeats` 结束，
    两侧邻居各在自己那端淡到 0）。**淡化在原曲线上乘**（`comping::addEdgeFade`）**且必须插
    肩点**。**切块只走 `splitSampleBlocksRaw`**。**接缝的分辨率不在渲染里**——判据看**块本身**。
30. **一根柱子只有一个矩形，一次拖动只算一个 delta**（P97）：`Model/VelocityLane.h` 是唯一
    数值规则（**线性轴**不是 dB、**地板 0.02 不是 0**——0 是静音与删除的语义）。`velocityBarFor`
    是唯一几何，paint / `velocityBarAt` / `marqueeInLane` 都问它。**按下记的是 delta**
    （`velocityOriginals`），绝对值会让框选一排后的第一个像素把重音削平。**lane 里的空白是框选区**：
    `marqueeInLane` 为真时 `hitsInMarquee` 只收柱子**并直接返回**。力度**没有新数据通路**
    （改 `NoteEvent::velocity`，`formatVersion` 不动）。**快照夹具必须把它改过的状态还回去**：
    还原要用 `setMixedSelection` 一次给（`setSelection` 与 `setSampleSelection` 各清另一半）。
31. **说「某功能已有模型」之前先 `grep` 那个符号**（P98）：**「我上次写的」不是证据，源码是**。
    **能复用就不新建**：CC 就是「绝对拍的 0..1 折线」，直接复用 `AutomationLane`。
    **要让规则在无声卡环境里咬得动，就把「何时发送」抽成纯函数**（`midicc::valueToSend`）。
32. **一条信号走两条路时，拓扑和延迟是同一个答案**（P99）：`send` 是**并行**的一份，不许指向
    主输出（那条路由 `out` 在做）。**控制台从链变成图**：`wouldLoop` 是 BFS
    （`MixerModel::reaches`），凡是 `while (ptr = next(ptr))` 形状的判据都要重看。
    **`pdc::solve` 的边必须带延迟**（post = `own[from]`，pre = 0），排序与延迟表问同一个
    `edgeLatency(from,to,offset)`。**推子前要第三条线 `pdcPre`**（插入就地改写 bus）；`pdcExit`
    **一块只 push 一次、多处读**。**return 不是新概念**：被指的通道就是 return（`isSendTarget`）。
    验收是「副本按单位增益混进直连，逐样本等于单份的 2 倍」——**前提是信号真的流过被测对象**
    （槽没指派到通道时声音走直连总线，通道循环整块被跳过）。
33. **键是「喂探测器的发送」，所以它没有新概念**（P100）：侧链 = 一条发送，落点不是总线而是
    压缩器的探测器。**存在听者身上**（`channelKeys[listener] = donor`），所以它在图里是一条
    **从源指向听者**的边，`reaches` 必须多扫一遍「谁在听我」——否则 `0→3→4→0`（其中一条是键）
    这种环 `wouldLoop` 看不见。`edgeLatency` 里唯一读**对端**的分支就是它
    （`channels[to].keySource`），`offset = own[source]`，**没有 pre 版本**。**`noKeySource` 是
    -1 不是 0**（0 是 1 号通道；构造/reset/读盘三处 `fill`）。**模型查环比发布严**（发布只把
    「真的有 COMP 在听」的键算成边）——一条空接线不能拖慢整条通道。DSP 只在一处分叉：
    `applyComp` 的 `level` 从 `fx::KeySignal` 来还是从 buffer 来，`nullptr`（没接）与「接了一条
    静音的线」是两件不同的事。**存盘 v34 `<Channel key>`**，没接不写。
34. **断言的第一步是让被测对象真的被改变**（P101）：插入链空的时候两条抽头拿的是同一份样本，
    所以「抽头位置写错」是没人能看见的改动——P99 的 M6 就这样全绿存活了一轮。夹具里要放一个
    **会改信号**的插入（`drive`，因为**无状态**；滤波器会把尾巴带进下一次渲染）。**非线性插入会
    让「2 倍」失效**：pre 副本是干的、直连是 driven 的，期望值是 `single + dry`，而 dry 要**量
    出来**（旁路后再渲染一次），不能假设。**断言的前提本身要写成断言**（「DRIVE 真的改了信号」），
    否则前提塌了会退化成两条同义断言、一起绿。反过来，**「键的 `delay` 恒为 0」不是缺陷**：
    solve 的键边保证 `entry[L] >= entry[S]+own[S]`，减法永不为正；咬测把它写成 0 而全绿，说明
    那段代码在单层拓扑下不可达，不是测不到。

## 索引
- 目录结构、构建命令（含 `unset *proxy*`）、CI 四条门禁、`formatVersion` 逐版字段、快照夹具
  —— 全在 `STATUS.md` 与 `REFERENCE.md`，本文件不重复。
- 只留两条不能忘的：**加断言的那一轮必须同步调大 `MIN_CHECKS`**；**"选中哪条自动化 lane"
  这类状态必须显式写属性**，不能靠元素顺序推断（先解析到临时对象、末尾一次性提交）。
