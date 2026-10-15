# Third-party materials

- **Qt**：Windows 使用 Qt 6.8.0，Linux 预编译包使用 Qt 6.11.1；动态链接 Core、Gui、Widgets 及平台 / 图片插件，按 Qt 提供的 GPL v3 / LGPL v3 条款分发。项目自身 GPL-3.0-or-later。许可证副本在发布包 `licenses`；未修改 Qt。对应官方源代码：<https://download.qt.io/archive/qt/6.8/6.8.0/single/>、<https://download.qt.io/archive/qt/6.11/6.11.1/single/>。
- `assets/official`：来自用户本地 35inchNEW 软件包的主题、背景及状态条图片，用于兼容实现与本地验证；这些资源的原有权利保持不变。
- `assets/fonts/GeForce_Bold.ttf`：从同一官方程序的字节数组资源提取，字体元数据为 “Generated in 2012 by FontLab Studio. Copyright info pending.”。派生的字形图集及度量来自 GDI+ 72 DPI 绘制；资源提取不执行官方程序。
- `assets/fonts/Roboto_Regular_14.ttf`：同一程序内嵌字体，元数据记载 Google 2011；保留参考，当前渲染不加载。
- `assets/fonts/NotoSansSC.ttf`：Google/Adobe Noto Sans SC Regular 实例，SIL Open Font License 1.1；原许可在 `assets/fonts/OFL.txt`。
- `assets/fonts/TuringScreenSans.ttf`：由 Noto 字体裁剪常用简体中文、界面用字和拉丁字符并更名生成，继续使用 SIL OFL 1.1；生成脚本 `scripts/generate-font.py` 仅构建时依赖 fontTools，程序不依赖 Python。
- Windows 包的 Microsoft Visual C++ 运行库来自本机 MSVC 2022 可再分发运行库目录，适用 Microsoft 原有条款。
- Linux 包的 Qt、ICU、double-conversion 等运行库保留构建系统中的许可证，位于 `licenses`。

NRBF 读取、主题模型、采集、渲染和串口均为项目中的 C++ 实现。运行时没有 Rust、Python、WebEngine、LibreHardwareMonitor 或外部采集进程。
