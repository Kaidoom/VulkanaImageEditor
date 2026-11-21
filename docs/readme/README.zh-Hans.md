# Vulkana 图像编辑器

<!-- readme-languages:start -->
[English](../../README.md) · [Français](README.fr.md) · [Español](README.es.md) · [Deutsch](README.de.md) · [Italiano](README.it.md) · [Português (Brasil)](README.pt-BR.md) · [Polski](README.pl.md) · [Türkçe](README.tr.md) · [Русский](README.ru.md) · [Українська](README.uk.md) · [العربية](README.ar.md) · [हिन्दी](README.hi.md) · [简体中文](README.zh-Hans.md) · [繁體中文](README.zh-Hant.md) · [日本語](README.ja.md) · [한국어](README.ko.md)
<!-- readme-languages:end -->

一款优先面向 Linux 的图像编辑器，用于绘画、修图和图层合成，
提供原生 Windows 构建，采用 Qt 工作区和 Vulkan 加速画布。

![Vulkana 工作区，显示带图层的图像、画笔预设和调整选项](../../res/Preview.png)

## 功能

- **图层编辑：** 栅格、可编辑文字和形状；位图蒙版、文件夹、剪贴蒙版组、
  多选、混合模式、复制、合并和手动栅格化。
- **链接项目与镜像图层** 可以复用已有作品。双击[项目图层](../PROJECT_LAYERS.md)即可编辑源项目，
  回到父项目就能看到更新。在同一文档中，[镜像图层](../MIRRORED_LAYERS_AND_GUIDES.md)会随源图层更新，
  也可以单独设置变换和效果。
- **通道组：** 将打包纹理的各个通道作为灰度图层编辑，支持无损转换、数值绘画、
  修图和调整。参见[通道组指南](../CHANNEL_GROUPS.md)。
- **绘画与修图** 支持压感画笔、填充、局部模糊、仿制图章和修复工具。
  [画笔对称](../BRUSH_SYMMETRY.md)可沿可移动的对称轴镜像笔画，在图层蒙版上也能使用。
  污点修复可填补透明空洞，并能从修复图层上方和下方的可见图层取样。
- **选区：** 矩形、椭圆、自由/多边形/磁性套索、按颜色选择和画笔引导的快速选择，
  支持扩展/收缩、反选和重新选择。
- **对象选择与细化：** 使用随附的离线模型，用粗略的矩形框选对象；通过选区细化、
  实时预览和手动修正改善选区与位图蒙版。使用对象选择无需下载模型或安装 Python。
- **本地 AI 图像编辑** 支持用自己的模型和 LoRA 生成图像、编辑图层和填充选区。比较不同结果后再应用。
  Qwen-Image-Layered 可将一幅图像拆分为多个可编辑图层，放在同一个文件夹中。
  发行包自带 Vulkan 推理进程，使用 NVIDIA GPU 时可另行添加 CUDA 推理进程。
  配置方法和 GPU 要求见 [Vulkana Diffusion](../../DIFFUSION.md)。
- **变换** 包括移动、缩放、旋转、翻转和角点扭曲，可作用于选区边界或选中的像素。
  在画布中央放置参考线或按等间距放置多条参考线，即可通过吸附来定位图层。
- **非破坏性调整：** 各图层独立校正，以及带蒙版的调整图层，作用范围可选 All Below
  （下方所有图层）或 This Group（当前组）；包括曝光、色阶、曲线、色相/饱和度、
  自然饱和度、色彩平衡、冷暖/色调、黑白转换和反相。
- **滤镜与图层效果：** 高斯模糊、动感模糊和镜头模糊；描边、投影/内阴影、外发光/内发光、
  颜色叠加、渐变叠加以及斜面和浮雕。
- **文档标签页** 各自保留历史记录、选区和缩放比例。复制标签页即可在未保存的副本中试改，
  也可以在文档之间复制图层，或将图像拖进图层面板，直接插入图层列表。
- **图层清理** 提供可缩放的透明度预览，方便检查发丝或柔和的边缘。
  “清除反选区域”会清除选区外的像素，并保留当前选区。
  “裁剪至画布边界”会移除所选图层中位于画布外的像素。
- **PSD 导入** 可将内容转换为原生图层，保留富文本、简单形状、位图蒙版、剪贴组和受支持的样式。
  导入时可查看图层需要转换的原因，并将同一处理方式一次应用到所有符合条件的图层。
- **PSD 导出：** 输出分层文件或合并为单层的文件；在支持的情况下保留可编辑文字、
  形状、蒙版和剪贴关系，并可审阅转换情况，同时重新生成合成图像。
- **PDF 导入：** 以指定的 PPI 将选定页面导入为栅格图层或独立文档，
  可使用透明或白色背景。
- **PDF 导出：** 将合成图像导出，或将图层/组分别作为页面导出；支持自定义页面选择、
  画布尺寸或适合内容的尺寸，并在支持的情况下保留可选择的文字。
- **工作区与输出：** 带标签页的可停靠面板、提供平移/缩放控制的导航器、主题、
  可编辑快捷键、标尺、像素预览、原生 `.vulkana` 项目以及 PNG/JPEG/WebP 导出。
  可以只导出所选图层，范围取这些图层全部可见内容的外接矩形，也可以调整画布大小以适应选定图层。
