# Turing Screen Qt

Qt 6 Widgets / C++17 原生跨平台 USB 副屏软件，版本 0.2.0。独立于原 Rust、Python 项目；运行时不依赖它们。目标设备为 35inchNEW / Rev A 的 320×480 副屏。

本版优先降低常驻内存和 CPU。默认每 2 秒采集、渲染一次；隐藏窗口时停止预览绘制，串口工作线程空闲时阻塞等待，只保留最新待发送帧，并仅发送变化矩形。

## 使用

- Windows：完整解压 Windows 压缩包，运行 `turing-screen-qt.exe`。旁边的 DLL、plugins、assets 必须保留。
- Linux：解压后运行 `./run.sh`。预编译包采用 Arch Linux / glibc 2.44 构建；其他发行版建议从源码编译。桌面需提供 X11 或 Wayland 及相应图形系统库。
- 首次自动跟随系统语言；“设置”可选中文 / English、四种配色、采样间隔、分区及关闭到托盘。
- 未连接硬件也能预览、编辑和采集本机数据。连接串口后点击“应用到副屏”开始发送。
- 可拖动原生标题栏、拖动边框缩放、最大化。状态栏或托盘的“退出”结束进程。“关闭到托盘”需要桌面提供托盘服务；没有托盘时关闭窗口正常退出。

## 功能

- 单一“主题”入口，预览 / 编辑切换；主题库缩略图、搜索和尺寸；传感器、设置、诊断独立页面。
- 导入 36 个随附官方 `.data` 主题及此前 Rust 版本的 JSON；保存为 Qt JSON，导出 PNG。
- 文字、传感器、图片、矩形、椭圆、直线、状态条、弧形条、分组；CPU / GPU / RAM / 网络 / 时钟 / 磁盘组件模板。
- 树状分组、重命名、嵌套拖拽、可见 / 锁定、复制 / 粘贴 / 复制图层、编组 / 解组、置顶 / 置底、框选和 Ctrl 多选。
- 画布移动、右下角缩放、方向键微调、Shift 加速、网格、吸附、适应窗口 / 1:1 / 缩放。
- 位置、尺寸、文字、指标绑定、字号、颜色属性；撤销 / 重做（最多 30 步，当前文档历史约 8 MiB 预算）。
- 原生串口、方向、镜像、亮度、清屏、休眠、唤醒、复位，连接 / 采集 / 发送三种独立状态。
- CPU 使用率 / 频率 / 型号、内存、分区、网络速率、日期时间；Linux 在可用时读取 CPU 温度。

精简范围：GPU 深度传感器、Windows 温度助手、音量、天气、在线服务、自动启动、GIF 动画、Photoshop 高级图像处理未实现。旧主题保留指标绑定，无法采集的值显示 `--`。提供基本图层编辑。官方 `.data` 只读导入；修改保存为独立 JSON。

## 快捷键

| 操作 | 快捷键 |
|---|---|
| 新建 / 导入 / 保存 / 另存为 | Ctrl+N / Ctrl+O / Ctrl+S / Ctrl+Shift+S |
| 撤销 / 重做 | Ctrl+Z / Ctrl+Y（跟随平台习惯） |
| 重命名 | F2 或树状列表双击 |
| 复制 / 剪切 / 粘贴 | Ctrl+C / Ctrl+X / Ctrl+V |
| 复制图层 / 编组 / 解组 | Ctrl+J / Ctrl+G / Ctrl+Shift+G |
| 新建文字图层 | Ctrl+Shift+N |
| 微调 / 快速微调 | 方向键 / Shift+方向键 |
| 缩放 | Ctrl+滚轮 |

## 编译

需要 CMake ≥3.21、C++17 和 Qt ≥6.4 的 Core、Gui、Widgets；测试另需 Qt Test。串口使用 Win32 / POSIX，不依赖 QtSerialPort。

```powershell
./scripts/build-windows.ps1 -QtRoot C:/Qt/6.8.0/msvc2022_64
./scripts/package-windows.ps1 -QtRoot C:/Qt/6.8.0/msvc2022_64
```

```sh
./scripts/build-linux.sh
./scripts/package-linux.sh
```

本机验证使用 Windows 11 / Qt 6.8.0 / MSVC 2022 和 WSLg Arch Linux / Qt 6.11.1 / GCC 16。Windows 默认使用 GDI 字体后端，减少本环境中 DirectWrite 首次绘制的内存开销；仍可用 `QT_QPA_PLATFORM` 显式覆盖。

## 验证与数据

`scripts/verify-windows.ps1`、`scripts/verify-linux.sh` 启动原生窗口，采样 RSS 峰值和 CPU 时间，保存七种界面截图。加 `-Stress`（Linux 第三个参数 `--stress-test`）测试主题切换、图层创建、撤销、页面切换、托盘和退出。MB 为 1,000,000 字节，包含共享库常驻页；CPU 报告包含单核比例及按逻辑核数归一化的整机比例。资源采样计时器只在测试模式启用。

具体结果见 [验证记录](docs/validation.md)。100 MB 是随附主题和基本工作流的实测目标，并非任意巨大图片、无限文档或系统字体后端的硬性上限。未连接真实副屏时仅校验协议字节、RGB565、差分矩形及软件流程；硬件发送仍需实机验证。

数据使用 Qt `AppLocalDataLocation`（Windows 通常为 `%LOCALAPPDATA%/TuringScreen/TuringScreenQt`，Linux 通常为 `~/.local/share/TuringScreen/TuringScreenQt`）；可用 `--data-dir` 指定目录。源码许可 GPL-3.0-or-later，资源见 [THIRD_PARTY.md](THIRD_PARTY.md)。
