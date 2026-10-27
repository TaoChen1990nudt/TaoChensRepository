# AtomicDAW UI 规范（草案 v0.1）

> 参考基准：`linear.app`（awesome-design-md 第 1 号候选，也是这批里最深的深色系统）
> 状态：**草案，供评审**。未改动任何产品代码。
> 生效范围：`Source/UI/`、`Source/Util/UiLookAndFeel.h`、`Source/MainComponent.*` 的绘制层。

---

## 0. 为什么选 Linear，以及为什么不能照抄

**选的理由**（对 DAW 有效的三条）：

1. 表面阶梯做层级 —— 五级 surface 靠明度分层，不靠阴影。DAW 面板多、嵌套深，阴影会在相邻面板间互相污染。
2. 单一彩色强调 —— 全站只有一个 chromatic accent，用在焦点环和主 CTA。DAW 里波形和电平表必须保持中性，否则判断被干扰。
3. hairline 边框 + 零装饰渐变 —— 分割线是 1px 实色，不是渐变、不是发光。

**不能照抄的**：

| Linear 的做法 | AtomicDAW 必须偏离 | 原因 |
|---|---|---|
| canvas 几乎纯黑 `#010102` | 用 `#0a0b0e` | 纯黑下波形正负半周对比过强，久视刺眼 |
| 卡片圆角 12px | 最大 6px | DAW 是密集网格，大圆角削弱对齐感 |
| Linear Display 私有字体 | 系统 UI 字体 + 等宽数字 | JUCE 不能加载网页字体，且时间码/电平值需要 tabular figures |
| 只有一个语义色（success） | 5 个语义色 | 录音、独奏、旁通、播放、危险是 DAW 的一等公民 |
| 营销页留白节奏 | 无留白，密度优先 | 屏幕空间是硬约束 |

---

## 1. 现状诊断（实测，不是感觉）

在 `Source/` 里统计：

```
颜色字面量总数   159 处
不同色值         80 个
最高频色值       #4fd1c5 ×24（teal）
近黑色值         40 个（RGB 三通道 ≤ 0x50）
```

**结论：这套 UI 不缺品味，缺的是颜色预算。** 40 个近黑里有大量视觉上无法区分的值（`#0d0e11` / `#0d0e12` / `#0d0e13`、`#15161b` / `#15171c` / `#16171c`），每次新写一个面板就随手取一个"看起来差不多"的黑。结果是层级没建立起来，因为七个不同的黑在眼睛看来是同一个黑。

`phase46.png` 上同时在场的饱和色：teal、amber、violet、pink、blue、gold、mint、coral、红播放头 —— 9 个。每张卡片还配了自己的彩色描边。

**这份规范的唯一目标：把 80 个色值收敛到 24 个 token，并且让"同时出现的饱和色"有上限。**

---

## 2. 三条不可谈判的原则

**① 颜色预算。** 同一屏幕内，饱和色（HSB 的 S > 40%）不超过 2 个：`accent` + 至多 1 个语义色。轨道身份色不计入配额，但只允许出现在 **≤ 3px 宽的条、1.5px 的描边、≤ 0.12 alpha 的填充**上。不做大面积饱和填充。

**② 层级靠表面，不靠描边。** 一个元素"比较靠前"要表达成 surface 升一级 + hairline，不是加一圈彩色边框。彩色边框只表达**状态**（选中、激活、错误），不表达**容器**。

**③ 身份靠形状，不靠色相。** Layer 和 Group 的区分已经由位置和形态承担。颜色只是辅助记忆，不承担语义。所以身份色必须低饱和 —— 它不该和"你有事要处理"（accent/红/黄）抢注意力。

---

## 3. 色板

### 3.1 表面阶梯（6 级，全部取自现有高频值）

