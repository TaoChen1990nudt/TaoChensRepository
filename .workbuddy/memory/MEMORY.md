# AtomicDAW — 项目长期约定

JUCE 8 音频工作站：图层叠加工作区 / 音源机架 / 音符级微观包络。分阶段交付，
每阶段只做一件事，必须能编译并跑通视觉或数据逻辑。

**当前阶段、自检基准数、构建命令看 `STATUS.md`**。**详细规则在 `REFERENCE.md`**（按主题分节）；
平台陷阱与自检夹具在 `PITFALLS.md`。本文件只留跨主题硬规则 —— 每条只写"不能违反的那一句"，
写法和理由去 REFERENCE 查。

## 硬规则

1. **中文一律 `atomic::utf8("...")`**，自检的失败信息也算。裸中文 = 乱码。
2. **几何走 `ui::px()/pf()`、字号 `ui::sp()`**，唯一密度旋钮 `ui::scale`，禁止裸像素。
   `numPitches` / gutter / 内联自动化框最小宽度不缩放。
3. **路由只有 `Model/Routing.h`**：`resolve()`（音符）、`resolveSample()`（采样，不看组）、
   `layerSlotId()`（兜底跳过 sampler）。UI 高亮与引擎必须调同一份。
4. **音频线程不读 Layer / NoteEvent / RackModel**：消息线程 `publishSchedule()` 产出不可变
   `PublishedSchedule`，发布期把拍换成秒；锁只覆盖 shared_ptr 赋值。
5. **改模型后必发 `layersChanged()` / `notesChanged()`**：`pruneOrphanSamplerSlots()` 挂在这两个
   通知上，不是回调——"谁记得调"不是规则。通知合并成一次撤销步靠 AsyncUpdater，一趟一次。
6. **唯一入口**：会话重置 = `buildSession(withEmptyLayer)`；删槽 = `removeSlotAndItsClips()`；
   撤销 = 整工程 XML 快照、**粒度是一次鼠标手势**（`beginUndoGesture()`/`endUndoGesture()`
   包住按下与释放）；试听分岔 = `MainComponent::auditionPreview()`。
7. **组的形状只按「起点 + 名字」配对**（`clipShapeLess`）：配对键不能是形状编辑自己会改的字段
   ——pitch 曾经是，拖一下下边缘两个副本就互换名字和音频。宽度/窗口/曲线/`laneSpan` 都是形状，
   改完一律 `syncShapeOfClip()` / `syncGroupShape()`。
8. **编组默认是对象，不是容器**（P73）：没双击进去之前，组内成员的按下只回答「整个出现」；
   `enterGroupEdit()` 之后成员才是对象。状态只有一个 `editingGroupId`，失效判据在
   `layersChanged()` 里**无条件**清——撤销/加载会把同一个 id 带回来，存在性检查会答 yes。
9. **按下的话事权归"你以为自己在按的东西"**（P75）：命中测试回答的是"这一像素上画着谁"。
   两块叠在同一拍同一行时，按在已选中块的中间必须是那块（`clipEdgeAt` 答 `(id, none)`），
   否则选择被换掉、多选只剩一个动。组编辑态下组框内的空白不是把手。
10. **选中一个成员 = 选中它所在的编组**（P74）：凡是把成员放进选择的手势都要先扩成整次出现
    （`armGroupMove` / `expandToWholeOccurrences` / `addOccurrenceToSelection`）。**扩散只写在
    手势层**，不写进 `setMixedSelection`——"改一部分才同步形状"全靠底层是精确集合。
11. **打开不是选择**（P76）：双击进组只改 `editingGroupId`，**不重写选区**；**按在已选中的成员
    上不能替换选区**（`else if (! isSelected)`），那是多选后还能整体拖的前提。测这类行为别拿
    "会动的东西有几个"当判据，要问「谁会被缩放」（`dragResizeNoteId`）。
12. **双击 = 打开，顺序照绘制顺序**（P78）：音符/采样块/机架卡片双击各开各的窗；采样那一问必须
    排在 `hitTestActiveLayer` **之后**（块在音符下面）。机架卡上**名字是改名、其余是打开**，
    S/M/推子/caret 一律排除。只有插件槽有窗可开，内置合成器答 false 且不弹窗。
