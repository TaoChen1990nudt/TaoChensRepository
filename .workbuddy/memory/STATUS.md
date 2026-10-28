# AtomicDAW — 当前状态索引

> 最后更新：2026-10-28 · 对应 **Phase 68**（缩小时网格自动变粗：1/16 细过 5px 就只画拍线，吸附同步跟上）
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
| 已完成阶段 | **Phase 68**（网格跟着缩放退档：`visibleGridStep()` 把步长从 1/16 逐档 ×4 抬到「拍 → 小节 → 4 小节」，直到相邻线至少 `ui::pf(5)` px；12 px/拍时 1/16 只有 3 px，直接消失。绘制与吸附**读同一个 step**：`collectGridLines()` 产出线表，`drawGridLines`/`drawGridInside` 遍历它，`snapBeat()` 也用它——看得见的线才吸得动。线条等级改按 **beat** 判（是不是小节/拍），不再按循环下标 `% 16`，否则步长变粗后小节线全丢）· **Phase 67**（标尺左键拖拽不再被窗口边框挡住：指针拖出网格左右边界时视口跟着滚，播放头钉在被推的那条边上——和 P66 的贴边跟随同一套几何，所以拉起播放头能一路拖到任意远处。`mouseDown` / `mouseDrag` 共用新的 `scrubRulerTo(screenX)`：**先扩标尺**（`raw > timelineBeats` → `ensureTimelineExtendsTo`）、再吸附、再滚、最后 locate；指针还在窗口内时一行都不滚。两条跟随分支加 `dragMode != rulerScrub`，按住期间窗口归拖拽所有）· **Phase 66**（①组的名字标签（`getGroupLabelBounds`）宽度再夹一次组框宽度：以前最小 34 px，一个 0.1 拍的组只有 5 px，标签会横出组框压到隔壁；标签窄到放不下曲线把手时 `getGroupAutomationHandleBounds()` 返回空、把手不画。②播放跟随从「翻页」改成「贴边滑」：触发线从窗口 3/4 挪到边界本身，落点从 15% 挪到边界——右边界外出就右移让播放头留在标尺尾部，左边界外出就左移让播放头留在标尺头部；回退到 0 点除外（那是「回家」，由 P58 的归还分支处理，`pastLeft` 要求 `beats > 1e-9`））· **Phase 65**（①多组选中后拖其中一个组的标签，`armGroupMove` 不再把选区缩成一个组——`isWholeOccurrenceSelected` 为真就保留选区，抓取也从「这一组的块」放宽成「所有选中的块」；②采样块的自动化框删掉启用开关，整行就是目标按钮，右键菜单的「启用/停用曲线」同步删除，`openSampleAutomation()` 负责把老工程里 `enabled=false` 的曲线恢复；③图层栏标题栏右端加眼睛按钮，一次切换「全部可见 / 仅当前可见」，状态由 `areAllLayersVisible()` 从模型派生）· **Phase 64**（播放头越过窗口 3/4 时钢琴窗闪屏：`setPlayheadBeat` 的「推走」与「归还」两条分支判据重叠——推走时记下的 `userScrollBeats` 窗口**就含当前播放头**，下一帧立刻「归还」把视图撤回，再下一帧又推走，60fps 来回横跳 85% 窗口。归还现在要求 `beats < previous`（回退才算回家：loop 跳回 / scrub / seek），前进永不归还。咬测：去掉这一条 → 189 次跟随里 87 次回退）· **Phase 63**（剪贴板里躺的不是「一个组」，而是**一组出现**：`groupClips` 是 vector，每个 `GroupClip` 自带 `notes`/`clips`。复制时选区按 `(groupId, occurrence)` 切片，整处选中的切片成一个对象、其余留散音符/散块；粘贴按对象逐个走「复用原组 + 新增一处」，全部用**同一个** delta（`clipboardFirstBeat()`/`clipboardLastBeat()` 算一个锚点），所以两组的间距原样保留。判据从 `isWholeGroupSelected`（问整个选区，多组时永远 false）换成新的 `isWholeOccurrenceSelected`（只问这一处）——`collectTwinsForEdit` 也一并换了）· **Phase 62**（①同源编组：出现只拥有**位置**（`NoteGroup::occurrenceStarts`，物化后不再漂），成员/音高/长度/力度/包络/采样块的宽高曲线反向伸缩全部属于组，编辑一处由 `syncGroupShape()` 同步到所有兄弟出现；整处拖动或删除是「对这一份的决定」，兄弟不动；②复制「一处出现」不再退化成散音符（旧判据要求选中全组，两处出现的组永远不满足），散音符粘贴显式 `groupId = -1`，块的组身份不再被扒掉）· **Phase 61**（①缩放把视口拉宽到超过标尺时，**这一档**就加长，不再等下一档——`clampScroll()` 里增长判定改用夹过下界的 `jmax(0.0, scrollBeats) + visible`，缩放的锚点常在 beat 0 左边、原始 scroll 是负数，把需求算小了一整块；②缩回去时同一条路径回收，长度与滚动条滑块宽度都回到原位）· **Phase 60**（①编组栏的行可以点选——点行 = 选中该组，折叠只留给左侧小三角；②选中框改用 `cardlook::drawSelectionRing`，与采样/VST 音源卡同一份代码、同一个 `theme::accent`；③`trimTimelineToView()`：往右滚进空白会加长（P11 原有），滚回来把多出来的尾巴还回去、滚动条恢复原长）· **Phase 59**（①`selectedTrimEdgeAt` 要求指针在块内 + 取最近的那条边，修掉"多选拉伸只有最右那块有效"；②钢琴窗选中组时编组栏对应行按组色描边高亮，由 `PianoRollComponent::getSelectedGroups()` 派生、`GroupPanel::setSelectedGroups()` 镜像；③折叠行不再把 dB/声相文字画在组名同一个矩形里）· **Phase 58**（组从"一条内容"变成"一个定义 + 多次出现"：`groupId + groupOccurrence`；粘贴组 = 新增一处出现而不是往组里塞成员；「复制到后一小节」同样是新增出现、不再新建组；编组栏的组行可以直接拖到钢琴窗放一处；每处独立移动；选中块的边缘优先于命中拾取；播放跟随回退到用户视野）；P57 的组框零外扩与 P56 的多选拉伸等见日志 |
| 自检 | **580 ALL PASSED / 0 FAIL，失败时退出码 1**（P68 咬测 3 处：阈值改 0（网格永不退档）→2 条挂（`0.25 / 0.25 拍，33 / 33 根线`、`2.25`）/ 吸附硬编码 1/16→2 条挂（`2.25 / 6.00`、`17 次落在空处`）/ 小节线判定失效→1 条挂（`0.075 / 0.075 / 0.030`）。P67 咬测 3 处：拖拽不滚→2 条挂（视口 `0.00 -> 0.00`，播放头跑到 80.5 在窗外）/ 先夹后扩→1 条挂（播放头 64.00、标尺 64.0）/ 去掉所有权闸→1 条挂（`0.00 -> 177.00`）。P66 咬测 4 处：标签不夹组宽→2 条挂（74.5 vs 5.0）/ 把手硬画→1 条挂 / 退回 3-4 处推走+15% 落点→2 条挂（离右边界最远 19.55 拍）/ 归零也左追→1 条挂（100.0→0.0）。P65 咬测 3 处：选区永远缩成一个组→2 条挂 / 眼睛只会关不会开→1 条挂 / 菜单行尾留 30px 开关位→几何断言挂 518.8 与 557.8。P64 咬测 1 处：去掉「回退才算回家」→ 189 次跟随里 87 次横跳。P63 咬测 3 处：整处判据恒 false→11 条挂 / 粘贴只贴第一个对象→4 条挂 / 每个对象各算锚点→间距那条挂 20.00 与 20.00） |
| 编译 | 零 error 零 warning |
| `formatVersion` | **25**（P66/P64/P63 只动运行时视图与剪贴板，不涉工程文件。v24 的 `groupocc` 不变；v25 给 `<Group>` 加 `<Occurrence index start>`，只在出现有起点时写，老文件靠懒物化补） |
| 可执行 | `build/AtomicDAW_artefacts/Release/AtomicDAW.exe` |
| 技术栈 | JUCE 8.0.4（FetchContent）+ VS2022 + MSVC 14.38 + Win SDK 10.0.22621 + C++17 |
| 源码规模 | `Source/` 下 70 个 .cpp/.h，分 Model / Audio / UI / Util / Media |