- **语言：** 提供 16 种界面语言，可在“首选项 → 常规 → 语言”中切换，无需重启。
  所有语言版本均为初稿，欢迎审阅并提出修正。

目前处于积极开发中的测试阶段。请为重要项目保留备份。

## 构建与运行

Linux 和 Windows 构建均面向 x86-64。克隆仓库后，在仓库根目录运行对应平台的命令：

```sh
git clone https://github.com/Kaidoom/VulkanaImageEditor.git
cd VulkanaImageEditor
```

### Linux

需要 Linux、C++20 编译器、Ninja、pkg-config 和以下开发库：

- CMake **3.28+**，用于随附的预设。
- Qt **6.8+**：Core、Gui、Widgets、Network、DBus 和 PDF；测试还需要 Qt Test。
- Qt Linguist `lrelease` 和 Python 3，用于构建语言包（仅在构建时使用）。
- qpdf **11+** 开发库（用于 PDF 导入/导出；不是命令行工具）。
- 兼容 zlib 的开发库（用于 PSD ZIP 通道）。
- Vulkan **1.2+** 头文件/加载器、可正常工作的 Vulkan 驱动程序以及 `glslc`。
- minizip-ng **4.x**（`minizip` pkg-config 模块，不是旧版 minizip）。
- libwebp 和 libwebpmux **1.2+**。

请安装适用于桌面环境的 Qt 平台插件、SVG 支持和图像格式插件。
如果 Qt 安装在系统搜索路径之外，请将 `CMAKE_PREFIX_PATH` 指向 Qt 安装目录。
许多发行版将 Qt PDF 作为单独的开发包提供，例如 Fedora 的 `qt6-qtpdf-devel`
和 Ubuntu 的 `qt6-pdf-dev`。Vulkana 运行时无需使用 WebEngine 浏览器。
Qt Linguist 工具由 Fedora 的 `qt6-linguist` 或 Ubuntu 的 `qt6-tools-dev-tools`
提供。如有需要，可设置 `-DVULKANA_LRELEASE=/path/to/lrelease`。
Qt 标准对话框的翻译目录由 Fedora 的 `qt6-qttranslations` 或 Ubuntu 的
`qt6-translations-l10n` 提供。仅构建语言包和审阅翻译的说明见
[本地化文档](../LOCALIZATION.md)。

在仓库根目录执行：

```sh
cmake --preset release -DIMAGEEDITOR_BUILD_TESTS=OFF
cmake --build --preset release --parallel
./build/release/src/app/imageeditor
```

也可以通过参数传入项目和图像：

```sh
./build/release/src/app/imageeditor "project.vulkana" "photo.webp"
```

Qt 根据会话选择 Wayland 或 X11。首选项和自定义预设保存在各用户自己的存储位置。
在线服务为可选功能；未进行[服务配置](../../config/SERVICES.md)时，更新和反馈功能保持禁用。

运行 Release 测试前，请启用测试目标并重新构建：

```sh
cmake --preset release -DIMAGEEDITOR_BUILD_TESTS=ON
cmake --build --preset release --parallel 6
ctest --preset release
```

也可使用调试预设。原生渲染检查需要 Vulkan 验证层。部分交互测试需要正在运行的 Wayland/X11 桌面。

### Windows

安装 [Windows 构建指南](../../tools/windows/README.md#prerequisites)中列出的必备组件。
在仓库根目录打开 Windows PowerShell，然后构建并启动应用：

```powershell
powershell.exe -NoProfile -ExecutionPolicy RemoteSigned -File tools/windows/Build.ps1
C:\Dev\Vulkana\stage\bin\imageeditor.exe
```

脚本会准备依赖项，并创建包含语言包的 Release 构建。
有关[自定义构建路径](../../tools/windows/README.md#configuration)、
[测试](../../tools/windows/README.md#testing)和
[便携目录](../../tools/windows/README.md#portable-folder)的说明，请参阅指南。

## 架构

- `src/core` — 与平台无关的文档、图层、几何、图像算法和可撤销事务。
- `src/render` — 共享的 Vulkan 画布、合成、叠加层和有容量限制的 GPU 缓存。
- `src/ui` — Qt 面板、工具、文档标签页、文字/形状预处理和文件 I/O。
- `src/platform` — 资源查找、桌面集成和可选服务。
- `src/app` — 应用程序入口；`shaders` — Vulkan 着色器源文件。

每个文档拥有自己的内容和历史记录；所有标签页共用一个工作区和 Vulkan 渲染器。
可编辑的源数据与渲染缓存分开保存。画布、像素预览和导出使用相同的合成规则。

参阅[开发者文档](../README.md)、[架构指南](../ARCHITECTURE.md)和 [MCP 配置与工具参考](../MCP.md)。

## 许可证

Vulkana 自有的代码、文档和资源采用 [MIT 许可证](../../LICENSE)。
第三方材料保留各自的许可证；参阅[第三方声明](../../THIRD_PARTY.md)。
