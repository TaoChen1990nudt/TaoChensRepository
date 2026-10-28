# AtomicDAW — 项目长期约定

JUCE 8 音频工作站：图层叠加工作区 / 音源机架 / 音符级微观包络。分阶段交付，
每阶段只做一件事，必须能编译并跑通视觉或数据逻辑。

**当前阶段、自检基准数、构建命令看 `STATUS.md`**。**详细规则在 `REFERENCE.md`**（按主题分节：构建、路由、音频、工程文件、混音台、编辑手势、
编组、采样、走带、文件树、波形、伸缩、缩放、电平表）；平台陷阱与自检夹具在 `PITFALLS.md`。
动代码前按主题查这两份，本文件只留跨主题硬规则。

## 六条硬规则
1. **中文一律 `atomic::utf8("...")`**，自检的失败信息也算。裸中文 = 乱码。
2. **几何走 `ui::px()/pf()`、字号 `ui::sp()`**，唯一密度旋钮 `ui::scale = 1.3f`，禁止裸像素。
   `numPitches` / gutter / 内联自动化框最小宽度不缩放。
3. **路由只有 `Model/Routing.h`**：`resolve()`（音符优先级链）、`resolveSample()`（采样，不看组）、
   `layerSlotId()`（兜底跳过 sampler）。UI 高亮与引擎必须调同一份，否则显示和听到是两套话。
4. **音频线程不读 Layer / NoteEvent / RackModel**：消息线程 `publishSchedule()` 产出不可变
   `PublishedSchedule`，锁只覆盖 shared_ptr 赋值；发布期把拍换成秒，音频线程只做乘加。
5. **改模型后必发 `layersChanged()` / `notesChanged()`**：采样槽清扫
   `pruneOrphanSamplerSlots()` 挂在这两个通知上，不是回调——"谁记得调回调"不是规则。
6. **唯一入口**：会话重置 = `buildSession(withEmptyLayer)`；删槽 = `removeSlotAndItsClips()`
   （先删块再删槽）；撤销 = 整工程 XML 快照，**粒度是一次鼠标手势**（`beginUndoGesture()` /
   `endUndoGesture()` 包住按下与释放，手势期间 `commitHistory()` 直接返回）；试听分岔 =
   `MainComponent::auditionPreview()`（插件槽发真 MIDI，内置合成器才发正弦）。

## 目录
- `Source/Model/` 数据与路由（LayerModel / RackModel / MixerModel / Routing / ProjectIO / Gain）
- `Source/Audio/` 引擎与纯函数 DSP（PlaybackEngine / ClipStretch / 导出）
- `Source/UI/` 时间线、机架、混音台、文件树、自动化、分栏
- `Source/MainComponent.*` 装配、走带、撤销、快照与自检

## 构建
VS2022 + MSVC 14.38 + Win SDK 10.0.22621 + JUCE 8.0.4（FetchContent）。PATH 无 cmake，
用 VS 自带那份。**构建前 `unset https_proxy http_proxy all_proxy no_proxy`**（大小写两份），
否则 MSB6001 谎报找不到编译器。

## 工程文件
`.atomic`，`formatVersion = 25`（逐版字段见 `REFERENCE.md`）。老版本必须仍能开。
**"选中哪条自动化 lane"这类状态必须显式写属性**，不能靠元素顺序或别的属性推断。
先解析到临时对象、末尾一次性提交，坏文件绝不破坏当前会话。