13. **名字是一份数据，不是一个副本**（P79）：图层/编组/音源三个名字各只有一处字段，三处入口都是
    **双击那行名字**就地改（`NameEditor`，位置 = `getNameBounds()`）。唯一例外是
    `SampleEvent::name`（导入时抄来的副本）。改名按 **id** 记住改谁，不用行索引。
    `namesChanged()` 默认转发 `layersChanged()`，只有钢琴窗 override 成纯 `repaint()`
    ——`layersChanged()` 会无条件关掉编辑中的组。
14. **速度一旦会变，时间就是积分**（P80）：`secondsPerBeat` 已从全工程消失，唯一换算是
    `Model/TempoMap`。段内 BPM 线性 ⟹ 求秒取对数、反解取指数；**平坦分支走线性形式**，
    两条分支互换了照样编译得过。`renderNextBlock` **每段**重算 `beatsPerSample`。
    曲线值到 BPM 是 `60·4^v`。速度图不放进 `MixerModel`（`resetToDefault()` 不该抹掉它）。
15. **录制的拍号在事件发生的地方取**（P81）：回调里读 `getPositionBeats()`，**把 beat 随事件一起**
    `callAsync` 过桥；在消息线程里问位置等于把队列延迟算成走音。音频线程只往预分配 buffer
    `copyFrom`，满了自己停不回绕；`finish()` **先读 `writePos` 再 disarm**。写 lane 前必须
    `decimateRecordedCurve`（上限 32 点），**少于两点不写**。
16. **一条曲线 = 一个形状 + 一个去处**（P82）：`Model/CurveBank` 三条不变量——**目标至多归一条
    曲线**、**列表永不为空**、**用 id 不用下标**。**const 读永不创建**（返回 `idleCurve`/`idleLane`）。
    **「空」在磁盘上是「没有 `target` 属性」**，不是一个词。旧访问器必须保住原语义。
17. **两条路径汇到一处，慢的那条不能被拉快**（P83）：`Audio/PdcDelay.h` 的 `pdc::solve` 是唯一
    延迟表，补偿只在**汇合点**做，绝不延迟源头。唯一问延迟的地方是 `PluginHost::getLatencySamples`。
    **没有插件报延迟 ⟹ 每张表都是 0 ⟹ 渲染逐位等于 P83 之前**——别加"至少 1"这类兜底。
    延迟线是记忆体不是常数：locate / 停止 / `resetMixState` 都必须清。
18. **恢复副本是「一份副本」，不是「那个工程」**（P84）：**写它的路绝不能改 `currentProject`**。
    唯一不变量：**副本存在 ⟺ 有未保存编辑且进程没能正常收尾**。启动只 offer **严格更新**的那份。
    写入靠 `File::replaceWithText`（走 TemporaryFile），**不要 appendOpen**。恢复出来的会话
    **必须仍算未保存**。身份只来自副本自己的 `recoveryOf`。功能**默认关闭**，只有窗口路径打开。
19. **一个类可以住在多个 TU 里，但共享的辅助必须 inline**（P85）：`PianoRoll*.cpp` 十一个文件一个
    主题，**拆的是文件边界不是逻辑**。共享层 `UI/PianoRollInternals.h`（`namespace atomic::roll`，
    每个定义 `inline`）。**进不进 internals 的标准是"几个 TU 用"**。验收不是"能编译"，是**逐行
    比对**：按非空行做多重集减法，只允许 `inline ` 前缀与 namespace 包裹两处差异。
20. **模型只存绝对路径，相对是文档的形式**（P86）：`Model/SamplePath` 是唯一路径规则层。
    **相对路径必须解得回原文件才写**；**读盘时把修好的位置写回模型，找不到也写绝对**。
    **重定位不许自动发生**（`relinkSamplesTo` 只认用户挑的目录）。恢复副本的相对路径以
    `recoveryOf` 指向的工程为基准。