| Token | 现值（第一步照用） | 目标（第二步） | 用途 |
|---|---|---|---|
| `bgApp` | `#0d0e11` | `#0a0b0e` | 窗口最底层、面板之间的缝 |
| `surface0` | `#101114` | `#101114` | 主工作区画布（钢琴卷帘底、自动化画布底） |
| `surface1` | `#14151a` | `#14151a` | 常规面板（图层卡、机架卡、混音条） |
| `surface2` | `#171922` | `#171922` | 面板内的分区、列表行、轨道头 |
| `surface3` | `#1b1d24` | `#1b1d24` | 可交互控件底（输入框、下拉框、卡片） |
| `surface4` | `#2c2e37` | `#2c2e37` | 悬停态、按下态、激活边框 |

> **修正记录（09:20）**：初稿把 `surface-3` 定为 `#262832`，错了。代码里 `#262832` 的实际角色是 **5 个文件里的 `kDivider`** —— 分隔线，不是控件底。真正的控件底是 `#1b1d24`（`EffectWindows::kPanel`、`RackPanel::kCardBackground`）。所以 `#262832` 归到 `hairline`，阶梯整体下移一级。

**规则**：任何新的背景色必须从这六个里选。不许再出现第七个近黑。需要区分两个同级面板时，用 `hairline` 分隔，不要改背景。

### 3.2 墨色（文字，4 级）

| Token | Hex | 用途 |
|---|---|---|
| `ink` | `#eef1f6` | 主数值、时间码、当前选择 |
| `ink-muted` | `#9aa0ad` | 正文、标签、轨道名 |
| `ink-subtle` | `#7d8492` | 次级说明、单位、辅助信息 |
| `ink-faint` | `#5a5f6d` | 禁用、占位、网格刻度数字 |

### 3.3 语义色（5 个，全部保留现值）

| Token | Hex | 现有出现次数 | 语义 | 用法约束 |
|---|---|---|---|---|
| `accent` | `#4fd1c5` | 24 | 选中、焦点、自动化曲线、滚动条 | 细线和小面积，不做大填充 |
| `record` | `#ff5f6d` | 8 | 录音、播放头、破坏性操作 | 播放头 1.5px 竖线，按钮按下态填充 |
| `live` | `#5ecf9a` | 4 | 正在运行、成功 | 只做状态指示点和小图标 |
| `warn` | `#e0b13c` | 3 | 警告、旁通、独奏 | 填充 alpha ≤ 0.18 |
| `link` | `#7c96f5` | 3 | 信息、链接、提示 | 文字色和小图标 |

### 3.4 hairline

| Token | Hex | 用途 |
|---|---|---|
| `hairline` | `#262832` | 常规分隔线、面板边界（现 `kDivider`，5 个文件） |
| `hairlineStrong` | `#343743` | 需要强调的分界（分栏拖动条、浮层外框） |

### 3.5 身份色（Layer ×5 / Group ×5，降饱和）

现在两套都是 Tailwind 300–400 系，在深底上过亮。**改法：保留色相，S × 0.80，V × 0.88**（在 HSB 空间算，不要手改 hex）。参考结果：

**Layer**
| | 现值 | 建议 |
|---|---|---|
| layer1 | `#4fd1c5` teal | **不动**（见下方说明） |
| layer2 | `#f6ad55` amber | `#d99a4d` |
| layer3 | `#9f7aea` violet | `#8a6ccb` |
| layer4 | `#ed64a6` pink | `#cf5a91` |
| layer5 | `#63b3ed` blue | `#599ecf` |

**Group**
| | 现值 | 建议 |
|---|---|---|
| group1 | `#ffd166` gold | `#dfb95d` |
| group2 | `#8ce99a` mint | `#7bcb88` |
| group3 | `#ff9f7a` coral | `#df8c6e` |
| group4 | `#74c0fc` sky | `#68a8d9` |
| group5 | `#d0bfff` lilac | `#b6a9df` |

**关于 `layer1 == accent` 这两个同值的问题**

`palette::layer1` 和 `MainComponent::kAccent` 都是 `#4fd1c5`。这是个真实的可读性缺陷：看到一片青色，分不清"这是 1 号图层"还是"这个被选中了"。

