# AtomicDAW — 项目长期约定

JUCE 8 音频工作站：图层叠加工作区 / 音源机架 / 音符级微观包络。分阶段交付，每阶段只做一件事，
必须能编译并跑通视觉或数据逻辑。

**当前阶段、自检基准、构建命令看 `STATUS.md`；为什么这么做在 `REFERENCE.md`；陷阱与咬测在 `PITFALLS.md`。**
本文件只写**不能违反的那一句 + 去哪找它**——理由与咬测流水一律在 `PITFALLS.md` 对应阶段段里。

## 硬规则

1. 中文一律 `atomic::utf8("...")`（含自检失败信息）；裸中文 = 乱码。
2. 几何 `ui::px()/pf()`、字号 `ui::sp()`，唯一旋钮 `ui::scale`；不缩放：`numPitches` / gutter / 内联自动化框最小宽。
3. 路由只有 `Model/Routing.h`（`resolve` / `resolveSample` 不看组 / `layerSlotId` 兜底跳 sampler）；UI 与引擎问同一份。
4. 音频线程不读 Layer / NoteEvent / RackModel：消息线程 `publishSchedule()` 出不可变 `PublishedSchedule`，锁只盖 shared_ptr 赋值。
5. 改模型必发 `layersChanged()` / `notesChanged()`；撤销 = 整工程快照、粒度一次手势（`beginUndoGesture`/`endUndoGesture`）。
6. 唯一入口：`buildSession()` 会话重置、`removeSlotAndItsClips()` 删槽、`auditionPreview()` 试听。
7. 组的形状只按「起点 + 名字」配对（`clipShapeLess`）；改了宽/窗/曲线/`laneSpan` 一律 `syncShapeOfClip()` / `syncGroupShape()`。
8. 编组默认是对象不是容器（P73）：状态只有 `editingGroupId`，`layersChanged()` 里无条件清。
9. 按下的话事权归「你以为自己在按的东西」（P75）：`clipEdgeAt` 答 `(id, none)`；组框里的空白不是把手。
10. 选中一个成员 = 选中它所在的编组（P74）；扩散只在手势层，不写进 `setMixedSelection`。
11. 打开不是选择（P76）：判据是「谁会被缩放」（`dragResizeNoteId`）。
12. 双击 = 打开，顺序照绘制顺序（P78）；机架卡上名字是改名、其余是打开。
13. 名字是一份数据不是副本（P79）：入口都是双击那行名字（`NameEditor`），改名按 id。
14. 速度一旦会变，时间就是积分（P80）：唯一换算 `Model/TempoMap`；`renderNextBlock` 每段重算 `beatsPerSample`。
15. 录制的拍号在事件发生处取（P81）：beat 随事件过桥；`finish()` 先读 `writePos` 再 disarm。
16. 一条曲线 = 一个形状 + 一个去处（P82）：`Model/CurveBank` 目标至多归一、列表永不为空、用 id 不用下标；const 读永不创建。
17. 两条路径汇到一处，慢的那条不能被拉快（P83）：`pdc::solve` 是唯一延迟表，补偿只在汇合点；没有延迟就全 0，别加兜底。
18. 恢复副本是「一份副本」（P84）：写它绝不改 `currentProject`；走 `File::replaceWithText`；默认关。
19. 一个类可以住多个 TU，共享辅助必须 inline（P85，`UI/PianoRollInternals.h`）；判据是「几个 TU 用」。
20. 模型只存绝对路径（P86）：`Model/SamplePath` 是唯一路径层；重定位不许自动发生。
21. 循环是一个区间不是标志（P87）：`Model/LoopRegion`；开关与数字分开；`isUsable()` 是唯一判据。
22. 点击是监听（P88）：`Audio/Metronome` 混进 `renderNextBlock` 每段，`renderInto` 里没有；预备拍不改 `positionBeats`。
23. 量化是形状的改动不是搬家（P90）：`quantize::apply` 唯一；`moved == 0` 不重绘不撤销；编组先重锚定再 `syncGroupShape`。
24. 拍号是一个值（P91）：`Model/Meter.h` 唯一；`<Transport>` 永远写；读侧先赋默认再查表。
25. 拖放的下一个动作由指针底下是什么决定（P92）：`getGroupReplaceTargetAt` 唯一判据（预览与松手都调）。
26. 一个度量只有一个来源（P93）：`perf::advance` 唯一；丢帧不自己数；峰值不夹 1.0；顶栏读 `meter::positionAt`。
27. 一个 take 两半 ⟹ 两条链两个偏移（P94）：补偿在 `commitTake` 门口一次做完；三项都不看监听开关；换算走秒。
28. 一遍录音 = 一个容器，只有当前那遍出声（P95）：入组唯一判据 `Layer::joinTakeSet`；`setActiveTake` 唯一写 active；发声门在 `gainAt`，它不是 mute。
29. 拼接是「把这段换成那一遍的材料」（P96）：`Model/Comp.h` 唯一；第一次拼接先分一份；接缝要重叠不是相接。
30. 一根柱子只有一个矩形（P97）：`velocityBarFor` 是唯一几何；lane 空白是框选区；夹具要把改过的状态还回去。
31. 说「已有模型」前先 grep 那个符号（P98）：「我上次写的」不是证据；能复用就不新建。
32. 一条信号两条路 ⟹ 拓扑与延迟同一答案（P99/P100）：`pdc::solve` 的边必须带延迟；`noKeySource` 是 -1；模型查环比发布严；send 不许指主输出。
33. 断言的第一步是让被测对象真的被改变（P101/102）：前提写成断言；参照量在「被测对象不存在」那张图里；空日志 + 秒退 127 = 帧太大。
34. 一个读数要三个动作配套（P103）：发布取大、按速率落、读完消费；换算只有 `fx::gainReductionDb`。
35. 读数会不会自己消失看它记状态还是事件（P104）：削波闩锁，`resetMixState` 不碰它；满刻度不是削波（`>`）。
36. 几何算「多大」，绘制算「画哪些像素」（P105）：`getGroupContentBounds` 不看视口；「看不见不画」落在绘制循环里。
37. 不要往别人的窗口里塞宿主的东西（P107）：只为一件事存在的中间层，那件事没了就一起删；撤回连断言/帧/文档一起撤。
38. 默认值不是一种形状，是「还没人改过」（P108）：`NoteEnvelope::defaultFlatValue = 0.8f` 唯一；中性点不是全局常量；拷帧按清单不按 glob。
39. 换一种画法，不是加一层东西（P109）：总览网格 x 与高度都不动；带号==图层号不过滤；只读判据是一行；「按了没反应」证明不了只读。
40. 保留一维先定口径（P110）：行 = 这层用到的不同音高等分；行表按图层不按视口；`rows == nullptr` = 折叠 = `count == 1`，不是第二条路径。
41. 「每层各自」的开关就该每层一个（P111）：`foldedOverviewLayers` 存图层 id 不存带号；绘制/命中/光标问同一个函数。
42. 「它没有 X」也要先 grep 结构体（P112）：块盖它的行不坐在行上；断言「重叠」必须同时断言「错开」。
43. 一条带多高取决于别人折没折（P113）：带宽是整摞派生量；折一层让下面全部上移，缓存的命中坐标当场失效。
44. 「一条线的宽度」要从被描述的东西推出来（P114）：`thin = min(7 设计像素, 等分)`；开关不许消失但可以换形状。
45. 只在部分情况下生效的折叠不叫折叠（P115）：折着的带永远 `thin`，从顶边摞、下面空着；跨轮比帧要编同期对照。
46. 「重叠」是区间运算（P116）：折叠带取并集（相接算，`<=` 不是 `<`），一段一条线一个头；等价变异不是咬测。
47. 一个手势动三个东西，顺序就是规则（P117）：`openOverviewBand` = 先在带上读拍与音高 → `setActiveLayerIndex`（会通知会夹滚动，故在摆视口前）→ 退总览 → `centreViewOnBeat`（自己夹到 `timelineBeats - visible`，`setScrollBeats` 会把标尺变长）+ `revealPitch`（只揭示不居中）→ `onLocateRequested`（视图不写 `playheadBeat`）。空白不是带，折叠带不猜音高。