21. **循环是一个区间，不是一个标志**（P87）：`Model/LoopRegion`，不变量全是关于**顺序与最短**
    （0.25 拍）。**开关与数字分开**：关掉不许忘记位置。关闭时渲染退化成 `[0, length)`，
    老行为逐位不变。**`isUsable()` 是唯一判据**；`loopMove` 每次从 `loopDragOriginal` 重建。
22. **点击是监听，不是声音，也不是定时器**（P88）：`Audio/Metronome` 混进 `renderNextBlock` 的
    **每个 segment**，`renderInto`（导出/分轨/渲染成采样）里没有——"导出拿不到点击"是**结构性的**。
    **按拍与每块 `beatsPerSample` 驱动**，落在该在的那个采样上。分句 `ceil(fromBeat - 1e-9)`
    让块边界那一拍**只算一次**。音量是它自己的，**重音跟工程拍号**（→ 规则 24）。
    **预备拍不改 `positionBeats`**，数完在同一个块里交接。
23. **量化是形状的改动，不是位置的搬家**（P90）：`Model/Quantize.h` 的 `quantize::apply` 是唯一规则
    ——`round` 不是 `floor`、`>= 1.0` 早退即夹紧、0 与网格 0 都逐位退回。**只改起点**，音符与采样块
    都动；`moved == 0` 不重绘也不产生撤销步，判"动没动"用 `1e-9`。
    **编组要做两件事**：先把出现原点重锚定成该出现的最小成员，再**无条件** `syncGroupShape`
    （拖动整次出现不改形状，量化改）。出现原点是**存下来的数**，任何不走拖动路径的位移都要自己挪它。

24. **拍号是一个值，不是一个 `constexpr`**（P91）：`Model/Meter.h` 是唯一规则，`MainComponent`
    持有它，钢琴窗与节拍器是**被推**的视图（标尺 / 复制到后一小节 / 重音读同一份）。
    **`<Transport>` 永远写**（否则「4/4 无循环」与「3/4 无循环」是同一份文档），
    **读侧先无条件赋默认再查表**（老文件连元素都没有），`setBeatsPerBar` **不调 `modelChanged()`**。

25. **拖放的下一个动作由指针底下是什么决定**（P92）：从编组栏拖一行到网格上，空网格 = 再放一处
    （P58），压在**已经在那儿的组**上 = 替换那一处。`getGroupReplaceTargetAt` 是唯一判据，
    预览与松手都调它。**命中顺序照按下的顺序**：成员（音符→采样块）优先，框在后；
    **自己组的成员一律不算目标**——否则会先删掉马上要插回来的东西。**只认同图层**（跨图层
    要搬定义，另一个手势）。锚点是**目标的出现原点**，不是指针的拍。替换的是**那一处**，
    不是整个组；目标没有别的出现才由 `pruneEmptyGroups` 把整行送走。**不许用 A 替换 A**
    写在函数里，不靠调用方记得。

26. **一个度量只有一个来源**（P93）：`Audio/LoadMeter.h` 的 `perf::advance` 是唯一的负载/峰值规则，
    平滑系数 0.2 与 JUCE 相同（同屏两个表不能各走各的）。**丢帧不自己数**——
    `AudioDeviceManager::getXRunCount()` 已含设备原生计数与 JUCE 的估算，我们的回调在它的计时器
    里面，再数一遍就是重复计。**峰值不夹到 1.0**（那正是 JUCE 丢掉的那个数）。**预算来自
    `numSamples`**：0 采样的回调不算一块，没开设备（率 0）什么都不记。计时器是 **RAII scope guard**
    ——回调有四个出口，最便宜的两个正是手写一次调用会忘的。读数用**回调真正用的采样率**。
    顶栏的 `bar.beat.tick` 读 `meter::positionAt`，**不许再写字面量 4.0**。