草案的处理是**不改色值，改用法**：`layer1` 做大面积实体（卡片色条 3px、音符块填充），`accent` 只做细线（1px 描边、1.5px 曲线、文字）。同色相、不同形态，靠用法区分。

不换色相的理由是成本：改 `layer1` 会让所有已有工程和新截图对不上，而 24 处 `accent` 更难动。**长期选项**：给 `accent` 换到 Linear 的 lavender-blue `#5e6ad2`，把 teal 彻底让给 layer1。这需要一次全局改色，建议等视觉规范稳定后再做。

---

## 4. 角色映射（具体部件用哪个 token）

| 部件 | 背景 | 文字 | 描边/线 |
|---|---|---|---|
| 窗口底 | `bg-app` | — | — |
| 面板（图层卡 / 机架卡 / 编组卡） | `surface-1` | `ink-muted` | `hairline` |
| 面板内分区标题 | 透明 | `ink-subtle`（大写 + 字距） | — |
| 轨道头（钢琴卷帘左侧） | `surface-2` | `ink-muted` | `hairline` |
| 自动化轨道头 | `surface-2` | `ink-muted` | `hairline` |
| 输入框 / 下拉框 / 数值框 | `surface-3` | `ink` | `hairline`（焦点时 `accent`） |
| 按钮（常态） | `surface-3` | `ink-muted` | `hairline` |
| 按钮（悬停） | `surface-4` | `ink` | `hairline-strong` |
| 按钮（激活/工具选中） | `surface-4` | `ink` | `accent` 1px |
| 音符块 | 图层身份色 α0.22 | `ink` | 图层身份色 α0.7，1px |
| 音符块（选中） | 图层身份色 α0.32 | `ink` | `accent` 1.5px |
| 音频块 | `surface-2` | `ink` | 图层身份色 α0.6 |
| 音源块（UI 渲染的波形外框） | 图层身份色 α0.16 | `ink` | 图层身份色 α0.55 |
| 网格线（小节） | — | — | `hairline-strong` |
| 网格线（拍） | — | — | `hairline` α0.6 |
| 网格线（拍内细分） | — | — | `surface-3` |
| 播放头 | — | — | `record` 1.5px |
| 自动化曲线 | — | — | 轨道身份色 1.5px |
| 自动化曲线下方填充 | 轨道身份色 α0.10–0.12 | — | — |
| 电平表（正常） | — | — | `live` α0.85 |
| 电平表（-6dB 以上） | — | — | `warn` |
| 电平表（0dB 削波） | — | — | `record` |
| 浮层（效果器窗、自动化窗、菜单） | `surface-1` | `ink-muted` | `hairline-strong` + 阴影 |

---

## 5. 排版

**全部走 `atomic::ui::font()` / `bold()`，参数是设计空间 pt。禁止裸字号。**

| 角色 | 设计空间 | 字重 | 字距 | 颜色 |
|---|---|---|---|---|
| 时间码 / 主数值 | 20 | Bold | -0.5 | `ink` |
| 面板主标题（"图层与混音"） | 11 | Bold | +0.5 | `ink` |
| 面板副标题（"LAYER MIX · 3"） | 8.5 | Bold | +0.8，全大写 | `ink-faint` |
| 正文 / 轨道名 / 标签 | 10 | Regular | 0 | `ink-muted` |
| 数值（dB、拍号、百分比） | 10 | Regular | 0 | `ink`，**等宽数字** |
| 次级说明 / 单位 | 9 | Regular | 0 | `ink-subtle` |
| 微标（时间线刻度、行号） | 8.5 | Regular | 0 | `ink-faint` |

**两条硬要求：**