## 下一步

**未定。** 功能排期等你给。P58 把组的模型改完了（定义 + 出现），P59 把多选拉伸的拾取收进
块内、编组栏做成选区的镜像，P60 让编组行真正可点选（选中环与机架卡统一），P61 把"标尺多长"这件事
在缩放上也补齐了，P62 把组从"定义 + 多次出现"补成"一个形状 + 多个位置"（成员内容也同源了），
P63 把剪贴板从"一个组"补成"一组出现"（选中几个就贴回几个），P64 修掉播放跟随的横跳，P65 把多选拖动、采样框开关和图层栏眼睛补齐，P66 修了组标签越界与播放跟随的贴边规则，P67 让标尺拖拽自己带走视口（播放头拖到边外继续拖，画面跟着走），P68 让网格随缩放退档并把吸附绑在同一条线上，工程侧没有挂账。
- **网格只有一份步长**（P68）：`gridDivision` 是音乐上的最小单位（0.25 拍，也是音符最短长度），
  `visibleGridStep()` 是**这一屏画得出来**的那一档。绘制线和吸附都必须读后者；要用前者的是
  "最小长度 / 粘贴推进 / 下一小节"这类音乐问题。两个概念混用就会出现"吸到一根看不见的线"。
- **线条等级按 beat 判**（P68-A）：`gridLineAlpha(beat)`。`step % 16` 那种写法只在步长恒为
  1/16 时成立，步长一抬头小节线就全丢，画面变平但没有任何报错。
