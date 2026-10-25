# AtomicDAW — 项目长期约定

JUCE 8 音频工作站。差异点：图层叠加工作区 / 音源机架 / 音符级微观包络。
分阶段交付，每阶段只做一件事，必须能编译并跑通视觉或数据逻辑。

## 构建
- VS2022 + MSVC 14.38 + Win SDK 10.0.22621；PATH 无 cmake，用 VS 自带的
  `.../Common7/IDE/CommonExtensions/Microsoft/CMake/CMake/bin/cmake.exe`；JUCE 8.0.4 走 FetchContent。
- **构建前 `unset https_proxy http_proxy all_proxy no_proxy`**：本机同时有大小写两份代理变量，
  MSBuild 的 Hashtable 抛 MSB6001，报的却是 "No CMAKE_CXX_COMPILER could be found"。
- **中文必须 `atomic::utf8("...")`**，CMake 给 MSVC 加 `/utf-8`。`juce::String(const char*)` 走
  CharPointer_ASCII，非 ASCII 字面量直接乱码。
- **验证编译别用管道 grep**：MSBuild 编码会让 grep 判成二进制、编译失败也看不见。一律
  `> /tmp/buildNN.log 2>&1` 再 `grep -a -E "error C|warning C"`，并核对 exe 的 mtime。

## 贯穿全局的数据规则
- **挂在音符上的东西都是值成员**（`NoteEnvelope` / `outputSlotOverride` / `groupId` / `modTarget`），
  不做外部 map：复制、拖动时物理上不可能脱钩。
- 音符包络是**归一化 0..1**（跟随音符长度），宏曲线是**绝对拍**（钉在时间线）。两者不许统一。
- UI 层寻址一律 `(layerIndex, noteId)` 或**稳定 id**，禁止缓存 `NoteEvent*` / `Layer*`（vector 重分配）。
- 图层共用一套 content 空间（128 pitch × N beat），`scrollY`/`scrollBeats` 映射到屏幕。
- 只有激活图层的音符可被命中：`hitTestActiveLayer` 在函数层面不返回 ghost。

## 路由（唯一实现：`Model/Routing.h` 的 `routing::resolve()`）
优先级 **note.outputSlotOverride > group.outputSlotId > layer.outputSlotId > 机架第一个槽**。
`Resolution` 用 `viaGroup`/`danglingGroup`/`overridden` 报告命中路径。UI 高亮、聚光灯、音频引擎
全部调它——任何一处另写三段判断，显示和听到的就会变成两套话。指向已删除音源的 override 回落到
图层默认并标 dangling，不静默丢弃。

## 音频
- 音频线程**绝不**读 Layer / NoteEvent / RackModel。消息线程 `publishSchedule()` 构建不可变
  `PublishedSchedule`（包络、曲线、增益、audible 全快照），锁只覆盖 shared_ptr 赋值。
- 声部无状态：相位 = freq × (音符内秒数)。离线渲染与实时播放同一条路径。
- 链路：**声部 → 每通道立体声总线（通道插入）→ 主插入 → 主推子**。图层推子/组增益在声部阶段
  （乘法），音源推子+滤波在总线阶段；通道由**音源**喂（`busIndexFor(mixerChannel)`，-1/越界 → 直通总线）。
  未被使用的总线整段跳过（不跑插入、不推延迟线）。
- 自动化算术只写在 `LayerMix::gainAt()/panAt()` 一类函数里：音量**乘**推子、声相**加**推子。
  solo/mute 在发布时解算成 `audible`，音频线程不认识 solo。
- 复用缓冲**必须无条件 clear**（`avoidReallocating` 不清零）。
- 模块只引 `juce_audio_devices`（含 audio_basics），不引 audio_utils/formats；WAV 头手写 44 字节。
- 模型层不引 juce_audio_basics；dB 换算走 `Model/Gain.h`。

## 工程文件（`.atomic` = 带 XML 头的 UTF-8，`formatVersion` 现为 8）
- **先解析到临时对象、末尾一次性提交**，坏文件绝不破坏当前会话。
- 载入后重置 `nextNoteId` / `nextSlotId` / 组 id 计数器（max+1），否则新对象顶掉旧 route。
- 新字段缺失按默认值，老文件必须仍能开：v3 `mod=`、v4 插件（`createXml` 标签 **"PLUGIN"** 全大写）、
  v5 `mixer="-1..7"`、v6 `<Groups>`（写在 `<Notes>` **之前**）、v7 `<Samples>`（读回重新解码，
  缺失就留矩形 + 淡色，不报错）、v8 `<Mixer channels>` + `<MasterAutomation>` + `owner="master"`。