1. **数值必须等宽。** 所有会变的数字（dB、BPM、时间码、拍位、电平读数）要 `juce::FontOptions().withExtraKerningFactor(0)` 且用带 tabular figures 的字体。现在 BPM 从 120 变到 87 时数字宽度会跳，整行都在抖。JUCE 里用 `juce::Font::getStringWidthFloat` 对齐，或直接给数值框固定宽度 + 右对齐。
2. **层级靠字距和大写，不靠字号。** 面板标题 11pt 和正文 10pt 只差 1pt，但加上 Bold + 字距 +0.5 vs Regular，层级就出来了。别靠把标题放大到 14pt 来做层级 —— 那会挤掉内容。

**不做负数行高、不做斜体、不做彩色文字**（除 `record` 录音态和 `link` 信息态）。

---

## 6. 几何

全部走 `ui::px()` / `ui::pf()`。下列数字是**设计空间**值，运行时会被 `scale = 1.3` 放大。

### 6.1 圆角

| Token | 设计空间 | 用途 |
|---|---|---|
| `radius-xs` | 2 | 音符块内的小标记、色条端点 |
| `radius-sm` | 3 | 音符块、音频块、标签胶囊（高度 ≤ 20 时） |
| `radius-md` | 4 | 按钮、输入框、下拉框 |
| `radius-lg` | 6 | 面板卡片、浮层 |

**禁用 pill 圆角**（`height/2`），除了状态胶囊（如"3 组联动"这种短标签）。

理由：现在 `phase46.png` 上几乎每个容器都是 8–12px 圆角，连 24px 高的按钮都是半圆。视觉上整块界面在"发胖"。密集网格里，圆角每增大 2px，相邻元素的间隙感就削弱一档。

### 6.2 边框

- 常规：1px，`hairline`
- 强调：1px，`hairline-strong`
- 选中：1px `accent`（组件类）或 1.5px（音符块、曲线）
- **不用 2px 以上描边。** 需要更强的边界时用背景对比，不要加粗线。

### 6.3 间距（4 的倍数）

`2 / 4 / 6 / 8 / 12 / 16 / 24`。设计空间的这些值 × `scale`。

面板内边距 `8`，面板之间缝 `4`（填 `bg-app`），控件内部左右 `6`，行间距 `4`。

### 6.4 阴影

**只有浮层有阴影。** 静态面板零阴影。

```
浮层：0, 8, 24, rgba(0,0,0,0.55)
菜单：0, 4, 16, rgba(0,0,0,0.60)
```

因为 DAW 里静态面板是贴边平铺的，任何一层阴影都会渗到相邻面板上，形成脏边。

---

## 7. 组件规格

### 7.1 轨道头（钢琴卷帘左侧 / 自动化左侧）

- 高度：图层行 74（现值），自动化行按现有
- 背景 `surface-2`，右侧 1px `hairline` 分界
- 左侧 3px 身份色竖条（`radius-xs`，上下各内缩 2）
- 轨道名：10pt Regular `ink-muted`，单行截断加省略号
- 行号/音高：8.5pt `ink-faint`，右对齐
- 选中：背景升到 `surface-3`，左侧色条宽度不变（**不要加外框**）
- 播放中：色条换 `live`

### 7.2 音符块

- 填充：身份色 α0.22
- 描边 1px：身份色 α0.70
- 圆角 `radius-sm`
- 内部文字（音名/组名）：8.5pt Bold `ink`
- 选中：填充 α0.32，描边改 `accent` 1.5px
- 冲突/重叠：描边改 `warn` 1.5px
- 最小可视宽度 `6`（设计空间），小于则只画色条不画文字

### 7.3 自动化曲线

- 曲线 1.5px 身份色
- 下方填充身份色 α0.10（现在是 α0.3+，太重，四条叠在一起像四块色板）
- 控制点：4×4 实心圆，身份色；悬停时 `accent` 描边
- 当前拍位置标记：`accent` 垂直细线 α0.5
- **不需要网格底色**：底用 `surface-0`，靠 `hairline` 网格线定位

### 7.4 电平表（竖直双声道）

按现有 `MixerLook.h::drawMeter` 的几何，只改颜色：

