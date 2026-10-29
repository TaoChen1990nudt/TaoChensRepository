# AtomicDAW — 当前状态索引

> 最后更新：2026-10-30 · **Phase 82**（自动化曲线的路由下拉）
> 本文件只回答三件事：**现在在哪、下一步、怎么验**，约 6 千字。
> 规则 `MEMORY.md` · 制度 `REFERENCE.md` · 坑 `PITFALLS.md` · 逐阶段实现记录 `memory/<日期>.md`
> · 被压掉的历史叙述（阶段全文 / 咬测流水 / 逐版沿革 / P62–P82 经验）在 **`STATUS-archive.md`**。

## 新会话读文档的顺序

1. **本文件**（1 分钟）
2. **`MEMORY.md`** — 十六条跨主题硬规则 + 目录/构建/工程文件约定（必须全文读）
3. **`REFERENCE.md`** — 按主题查制度（构建/路由/音频/工程文件/混音台/编辑手势/编组/采样/走带/波形/伸缩…）
4. **`PITFALLS.md`** — 卡住了再查对应阶段
5. **`memory/<日期>.md`** — 想知道「为什么这么写」才翻

不要从日志开始读。日志是流水账，一份能到 1700 行。

## 当前状态

| 项 | 值 |
|---|---|
| 已完成阶段 | **Phase 82** — 三个自动化框底部改成两个下拉：左边选「哪条曲线」（含新建），右边选「路由到哪」（含**空**）。核心是 `Model/CurveBank<CurveType>`：`CurveSlot{id, name, target, pluginParam, curve}`，**`target = -1` 是一个合法答案**（画着、存着、不驱动谁），于是「哪条曲线」和「驱动哪个参数」第一次成了两件事 |
| 自检 | **714 ALL PASSED / 0 FAIL**，失败退出码 1 |
| 编译 | 零 error 零 warning |
| `formatVersion` | **26** — 新增 `<Curve id name target pluginparam enabled>` + `<CurveState active/>`；**v25 及更老的文件照常打开**，只有重新保存才写新形状 |
| 可执行 | `build/AtomicDAW_artefacts/Release/AtomicDAW.exe` |
| 技术栈 | JUCE 8.0.4（FetchContent）+ VS2022 + MSVC 14.38 + Win SDK 10.0.22621 + C++17 |
| 源码规模 | `Source/` 下 88 个 .cpp/.h，分 Model / Audio / UI / Util / Media |

## 下一步

**未定**，等你给功能排期。P58–P82 的逐条主题不在这里重复，见下面的表和 `STATUS-archive.md`。

## 最近六轮（新会话最需要的前情）

- **P82 曲线路由** — `CurveBank` 把音符/采样/编组三个对象的曲线集合统一成一张有序表；旧访问器
  （`envelope` / `volumeCurve` / `automationFor` / `laneDriving`）全做成薄壳，引擎与三个编辑窗一行没动。
  磁盘上「空」= **没有 `target` 属性**（写成词会被读回 level）。删掉了 `expandedGroupTarget` 缓存字段。
- **P81 录音** — `initialise(2, 2, …)` 才是「能录音」的前置条件；拍号在 MIDI 线程随事件过桥；
  一次按键落两样东西：音频→采样块、音符→**新编组**、手拧的旋钮→那个组的 lane（先稀疏化到 32 点）。
- **P80 速度曲线** — `secondsPerBeat` 从全工程消失，换成 `Model/TempoMap`（段内 BPM 线性 ⟹ 求秒取对数）。
  `renderNextBlock` **每段**重算 `beatsPerSample`。
- **P79 三个名字** — `Layer::name` / `NoteGroup::name` / `SynthSlot::name` 各只有一处字段，双击那行就地改；
  唯一例外是采样块的 `SampleEvent::name`（导入时抄来的副本，所以只有它要跟随规则）。
- **P78 采样包络窗** — 双击开**窗口**不是内联窄条；采样那一问必须排在 `hitTestActiveLayer` 之后
  （块画在音符下面，压在块上的音符才是指针看着的东西）。
- **P77 ghost** — 挡单击、不挡双击：「点穿」是"抓不到"，「双击」是"让我过去"。

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
- **录音的已知边界**：一次按键只产出一个组，**分两次录同一段会得到两个组**（没有"接着上一组继续录"）；
  录进来的音频**不被切分**——一次按到底就是一个块，不会按静音段自动分段；录制期间**不画实时波形、不画录音头**，
  画布上只有那颗红点变亮；采样率跟随设备（48k），不与工程帧对齐；`RecordBuffer` 满了（默认 300 秒）**自己停**，
  界面上没有提示，停下来时那股手还在按的音符会被正常收尾。CC 只映射三条，其余**丢弃不猜**。
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

| 阶段 | 主题 | 位置 |
|---|---|---|
| P1–P10 | 图层钢琴窗 / 音符包络 / 音源机架 / 走带 / 混音 / 工程读写 / 滚动与效果架 / 独立混音台窗 / VST3 宿主 | `memory/2026-10-23.md` |
| P10–P13 | 空白启动 / 机架自动化进窗 / 效果器界面 / 1.3x UI 放大 | `memory/2026-10-24.md` |
| P13–P21 | 右键菜单 / 走带图标 / 编组与聚光灯 / 采样四栏 / 吸附标尺 / 双向拉伸 / 光标中线 | `memory/2026-10-25.md` |
| P22–P43 | 刚体拖动 / 127 通道 / 分栏跟手 / 文件树 / 采样窗口与淡变 / 高亮不压暗 / 伸缩三模式 / 走带落点 / 清扫不变量 / 删除闸门 / 组管采样 / 三层叠加 / 粘贴吸附 / 按键发声 / 默认最大化 / VST 松手 / 插件窗按键与音源参数自动化 / 混音台按键与文件栏抬高 / 琴键音源统一 / 效果器换位置 / 通道条四控件自动化 | `memory/2026-10-26.md` |
| P44–P55 | 自检退出码门禁 / const 设计债还清 / 混音链三件套 / 效果器多参数化 / 视觉规范 / 采样音量曲线 / 声相与整层渲染成采样 / 停止回起点 / 机架卡拉伸 / 刻刀 / 卡片高亮统一成一个环 | `memory/2026-10-27.md` |
| P56–P76 | 多选拉伸 / 组框贴成员 / 组 = 定义 + 出现 / 编组栏镜像 / 缩放视口 / 同源编组 / 剪贴板 / 播放跟随 / 图层栏眼睛 / 网格退档与吸附 / 1/64 拍 / 全量审查 16 条 / 撤销按动作 / 块高可拖 / 编组双击才可改 / 选中即整组 / 组内空白 / 打开≠选择 | `memory/2026-10-28.md` |
| P77–P82 | ghost 换层 / 采样包络窗 / 三个名字 / 速度自动化 / 录音 / 曲线路由下拉 | `memory/2026-10-29.md` |
| 踩坑索引 | 各阶段的不变量与咬测 | `PITFALLS.md` |

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

断言总数只增不减。**新增功能要加断言并报出新的总数**（当前基准 **714**），退回去说明有东西坏了。

其他 dev 参数：`--cursor-preview <png>`（光标 8x 放大核对）、`--pluginscan`、`--audiotest`、`--transporttest`、`--load` / `--save` / `--export`。

## 未建成的东西

- `.scratch/<feature-slug>/` — `AGENTS.md` 里约定的 issue 存放处，**目录还不存在**
- `GLOSSARY.md`、`docs/adr/` — 同上，声明了但未创建（按约定 lazy 创建）