- **闭环断言不能当唯一判据**（P68-B）：`collectGridLines` 与 `snapBeat` 读同一个 step，
  所以"吸附结果落在线上"这条在两边一起错时仍然通过。真正咬得住的是绝对值（步长 1.0、
  9 根线、吸附到 2.0）。
- **标尺拖拽只有一个入口**（P67）：`scrubRulerTo(screenX)`。mouseDown 的点击、mouseDrag 的拖动
  都调它，任何人想加"标尺上还能干什么"都从这里进去。里面顺序固定：先扩标尺 → 再吸附 → 再滚 →
  最后 locate。别把 `jlimit(0, timelineBeats, ...)` 提到扩展前面（P67-A）。
- **按住拖拽期间，窗口归拖拽所有**（P67-B）：`setPlayheadBeat` 的贴边跟随与归还两分支都要求
  `dragMode != DragMode::rulerScrub`。transport 不会因为 locate 而停，所以拖拽期间它每帧还在
  推进播放头——不挡就是两个写者抢 `scrollBeats`。
- **剪贴板的形状**（P63 定下的语义）：`groupClips` 每个元素 = 一处完整出现，自带成员；
  `noteClip`/`sampleClip` 只装**不属于任何整处**的散音符/散块。别再往这两条里塞整处成员——
  那份数据是粘贴时"一组内容被写进两个组"的来源。
- **跟随有两种写法，别混**（P66-A）：翻页式（触发 3/4、落点 15%）与贴边式（触发边界、落点边界）。
  贴边式每帧都滚一小段（60fps 连续滑），所以「每帧整窗重绘」是它的正常成本；判据是
  「跟随开始后播放头离边界始终 < 一帧的步进」，不是「跳几次」。
- **"这一处整不整"只有一个判据**（P65-A）：`isWholeOccurrenceSelected(gid, occ)`。凡是
  "点一下其中一个，其它的要不要跟着走"的地方都用它——拖动抓取（`armGroupMove` / `simulateGroupMove`）、
  复制切片、连带删除。用 `isWholeGroupSelected` 的地方都是"问整个选区"，多选下必假。
- **删掉一个控件不算完，要断言剩下的东西没留空**（P65-B）：采样的开关删了，行尾仍可能空 26px，
  编译不会报错。用 `getSampleTargetAreaForTest() == getSampleMenuAreaForTest()` 这种几何恒等式钉住。
- **播放跟随的「推走」和「归还」是一对互斥的条件，改一个必看另一个**（P64-A）：
  推走 = 播放头越过窗口 75%；归还 = 播放头回到 `userScrollBeats` 那个窗口内。
  推走时播放头**就在**那个窗口内（那是它被抓拍的地方），所以归还必须再加「回退」
  （`beats < previous`）才不会每帧来回。
- **"整处选中"有两个判据，别用错**：`isWholeGroupSelected(gid, includeSamples, occ)` 问的是
  **整个选区**（"这个组是不是选了全部"），多组一起选时永远 false；`isWholeOccurrenceSelected(gid, occ)`
  只问**这一处**。凡是要按对象处理的地方（复制、连带删除、同步）用后者。
- **同源的三条已知边界**（P62 定下的语义，不是 bug）：
  1. 整处拖动 / 整处删除 = 对这一份的决定，兄弟不动；动一部分才同步。
  2. 形状同步是"以被编辑的那一处为准，其余多退少补"。老工程里若同一个组的两处
     已经不一样（P62 之前做的），第一次编辑会把它们收敛到被编辑的那一处——包括删掉
     多出来的成员。这是"同源"的定义，不是意外。
  3. **剪切一整处再粘贴**会让组定义先被 `pruneEmptyGroups()` 扫掉、粘贴时新建一个同名的
     组（id 变了，绑在这个 id 上的自动化轨道会脱钩）。用户目前没提，要修就得让剪贴板
     押住组定义，或允许 `addGroupCopy` 复用原 id。