- 底槽：`surface-3`
- 正常段：`live` α0.85
- -6 dB 以上：`warn`
- 0 dB / 削波：`record`
- 峰值保持线：`ink` α0.9，1px
- 刻度数字：8.5pt `ink-faint`，等宽

### 7.5 按钮

| 状态 | 背景 | 文字 | 描边 |
|---|---|---|---|
| 常态 | `surface-3` | `ink-muted` | `hairline` |
| 悬停 | `surface-4` | `ink` | `hairline-strong` |
| 按下 | `surface-4` | `ink` | `accent` |
| 激活（工具/开关） | `surface-4` | `ink` | `accent` 1px + 左侧 2px `accent` 条 |
| 录音 | `record` α0.20 | `record` | `record` |
| 禁用 | `surface-2` | `ink-faint` | `hairline` α0.5 |

高度 24（设计空间），圆角 `radius-md`，内边距左右 6。

### 7.6 浮层（效果器窗 / 自动化窗 / 菜单 / 提示）

- 背景 `surface-1`
- 外框 1px `hairline-strong`
- 阴影按 6.4
- 圆角 `radius-lg`
- 标题栏高 28，背景 `surface-2`，标题 10pt Bold `ink`
- 关闭按钮用文字 `×` 10pt `ink-subtle`，悬停 `record`

---

## 8. 落地方式

### 8.1 新建 `Source/Util/Theme.h`

> **为什么叫 `Theme` 不叫 `Palette`**：`atomic::palette` 已被 `Model/LayerModel.h` 占用（layer/group 身份色）。改那个名字要动模型层和所有引用处，超出第一步范围。用 `atomic::theme` 避开冲突。

```cpp
#pragma once
#include <juce_gui_basics/juce_gui_basics.h>

namespace atomic::theme
{
//-- surfaces ----------------------------------------------------------------
// 第一步的值 = 现有代码里的实际值，保证视觉零变化。
// 第二步按 DESIGN.md 3.1 的目标值逐个替换（只有 bgApp 需要变）。
inline const juce::Colour bgApp     { 0xff0d0e11 };   // 目标 #0a0b0e
inline const juce::Colour topBar    { 0xff16171c };
inline const juce::Colour surface0  { 0xff101114 };
inline const juce::Colour surface1  { 0xff14151a };
inline const juce::Colour surface2  { 0xff171922 };
inline const juce::Colour surface3  { 0xff1b1d24 };
inline const juce::Colour surface4  { 0xff2c2e37 };

//-- ink ---------------------------------------------------------------------
inline const juce::Colour ink        { 0xffeef1f6 };
inline const juce::Colour inkMuted   { 0xff9aa0ad };
inline const juce::Colour inkSubtle  { 0xff7d8492 };
inline const juce::Colour inkFaint   { 0xff8b91a0 };   // 现 kMutedText / kMuted

//-- hairlines ---------------------------------------------------------------
inline const juce::Colour hairline       { 0xff262832 };   // 现 kDivider
inline const juce::Colour hairlineStrong { 0xff343743 };   // 现 kPanelBorder

//-- semantics ---------------------------------------------------------------
inline const juce::Colour accent { 0xff4fd1c5 };
inline const juce::Colour record { 0xffff5f6d };
inline const juce::Colour live   { 0xff5ecf9a };
inline const juce::Colour warn   { 0xffe0b13c };
inline const juce::Colour link   { 0xff7c96f5 };

//-- shadows ----------------------------------------------------------------
inline juce::Colour shadowForOverlay() { return juce::Colours::black.withAlpha (0.55f); }
}
```

**注意**：`palette::layer1..5` 和 `groupPalette::group1..5` 已在 `Model/LayerModel.h` 里，那条定义**保留在原处**（模型层依赖它，别搬到 UI 层），只在 UI 层引用。

### 8.2 迁移顺序（三步，每步可独立编译验证）

**第一步：建立 token，不动视觉。 ✅ 已完成（2026-10-27 09:20）**

