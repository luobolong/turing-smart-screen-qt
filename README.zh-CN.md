# Turing Screen

[English](README.md) | 简体中文

一款轻量的 USB 副屏工具，支持 Windows 和 Linux，可预览、编辑主题并显示电脑状态。

适用于 **35inchNEW / Rev A，320×480 或 480×320** 的设备，其他型号不保证兼容。

## 开始使用

- **Windows**：运行安装包，或完整解压便携包后打开 `turing-screen-qt.exe`。
- **Linux**：解压发布包后运行 `./run.sh`。如果无法连接设备，请检查当前用户是否有串口访问权限。
- **Nix / NixOS**：在源码目录运行 `nix run path:.`。

便携包中的 `assets`、运行库和插件需要与程序一起保留。

1. 在“主题预览”中选择主题。
2. 插入 USB 副屏，在右侧选择串口并点击“连接设备”。
3. 点击“应用到副屏”开始显示。方向、亮度和镜像也在右侧调整。

未连接副屏时，也可以预览和编辑主题。

## 常用功能

- **主题**：支持官方 `.data` 和 JSON 主题。双击主题进入编辑器，可调整文字、图片、图层、进度条和指标绑定，支持撤销、重做和导出 PNG。官方主题修改后保存为 JSON。
- **缩放**：预览和编辑器右下角提供适应窗口、1:1、滑动条和百分比输入，也可用 Ctrl + 滚轮缩放。
- **采集数据**：默认每秒更新，可在设置中调整间隔。关闭“采集数据”会暂停更新并保留最后一帧；设置、设备面板和托盘菜单中的开关同步。
- **轻量模式**：释放编辑界面和静态绘图缓存，继续更新已连接的副屏。点击托盘图标恢复窗口；托盘菜单中的“退出”结束程序。
- **天气**：在设置中启用并填写城市，留空按 IP 定位。自动每 10 分钟更新，也可点击“立即获取天气信息”；按钮旁显示结果或错误。天气需要联网并开启数据采集。
- **外观与启动**：设置中可切换中文 / English、界面配色、关闭到托盘及登录自启动。自启动会进入轻量模式，连接上次使用的串口并开始发送。

## 传感器

默认可显示 CPU 使用率、内存、网速、磁盘空间、音量和时间。无法获取的指标显示 `--`，可在“传感器”页查看原因。

Windows 的温度、实时 CPU 频率和 GPU 数据需要第三方监控软件。在“设置 → Windows 传感器”中选择对应来源：

| 来源 | 配置方法 |
| --- | --- |
| HWiNFO | 运行 Sensors，开启 `Shared Memory Support`。 |
| AIDA64 | 在“硬件监控 → 外部程序”中启用共享内存，并勾选所需指标。 |
| LibreHardwareMonitor | 开启 `Options → Remote Web Server → Run`，端口与本程序一致，默认 `8085`；关闭 Web Server 身份验证。 |

第三方软件需单独安装并保持运行，能否读取具体指标取决于硬件、驱动和所选来源。

Linux 直接读取系统驱动提供的 CPU、磁盘和 AMD GPU 指标；NVIDIA / Intel GPU 采集暂未实现。音量采集需要 PulseAudio 或 PipeWire-Pulse。

## 文件与备份

便携版主题通常保存在 `assets/themes`：`official` 为官方主题，`custom` 为个人主题，`deleted` 为回收站。目录不可写时，程序会使用用户数据目录。

更换便携包时，请保留自己的主题文件。设置中的“打开数据目录”可找到 `config.json`；备份设置时保存此文件。

## 从源码编译

需要 CMake ≥ 3.21、C++17 编译器，以及 Qt ≥ 6.4 的 Core、Gui、Widgets、Network、Svg 和 Test 模块。Linux 另需 Ninja、pkg-config 和 libpulse 开发文件。

Windows 使用 Visual Studio 2022 / MSVC：

```powershell
./scripts/build-windows.ps1 -QtRoot C:/Qt/6.8.0/msvc2022_64
```

Linux：

```sh
./scripts/build-linux.sh
```

构建脚本会同时运行测试。Nix 用户也可运行 `nix build path:.`。打包脚本位于 `scripts`，命令行参数可用 `--help` 查看。

GitHub Actions 的 `Build, test and package` 工作流会在 push 或手动运行时生成 Windows x64 便携包和安装包。打开工作流运行页面，在 **Artifacts** 中下载 `turing-screen-qt-<版本号>-windows-x64-portable.zip`，完整解压后运行 `turing-screen-qt.exe`。便携包包含 Qt、MSVC 运行库、插件和资源文件，产物保留 30 天；Pull Request 只执行构建与测试。

串口报文见 [协议说明](docs/protocol.zh-CN.md)。源码采用 [GPL-3.0-or-later](LICENSE)，字体、主题等第三方资源说明见 [第三方资源说明](THIRD_PARTY.zh-CN.md)。