- 导出：消息线程发布快照 → 后台按块渲染，长度用 `contentBeats`，不归一化（峰值 >0.999 才拉回）。
  验证手法：直接导出与「存→读→导出」的 WAV MD5 相同。

## 会话与撤销
- **会话重置唯一入口 `buildSession(withEmptyLayer)`**：清 automation/mixer → restoreLayers →
  restoreSlots → BPM 120 → 清 currentProject → reconcilePlugins。空白 = 1 空图层 + 1 原装音源
  （Phase 18 起该音源默认 `mixerChannel = 0`）。`loadDemoProject()` = `buildSession(false)` + demo 内容，
  **不要**让它复用 buildBlankProject()（会多出一层空图层）。
- `RackModel` 构造预置 Synth A/B 是自检 fixture 的默认机架；空机架是**会话**属性，别改模型构造函数。
- 脏标记 = `savedStateText` + `dirtyCache`（在 commitHistory / applyHistoryPosition 刷新），
  没有独立 dirty bool。关窗/新建/载入统一走 `confirmDiscardChanges(action, onProceed)`；
  `handleCloseRequest()` 返回 false = 对话框接管退出，MainWindow 不要再 quit。
- 撤销 = 整工程 XML 快照 + AsyncUpdater 合并同一轮次；`restoringHistory` 期间所有 Listener 直接返回。
  自检里 `restoreState()` 之后必须同步 history/historyPos。
- 混音台在 `MixerWindow`（DocumentWindow），面板归 MainComponent 所有：关窗不销毁状态。

## 自动化
- 每个 owner 常驻 `std::array<AutomationLane,2>` + `automationFor(target)`，不新增按 target 分散的 map。
  `AutomationModel` 只存 `{id, owner, ownerId, target}`，**从不存 `AutomationLane*`**，绘制/命中去 model 现查。
- 建轨只有一条路：`MainComponent::ensureAutomationTrack(owner, ownerId, target)`
  （lane 开 → setTarget → 展开 → addTrack → revealTrack → republish）。四处右键菜单全调它。
- 面板上没有「自动化」徽章（Phase 14）。建轨入口 = 右键 `创建自动化 · 参数`（勾选 = 已存在），
  第三项"展开/收起曲线"是开关内联曲线的唯一入口，别删。
- 行高是 UI 偏好、**按 trackId 存**；行几何走 `rowTopAt/getTrackHeight/getContentHeight`，
  `index * rowHeight` 一律视为 bug。行底 6px 是 resize 带，命中排在 list/canvas 之前。
- 混音台通道的 owner 是**音源**（`slotIdOnChannel`）；主输出（Phase 18）owner = `master`、ownerId = 0，
  两条 lane 存在 `MixerModel::automationFor(0/1)`，引擎在 master 阶段用 `applyGainRamp` 按块斜坡。
- 通道没有音源时右键**标灰菜单项**，不弹对话框（Phase 18）。
- 时间线长度变化经 `PianoRollComponent::onTimelineChanged` 灌给自动化窗。

## UI 制度
- 唯一密度旋钮 `ui::scale = 1.3f`（`Util/Metrics.h`）。几何写 `ui::px()/pf()`，字号写 `ui::sp()`；
  代码里保留设计值。**禁止裸像素数字**（bounds、reduced、圆角、描边全包 px/pf）。
- **不缩放**：`numPitches`、内联自动化框最小宽度、时间线 gutter 宽度。
- 时间线左侧只有一个宽度 `ui::gutterWidth`：钢琴键 `keyboardWidth` 与自动化窗 `listWidth` 都指向它。
  两者不等 → 画布 beat 0 不在网格 beat 0 上（曾经差 86 px）。
- 菜单/对话框字体走 `Util/UiLookAndFeel.h` + `setDefaultLookAndFeel`（popup 归 desktop，
  组件级 setLookAndFeel 继承不到），`shutdown()` 里置 nullptr。
- 自绘图标由控件尺寸派生，禁止固定 inset（放大后会把图标压成薄片）。
- `g.drawText(t, {x,y,w,h}, ...)` 会 C2668 歧义，必须写 `juce::Rectangle<int>(...)`。
  量中文宽度用 `GlyphArrangement::addLineOfText` + `getBoundingBox`（`getStringWidthFloat` 已弃用且算不出 CJK）。
  中文标签塞不下**先让邻控件让位**，别压字号（下限约 9.5sp）。