实际做法比计划大一点：不只 `MainComponent.cpp` 的 3 个，而是把**所有值与 token 完全一致的重复定义**都接上了 —— 12 个 UI 文件 + `MainComponent.cpp`，共 **52 处引用别名**。

用引用别名（`const juce::Colour& kAccent = theme::accent;`）而不是重命名，所以 **200 多处使用点一行没动**。

**验证方式不是看图，是逐字节比对**：把改动前的 `Source/` 备份下来，解析每一处别名的 `theme::token` 实际值，和备份里的原 `0x...` 逐字节对比。

```
Theme.h 解析出 18 个 token
找到 52 处引用别名
逐字节相同 52    不一致 0    无法核对 0
```

**编译验证**：13 个改动文件全部通过编译，零 error 零 warning。构建被 `SelfTest.cpp` 里 P47 的 `params` 重构（`std::array<float,9>` 给 const 赋值，7845–7847 行）阻断，与本次改动无关 —— 见下节。

**第二步：收敛表面色（收益最大，风险最低）。 ✅ 已完成（2026-10-27，P48）**
按表归位，另把 3 处头文件默认 accent、RackPanel 静音灰、3 处低 alpha 白 hover
（改写成 `white.withAlpha`）一并归位。纯替换，不改布局不改逻辑。

**当前仍未收敛的 31 个定义**（第一步刻意没碰，值不在 ladder 上）：

| 文件 | 常量 | 现值 | 归到 |
|---|---|---|---|
| `AutomationLaneComponent` | `kLaneBackground` / `kLaneBorder` / `kGrid` | `#12131a` / `#2a2d38` / `#20222c` | surface0 / surface3 / hairline |
| `AutomationWindow` | `kPanel` / `kRowOdd` | `#15161b` / `#111216` | surface2 / surface1 |
| `BpmField` | `kFieldBg` / `kFieldHover` / `kFieldEdge` | `#1e2027` / `#262932` / `#333744` | surface3 / surface4 / hairlineStrong |
| `FileBrowserPanel` | `kFileText` / `kFolderText` / `kPreviewAccent` / `kScanAccent` | `#b9bfcc` / `#d6dae2` / `#7bd88f` / `#e0b268` | ink / ink / live / warn |
| `MixerPanel` | `kPanel` / `kStrip` / `kMaster` | `#15161b` / `#191b21` / `#e2e8f0` | surface2 / surface2 / ink |
| `NoteEnvelopePanel` | `kEditorBg` / `kGroupColour` / `kStackColour` | `#101116` / `#ffa06b` / `#ffd479` | surface0 / 待定 / 待定 |
| `PianoRollComponent` | `kGridBackground` / `kBlackRowColour` / `kRulerBackground` / `kKeyWhite` / `kKeyBlack` | `#17181f` / `#0e0f14` / `#1a1b21` / `#d7dae2` / `#26282f` | surface2 / bgApp / surface2 / ink / surface4 |
| `RackPanel` | `kCardHover` / `kCardBorder` | `#212430` / `#2b2e39` | surface4 / hairline |
| `SettingsWindow` | `kPanel` / `kRow` | `#15161b` / `#191b21` | surface2 / surface2 |

注意 `#15161b`（3 个文件）和 `#191b21`（2 个文件）各自重复了，归到同一级就自然合并。

**第三步：削饱和 + 收圆角（视觉变化明显，需评审）。 ✅ 已完成（2026-10-27，P48）**
身份色按 3.5 表降饱和（layer1 不动）；自动化填充 0.13→0.12；
58 处圆角收紧（2.5→2、5→4、7/8/9/10/18→6，按调用参数位置精确替换，
尺寸参数不受影响）。截图见 `docs/phase48.png`。

### 8.3 自检

`--snapshot` 跑完，加三条断言到现有自检里（都是纯函数，好测）：