- 编组栏的高亮只是镜像，**不做滚动跟随**：选中的那个组若在列表视口外，行不会自动滚进来。
  要加就先想清楚"什么时候允许抢用户自己的滚动位置"。
- 排版层只落了「走带读数 / 混音台 dB / BPM 数值 / 面板标题与大写副标题」四处；
  `Metrics.h` 里的 `tracked()`、`title()`、`section()`、`drawTabular()` 是可复用的，
  其它面板要用就换调用，不要另写一套字号。
- 组的"出现"目前只有「粘贴 / 复制到后一小节 / 从编组栏拖进来」三种来源，没有"在钢琴窗里
  直接把某处再放一份"的手势（想加就先想清楚落点怎么定）。

## 已知边界（用户提过、还没做）

- **停止只记住"最后一次播放从哪开始"**：播放中途拖标尺换位置，停止仍回到本次播放的起点，不是新的落点。
- **刻刀一次只切点中的那一个**：不切整个选区（旧菜单行为是切选区，工具语义是切点中的对象）。
- **MP3 只在 Windows 上可用**：走 Media Foundation（系统自带）；其它平台 `mp3::isAvailable()==false`，
  导出框里 MP3 chip 灰掉。N 版 Windows 缺媒体功能包时同样灰掉，错误信息会带 HRESULT。
- **变调范围默认 ±24 半音**：老工程（v22 及更早）读进来就是 24，ratio 超过 4 的极端块会开始被夹住——
  想要旧的无限制行为就把 chip 拖到 36。
- **反转是播放期镜像**：`reversed` 只改 read 位置，源文件不变；没有「反转后重新对齐零交叉」这种精修。
- **渲染没有进度条与取消**：`ScopeRenderTask` 跑在后台线程，完成就插入；
  长选区的几秒钟里 UI 不给提示，也没法中止。
- **音符粘贴没有 streak**：P37 只修了采样。音符连按 Ctrl+V 仍落同一处（用户还没要求）。
- **VST 乐器没有双击打开界面**：只有右键菜单第 41 项，`RackPanel` 还没有 `mouseDoubleClick`。
- **粘贴/复制到后一小节都是"新增出现"**：同组的两处内容**各自独立移动**（拖哪一处只动那一处），
  因为它们是同一个定义的两个 `(groupId, groupOccurrence)`。要"整组一起动"就框选两处。
- **出现共享一条曲线**：组 lane 是绝对时间，第二处出现**不会**再画一段曲线，也**不会**按 delta 平移
  ——"同一个组"就是这个意思。要两段独立曲线就是两个组。
- **剪切→粘贴会重建组**：剪切会把榨干的组清掉，粘贴时源 id 找不到，于是新建一个同名同色的组
  ——看着一样，但它是新对象（id 变了，自动化窗口的旧条目已经先被摘掉）。
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
| P48 | 视觉规范落地：色值预算 + 静音/独奏统一 + 降饱和 + 圆角 + 色值门禁 | `memory/2026-10-27.md` |
| P49 | 采样块的音量曲线：向下展开箱 / 箱宽=块宽 / 曲线点手势 / 淡入淡出删除 / v21 迁移 | `memory/2026-10-27.md` |
| P50 | 声相曲线与中线 / 播放头切割 / 反转 / 音符·编组·整层渲染成采样并落位 / 等宽数字与字距层级 / v22 | `memory/2026-10-27.md` |
| P51 | 停止回播放起点 / 尾部无上限 / 机架卡拉伸控件 / 刻刀切采样+切音符 / chevron 上移 | `memory/2026-10-27.md` |
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

断言总数只增不减。**新增功能要加断言并报出新的总数**（当前基准 580），退回去说明有东西坏了。

其他 dev 参数：`--cursor-preview <png>`（光标 8x 放大核对）、`--pluginscan`、`--audiotest`、`--transporttest`、`--load` / `--save` / `--export`。

## 未建成的东西

- `.scratch/<feature-slug>/` — `AGENTS.md` 里约定的 issue 存放处，**目录还不存在**
- `GLOSSARY.md`、`docs/adr/` — 同上，声明了但未创建（按约定 lazy 创建）