48. 「录什么」是一个值（P118）：`takeArmingFor (TakeMode)` 一次答 capture/events/monitor 三问；门在**采集**入口（`recordNoteAt`），不在落地入口（`commitTake` 只答「手上有什么」）；停从不提问；取消不回落；那个 30×28 的按钮自己画答案——状态条那一行已经没地方了。

49. 累加出来的 double 不能和终点比（P119）：数拍结束的判据是 `segment < numSamples`，不是 `beat >= endBeat - 1e-9`（差值 4×10⁻⁵ 拍，永远假）；夹具的块大小是坐标不是背景；`countInApplies (wasRolling, …)` 的 `wasRolling` 必须在 `playTransport()` 之前读；数拍期间 `recordNoteAt`/`recordControlAt` 一律 return。

50. 显示不是提交（P120）：录中的音符是一份**快照**（`publishLiveTakeNotes` → `setLiveNotes`），不进图层/撤销/工程文件，只在键按下、键抬起、take 落地三个时刻重发，不从 timer 发；按住的音符没有长度，尾巴就是播放头；预览与落地共用 `getNoteBounds` 三参版——「画在哪」和「落在哪」必须是同一份几何；「画出来没」要用像素读回快照来咬（自检 P120 第 8 条），列表在手不等于上了屏。
51. 记住的是意图不是成功（P121）：`MidiPreferences` 存两个开关/八度/勾选过的端口（`identifier` 是身份，`name` 只是 id 变了之后的退路），失败与关控制器都不删，只有主动取消勾选才删；只有窗口那一个 `MainComponent` 拿真实路径，headless 拿空路径；恢复排在回调接好之后且自己不写回；`openRememberedDevices` 按值收参数——它会重建成员，按引用就是在自己的 `clear()` 上读空。
52. 外框和插件界面只认一套尺寸（P122）：窗口的 constrainer 必须转交给编辑器自己的那一个（`EditorConstrainer` 扣掉原生边框），不能在窗口上另写 `setResizeLimits`——那是第二套尺寸，插件的 `checkSizeConstraint` 一拒绝就露出黑色底板；`getDesktopScaleFactor()` 钉 1.0，原生视图自己管 content scale；`setContentOwned(editor, true)` 保住「编辑器自己改、外框跟着走」那一路。

## 索引
- 目录结构、构建命令（含 `unset *proxy*`）、CI 门禁、`formatVersion` 逐版字段、快照夹具 —— `STATUS.md` / `REFERENCE.md`。
- 两条不能忘：**加断言的那一轮必须同步调大 `MIN_CHECKS`**；**「选中哪条 lane」这类状态必须显式写属性**。