1. `Source/` 下不再出现 `0xff` 字面量（除 `Palette.h` 和 `LayerModel.h` 的两套身份色板）—— 可以用一个 grep 脚本在构建前跑，不必进 C++。
2. 自动化曲线填充 alpha ≤ 0.12。
3. 所有圆角值 ∈ {2, 3, 4, 6}（设计空间）。

第 1 条最有用。它是防漂移的唯一手段 —— 一旦不设卡，三个月后又会冒出 42 个近黑。

---

## 9. 明确不做的事

- **不引入 UI 框架 / 不换 LookAndFeel 体系。** 现有 `UiLookAndFeel` 只调字号，这个分工是对的，继续用。
- **不做浅色主题。** 深色基底上做浅色主题等于重画一遍，且 `bg-app` 系列 42 个近黑里没有一个能直接用。真要做，等这份规范稳定后单独立项。
- **不做动效。** Linear 的悬浮渐变、光晕在 DAW 里没有信息价值，且会干扰电平表和波形的动态判断。
- **不改 `ui::scale`。** 1.3 是调过的，这份规范里的所有数字都是按 1.3 校准的。要改密度，改那一个常数，然后重看全套截图。
- **不批量重写 `PianoRollComponent.cpp`（8115 行）。** 它是最大的单文件，逐处改颜色即可，结构不动。

---

## 10. 待确认

1. ~~`.atomic` 工程存的是颜色值还是调色板索引？~~ **已确认：存色值。** `ProjectIO.cpp` 用 `colourToHex` / `colourFromHex` 把 ARGB 写进 `colour` 属性，读回时不做映射。
   **后果**：第 3.5 节的降饱和只影响**新工程**，老工程打开后仍是老色值 —— 新旧截图会不一致。
   **建议：不自动映射。** 工程文件存色值意味着"颜色属于工程数据"，擅自改会破坏用户打开旧工程时的预期。如果确实需要统一，做成一个显式的"视觉升级"菜单项，让用户自己点。
2. 现有 `kPlayhead` 是 `#ff5f6d`（红），但顶栏"运行中"指示点用的是 `live` 绿。录音时播放头该不该变色？建议：录音用 `record`，播放用 `accent`，这样红只表示"在录"。
3. 音源块的紫色波形外框（`phase46` 上的 `demo-clip.wav`）现在用的是紫色，既不是图层色也不是语义色 —— 需要定它是"音频块"还是"音源身份"。

---

## 11. 第一步顺带查出的两个真 bug

收敛过程中逐文件核对常量值，发现**同一个概念在不同面板用了不同颜色**。这不是风格问题，是同一个状态在界面上有两种长相。

### 11.1 静音：红 vs 黄

| 位置 | 常量 | 值 |
|---|---|---|
| `LayerPanel.cpp:21` | `kMuteColour` | `#ff6b6b` 红 |
| `RackPanel.cpp:23` | `kMuteColour` | `#ff6b6b` 红 |
| `GroupPanel.cpp:22` | `kMuteColour` | `#ffb020` 黄 |
| `MixerPanel.cpp:21` | `kMute` | `#ffb020` 黄 |

图层面板和机架面板用红，编组面板和混音台用黄。用户在同一首歌里看两个面板，同一个"静音"是两种颜色。

### 11.2 独奏：黄 vs 绿

| 位置 | 常量 | 值 |
|---|---|---|
| `LayerPanel.cpp:19` | `kSoloColour` | `#e0b13c` 黄 |
| `RackPanel.cpp:22` | `kSoloColour` | `#e0b13c` 黄 |
| `MixerPanel.cpp:22` | `kSolo` | `#5ecf9a` 绿 |

图层面板黄，混音台绿。而绿 (`live`) 在本规范里是"正在运行/成功"的语义，拿来当独奏会和播放状态指示撞。

**已修（2026-10-27，P48）**：
- 静音四处统一到 `warn` 黄（红留给录音和危险操作）
- 独奏四处统一到 `accent`（选了建议的第一项；`live` 绿彻底留给运行态）