27. **一个 take 有两半，就有两条链、两个偏移**（P94）：监听**默认关**，由录音键开、录音键关
    （不加询问就活的输入是啸叫）。补偿在 `commitTake` **门口一次做完**，之后每个读者拿到的
    位置都已经对了。`getRecordOffsetSamples` = 输出 + **输入** + 监听链；
    `getMidiRecordOffsetSamples` = 输出 + 监听链——**键盘没走过采集缓冲**，把输入项摊给它是
    让每个音符提前。两个数不是"一个数加个余量"，是两条不同的链，**用错一个就把同一手的
    两半拆开**。**三个项都不看监听开关**：`finishRecording()` 在 `commitTake` 之前就关了监听，
    按开关取值正好抹掉监听链那一项。换算**走秒不走拍分数**（速度会变）；零点由 `TempoMap` 兜，
    **不许在落点再加一个死的 `jmax`**（咬测证明它不可达）。

28. **一遍录音 = 一个容器，只有当前那一遍出声**（P95）：take 集合是**派生的**（成员各带
    `takeSetId`/`takeIndex`/`takeActive`），没有 set 实体。**入组只有一个判据**
    `Layer::joinTakeSet`，覆盖率按**长**的那一段量（0.9 起）——按短的量会让「8 小节里补 2 小节」
    拿到 100%，正好把要防的那件事放进来。补录因此自成一个普通组，不是并进去。
    **提升是隐式的**：普通组被第二遍撞上时才变成 take 1，所以单遍录音的行为与 P92 逐字节相同。
    **`setActiveTake` 是唯一写 active 的地方**，`joinTakeSet` 末尾必须调它——只把新 take 标成
    当前而不管旧的那遍，就是两遍同时出声。发声门在发布：`GroupMix::takeSilent` → `gainAt`
    一处，音符与块共用；**它不是 mute**，别把两个字段合成一个。存盘 **没有 `takeset` 就是
    普通组**、**没有 `takeactive` 就是当前**；加载后 `normaliseTakeSets`（序号最大的赢）。
    **`groups` 是 vector，录一遍就 realloc**：不许跨一次录音缓存 `NoteGroup*`，一律按 id 取。

29. **拼接是「把这段换成那一遍的材料」，不是一个分段表**（P96）：`Model/Comp.h`
    （`namespace comping`）是唯一规则。**不存 provenance 是设计**——记「哪一遍从哪到哪」要一份
    分段表活过存盘，而 take 集合是派生的、没有实体能挂它；写成替换之后，三处接缝是三次独立
    编辑，顺序无关、重复无害。**第一次拼接先分一份**：`NoteGroup::comp`（v31 `takecomp`，只在
    是 take 时写）标记「这就是拼接稿」，没有就从**当前那一遍**复制一份新的 take 并设为当前；
    第二处接缝改的是同一份，不再分。**接缝要的是重叠，不是前后相接**：进来那一段比接缝早
    `fadeBeats` 开始、比结尾晚 `fadeBeats` 结束，两侧邻居各在自己那一端淡到 0；少掉提前量就是
    「淡出 + 淡入」中间一段静音，那不是交叉淡化。**淡化是在原曲线上乘**（`comping::addEdgeFade`），
    而且**必须插肩点**——不插的话斜坡会从块边一路斜到曲线的下一个点（常常是块的另一头），
    4 ms 变成整块。**切块只走 `splitSampleBlocksRaw`**（`splitSamplesAt` 的算术半边）：源窗口 /
    变速 / 倒放的换算 P50 写过一遍，别写第二遍。**接缝的分辨率不在渲染里**——采样器自己在块
    边沿就有包络，把 fade 改成 0 渲染跳变只从 0.00040 变 0.00046；判据要看**块本身**（提前量
    与两端曲线值 0）。

## 索引
- 目录结构、构建命令（含 `unset *proxy*`）、CI 四条门禁、`formatVersion` 逐版字段、自检夹具
  —— 全在 `STATUS.md` 与 `REFERENCE.md`，本文件不重复。
- 只留两条不能忘的：**加断言的那一轮必须同步调大 `MIN_CHECKS`**；**"选中哪条自动化 lane"
  这类状态必须显式写属性**，不能靠元素顺序推断（先解析到临时对象、末尾一次性提交）。