- `Rectangle::withHeight()` 保持中心，`withY()` 才定顶边。`▾` 一律 `mixerlook::drawCaret()`：
  `open=true` → ▼，`false` → ▶（收起行必须看起来能再展开）。
- 窗口最大化 = `setBounds(display->userArea)` **减掉 `peer->getFrameSize()`**：JUCE 的 setBounds 收的是
  内容尺寸、边框加在外面，直接传 userArea 会把标题栏顶到屏幕外（Phase 18 修的）。
  `DocumentWindow` 没有 `setMaximised`。

## 各阶段遗留要点
- **VST3 只能在子进程扫**（本进程 dlopen 会 SIGSEGV）：`self --plugin-scan-child`，缓存
  `userAppData/AtomicDAW/plugin-library.txt`（首行 search-path 签名）。插件一律 **int id** 寻址，
  音频线程 try-lock、消息线程阻塞锁，离线导出 `createOfflineClone()`。卸载插件前必须同步销毁窗口。
- 音高调制必须用相位积分表（发布时制表），直接乘进 `f*elapsed` 会有无界相位误差。
- 采样：`SampleEvent` 只存 `laneSpan`（整数行）+ `lengthBeats`，高度 = laneSpan×keyHeight、
  宽度 = lengthBeats×beatWidth，所以"占整数行/随缩放"是构造保证。WAV 解码手写在
  `Media/SampleLibrary.cpp`（PCM 8/16/24/32 + float32/64 + EXTENSIBLE），别引 juce_audio_formats。
  采样不进合成器滤波器（一阶滤波有状态会污染声部），声相用 balance 不是 constant-power。
- `beatWidth` 是**成员**（默认 48，12..180，Ctrl+滚轮 / 拖标尺），`setBeatWidth()` 以视图中心为锚
  并 fire `onScrollChanged()`。缩放只改 beatWidth，不许改 keyHeight。
- 折叠状态一律存模型（`Layer/SynthSlot/NoteGroup::collapsed`），行高是 `f(model)`，命中走 `getRowBounds()`。
  四个列表栏各自持有像素 `scrollY`，接口统一。拖拽：MainComponent 是 DragAndDropContainer，
  文件栏只 `startDragging(var(path))`（用 `ScaledImage` 重载），钢琴窗是 DragAndDropTarget。
- demo 夹具自己写 `userAppData/AtomicDAW/Samples/demo-clip.wav`（解码器自写，素材只能自产）；
  采样块放 beat 12 / pitch 81 才在视野内。
- `--snapshot` 渲染 1872x1066（=px(1440)xpx(820)），混音台 1534x390；**不覆盖已存在的 PNG**，
  重出图前先 `rm -f`。`runSelfTest()` 结尾必须 `loadDemoProject()` 复原 fixture。
- 自检里写死的像素断言是**真实像素**，与 scale 无关；改 scale 后数字变了说明有常量没走 px()。
- **试听声部（Phase 19）**：`PlaybackEngine::auditionNote()/renderAudition()`，只由 AudioEngine
  回调在 masterFade **之后**调用——导出/离线渲染永远不发声。`retrigger=false` 是拖拽滑音
  （保相位、`played` 夹在 hold 内）；波形/电平/声相从 `SlotMix.waveform`（冻结值）取。
  设备回调的 early-return 必须问 `hasAudition()`；它 = `engaged || serial != consumed`，
  只比 engaged 会在"音频线程收尾清标志"与"消息线程刚投递"重叠时吞掉一次请求。
- **音符两端都能拉伸**：`kResizeHandleWidth` 是设计值，处处 `ui::pf()`；尾部自动化按钮
  `inset = band+gap+11`、`minWidth = inset+band+6`，短于此不画（否则按钮吃掉右侧拉伸带）。
- **标尺**：左键 = `rulerScrub`（连续 `onLocateRequested`，走 `setPositionBeats` 不停机），
  右键 = `rulerZoom`。两个模式都要在 mouseUp 里 `updateCursor()` 收尾。
- **JUCE 8 往 `juce::Image` 画图**：必须先把 `juce::Graphics` 析构再读像素/写文件，否则拿到全透明图
  （延迟提交）。手绘光标走 `PianoRollComponent::renderToolCursorImage()`，`--cursor-preview` 出 8 倍图。
