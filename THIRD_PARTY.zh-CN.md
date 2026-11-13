# 第三方资源说明

[English](THIRD_PARTY.md) | 简体中文

- **Qt**：Windows 使用 Qt 6.8.0，Linux 预编译包使用 Qt 6.11.1；动态链接 Core、Gui、Widgets、Network 及平台 / 图片 / TLS 插件，按 Qt 提供的 GPL v3 / LGPL v3 条款分发。项目自身 GPL-3.0-or-later。许可证副本在发布包 `licenses`；未修改 Qt。对应官方源代码：<https://download.qt.io/archive/qt/6.8/6.8.0/single/>、<https://download.qt.io/archive/qt/6.11/6.11.1/single/>。
- **libpulse**：Linux 音量使用 PulseAudio 原生客户端库（也兼容 PipeWire-Pulse），LGPL-2.1-or-later；<https://www.freedesktop.org/wiki/Software/PulseAudio/>。
- **界面配色**：`assets/ui-themes/*.json` 将 [Catppuccin](https://github.com/catppuccin/palette)、[Dracula / Alucard](https://github.com/dracula/dracula-theme)、[Nord](https://github.com/nordtheme/nord)、[Solarized](https://github.com/altercation/solarized)、[Gruvbox](https://github.com/morhetz/gruvbox) 的公开颜色值适配到 Qt 角色。原项目均声明 MIT（Gruvbox README 标为 MIT/X11）；原许可及出处声明保留在 `assets/ui-themes/licenses/`。未复制其编辑器或终端主题实现。
- **Open-Meteo**：可选天气数据及城市搜索服务，数据按 CC BY 4.0 使用，界面提供来源链接；<https://open-meteo.com/>。IP 粗略定位来自 <https://ipwhois.io/>，仅在启用天气且未填写城市时访问。
- `assets/themes/official`：来自用户本地 35inchNEW 软件包的主题，背景和状态条图像已内嵌在主题文件中，用于兼容实现与本地验证；这些资源的原有权利保持不变。
- `assets/fonts/GeForce_Bold.ttf`：从同一官方程序的字节数组资源提取，字体元数据为 “Generated in 2012 by FontLab Studio. Copyright info pending.”。派生的字形图集及度量来自 GDI+ 72 DPI 绘制；资源提取不执行官方程序。
- `assets/fonts/Roboto_Regular_14.ttf`：同一程序内嵌字体，元数据记载 Google 2011；保留参考，当前渲染不加载。
- `assets/fonts/NotoSansSC.ttf`：Google/Adobe Noto Sans SC Regular 实例，SIL Open Font License 1.1；原许可在 `assets/fonts/OFL.txt`。
- `assets/fonts/TuringScreenSans.ttf`：由 Noto 字体裁剪常用简体中文、界面用字和拉丁字符并更名生成，继续使用 SIL OFL 1.1；生成脚本 `scripts/generate-font.py` 仅构建时依赖 fontTools，程序不依赖 Python。
- `assets/fonts/TuringScreenSymbols.ttf`：约 8 KB 的 Unicode 图标字体，含硬件、网络、系统和天气图标，从 [JetBrainsMono Nerd Font 3.5.0](https://github.com/ryanoasis/nerd-fonts) 裁剪并更名。字体表 / 空格保留 JetBrains Mono 和 Nerd Fonts 的 SIL OFL 1.1；图标轮廓来自 [Material Design Icons / Pictogrammers](https://github.com/Templarian/MaterialDesign-Webfont)，Apache-2.0。许可和来源说明位于 `TuringScreenSymbols-OFL.txt`、`MaterialDesign-APACHE-2.0.txt`、`TuringScreenSymbols-NOTICE.txt`。生成脚本为 `scripts/generate-symbol-font.py`。
- NixOS 图标字形来自 [Font Logos](https://github.com/lukas-w/font-logos)，Unlicense，文本保留在 `assets/fonts/FontLogos-UNLICENSE.txt`。
- `assets/fonts/TuringScreenLatin-Regular.ttf` / `TuringScreenLatin-SemiBold.ttf`：Nightfall 使用的 Inter 4.1 拉丁字母子集，更名为 Turing Screen Latin，保留 SIL OFL 1.1；许可在 `TuringScreenLatin-OFL.txt`。原始项目：<https://github.com/rsms/inter>，生成脚本：`scripts/generate-latin-font.py`。
- `assets/fonts/TuringScreenMono.ttf`：时钟使用的 JetBrains Mono 2.304 Bold ASCII 子集，从 JetBrainsMono Nerd Font 3.5.0 生成，更名为 Turing Screen Mono；SIL OFL 1.1，许可在 `TuringScreenMono-OFL.txt`，生成脚本为 `scripts/generate-mono-font.py`。
- Windows 包的 Microsoft Visual C++ 运行库来自本机 MSVC 2022 可再分发运行库目录，适用 Microsoft 原有条款。
- Linux 包的 Qt、ICU、double-conversion 等运行库保留构建系统中的许可证，位于 `licenses`。

NRBF 读取、主题模型、采集、渲染和串口均为项目中的 C++ 实现。运行时没有 Rust、Python 或 WebEngine。Windows 可选读取用户运行的 HWiNFO / AIDA64 / LibreHardwareMonitor，不随程序分发其软件或驱动，也不启动采集助手进程。

- HWiNFO 共享内存布局参考 [HWiNFO Shared Memory Interface](https://www.hwinfo.com/forum/forums/shared-memory-interface.13/)。HWiNFO 由其权利人单独授权；免费版共享内存连续运行限制见[官方说明](https://www.hwinfo.com/licenses/)。
- AIDA64 共享内存参考其[公开接口说明](https://www.aida64.com/user-manual/hardware-monitoring/external-applications?language_content_entity=en)。AIDA64 由 FinalWire 单独授权。
- LibreHardwareMonitor 本机 Web 接口参考其 [HttpServer 实现](https://github.com/LibreHardwareMonitor/LibreHardwareMonitor/blob/master/LibreHardwareMonitor.Windows.Forms/Utilities/HttpServer.cs)。仅实现 JSON 接口客户端，未复制或捆绑其库；原项目使用 MPL-2.0。
