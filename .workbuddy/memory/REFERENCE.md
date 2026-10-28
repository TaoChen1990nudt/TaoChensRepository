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

## 工程文件（`.atomic`，`formatVersion` = 24）
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
  **新 lane 一律追加在枚举末尾**，老文件不需要映射表。
- **选中哪条 lane 显式写 `autotarget`，不靠元素顺序推断**：三条 lane 都写出来之后，
  "最后读到的那条算选中"恒把 target 变成 `plugin`。回退规则只在 `autotarget` **缺失**时启用。
- **组静音存在组上，不存成员上**：一个属性熬过粘贴、复制、合并；写到六十个音符上就是六十个
  可能出错的地方。
- **`out` 的 master 哨兵随上限走**：v11 及更早 = 8，v12 起 = 127。读旧文件必须翻成 master。
- 提交顺序：**先 `setChannelCount` 再 `setChannelOutput`**（后者拒绝非活通道）。
- 导出：发布快照 → 后台按块渲染，长度用 `contentBeats`，不归一化（峰值 >0.999 才拉回）。

## 会话与撤销
- **会话重置唯一入口 `buildSession(withEmptyLayer)`**：清 automation/mixer → restoreLayers →
  restoreSlots → BPM 120 → 清 currentProject → reconcilePlugins。空白 = 1 空图层 + 1 原装音源。
  `loadDemoProject()` = `buildSession(false)` + demo，**不要**复用 `buildBlankProject()`。
- 脏标记 = `savedStateText` + `dirtyCache`；关窗/新建/载入统一走 `confirmDiscardChanges`，
  `handleCloseRequest()` 返回 false = 对话框接管退出。
- 撤销 = 整工程 XML 快照 + AsyncUpdater 合并同一轮次；`restoringHistory` 期间 Listener 返回。
- 混音台在 `MixerWindow`，面板归 MainComponent：关窗不销毁状态。

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
- **内联曲线框**：框的左右 = **组的起止拍**，`getGroupCurveBounds()` 水平零内边距；X 轴是组自身
  跨度、Y 轴 0..1。打开即 `setEnabled(true)`；**与音符内联框互斥**。
- **组要能拖起来**：唯一入口 `armGroupMove(groupId, pos)`。两条路：点标签 `getGroupLabelAt`、
  点框内空地 `getGroupFrameAt`（只认激活图层，排在音符命中之后、marquee 之前）。
  **P65：这一处若已经整处在选区里，就不调 `selectGroup()`** —— 多选后点其中一个组的标签，
  意思是"搬走我选的东西"，把选区缩成这一个组会让其余组在指针动起来的瞬间掉队。
  抓取也从"这一组的块"放宽到"所有选中的块"（`captureGroupClips()` 现在就是
  `captureSampleDragOriginals()`），所以两组各自的采样块一起平移。
  整处判据是 `isWholeOccurrenceSelected(gid, occ)`，不是 `isWholeGroupSelected`（后者问整个选区，
  多选时永远 false）。
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
  - **形状同步** `Layer::syncGroupShape(gid, source)`：按成员序（start, pitch, id）配对，
    把源出现的相对位置 + 全部属性写进每个兄弟出现，锚点是兄弟自己的起点。多退少补。
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
- **卡片右键的「创建自动化 ▸」**（P42）列出该槽三样东西：`滤波器` / `音量` / `音源参数 ▸`；
  「音源参数」展开后是该插件自己的参数表（下标直通 `pluginParam`），内置振荡器没有这一段。
- 替换 = `replaceWithPlugin` / `replaceWithNative`，**槽 id 不变**，路由 / override / 自动化全保留。
  两者都返回被换下来的旧实例 id，调用方接 `reconcilePlugins()` 收尸。
- 替换后不用手动 republish：`AudioEngine` 自己是 rack 的 listener，`rackChanged()` → `republish()`。
  这条不成立时"换音源没声音变化"就会回来。

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
- **播放跟随视图（P58 / P64）**：`setPlayheadBeat()` 里两条互斥分支。
  **推走** = 播放头越过窗口 75%（`scrollBeats + visible * 0.75`），记下当时的 `userScrollBeats`，
  视图前移让播放头落在 15% 处（一次挪 60% 窗口）；**归还** = 播放头回到 `userScrollBeats` 那个
  窗口内，视图退回原处。
  **归还必须再要求"播放头在往回走"**（`beats < previous`）。推走时抓拍的那个窗口本来就含着
  当前播放头，少这一条就是：本帧推走 → 下帧归还 → 再下帧推走，60fps 横跳 60% 窗口，
  表现为钢琴窗疯狂闪屏（P64）。前进永不归还；loop 跳回 / scrub / seek 都算回退，照常归还。

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
- **块是窗口不是拷贝**：`sourceOffsetSeconds` 是文件内起点；左端拉伸同时改 `startBeat` 与
  offset（同样的秒数），音频留在原时间位置；右端拉伸只改 `lengthBeats`。
- **尾部无上限（P51）**：右端想拖多长拖多长（见"采样伸缩"节）；左 ≥ offset 允许的距离
  （offset 不会 < 0，那个判据仍在）。旧的 `maxSampleLengthBeats()` 已删除。
- **P65：开关没了**。块自动化框底部那行不再是"目标标签 + 启用开关"，整行就是目标按钮
  （`getSampleTargetArea()` 就返回 `getSampleMenuArea()`），曲线画出来就生效。右键菜单里的
  「启用 / 停用曲线」一并删掉——留着它只是把同一个谎言挪一次点击的距离。
  `openSampleAutomation()` 会把 `enabled = false` 的旧曲线恢复成 true：开关删了之后，
  那是唯一的回头路（老工程里可能存在这个状态）。
- **三个命中区（P49 起是两个 + 一个按钮；P51 按钮挪到块顶）**：整高的左右边 = 拉伸，
  其余 = 拖动，**块顶右上**的 chevron = 展开曲线箱（Y = 块顶 + 1，距右缘 = 音符 chevron
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

## 标尺缩放（P31）
- 右键拖标尺 = 以**按下那一点**为锚缩放：`setBeatWidthAround(px, anchorBeat, anchorX)`，
  锚点由 mouseDown 捕获（`rulerZoomAnchorBeat/X`），不是每帧重算。
- 滚轮缩放仍锚定**视图中心**（`setBeatWidth()` 只是它的特例）。两者共用同一份数学。
- 唯一的例外是 `clampScroll()` 在时间线两端生效——左边没有负拍，锚点只能让位。

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
