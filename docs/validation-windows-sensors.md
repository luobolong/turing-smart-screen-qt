# Windows 采集来源验证（2026-11-13）

本次在 Qt 版增加 HWiNFO、AIDA64、LibreHardwareMonitor 三个可选来源，保留系统来源。设置即时生效，配置可恢复，登录启动及轻量运行使用同一来源。Windows 系统来源不再把 `CurrentMhz` 作为实时 CPU 频率。

## 自动化验证

环境：Windows 11、MSVC 2022、Qt 6.8.0 x64 Release。

```powershell
cmake --build build/windows --config Release --parallel 4
ctest --test-dir build/windows -C Release --output-on-failure --timeout 90
```

- core、gui、startup、windows-sensors 四组测试通过。
- Windows 采集专项：8 passed、0 failed、1 skipped（统计包含初始化、清理）。跳过项是创建 `Global\HWiNFO_SENS_SM2` 的隔离生产者测试，当前测试进程没有创建全局映射的权限。
- 已覆盖 HWiNFO 结构边界、稳定原始标签、实时核心最大值及过期时间判断；过期判断使用注入时间独立验证，不依赖全局对象权限。
- 已覆盖 AIDA64 ANSI / UTF-16、相邻 XML 片段、试用占位值、显存单位、多显卡选择；使用真实 Win32 命名映射及模拟数据验证读取和映射关闭后的失效。
- 已覆盖 LHM 数字 / 带单位字符串、GHz / MHz、摄氏 / 华氏、空值、CPU / GPU 隔离、显存换算及百分比风扇排除；使用本机 TCP HTTP 服务及模拟数据验证异步读取、显卡切换和连接失败。
- GUI 测试覆盖四项来源、动态说明、LHM 端口和显卡序号的保存、重新打开及切换立即生效。

上述测试中的硬件数值为模拟数据，不代表此机器的实测温度、频率或功耗。

## 包验证及实机限制

- 便携包清空开发工具及 Qt 开发目录的 PATH 后，`--version` 和内置 Nightfall 主题渲染成功。
- 命令行在 HWiNFO 未开放共享内存时正确返回启动 Sensors / 开启 Shared Memory Support 的原因。
- 命令行在 LHM 本机端口没有 Web Server 时正确返回连接失败，CPU 频率和 GPU 温度缺失，不显示缓存或基准频率。
- HWiNFO 界面自动操作由用户按 Esc 停止；未继续进行 UI 配置。三款监控软件的真实硬件数值联调尚未完成。使用时需按 README 开启对应来源，再对照第三方软件确认数据。
- 本轮未进行 Linux 构建；Linux 现有采集路径未改动，Windows 设置项仅在 Windows 显示。

发布物：`dist/turing-screen-qt-0.2.0-windows-sensors-20261113.zip`。完整解压后运行其中的 `turing-screen-qt.exe`。
