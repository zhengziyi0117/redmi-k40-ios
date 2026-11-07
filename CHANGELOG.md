# 更新记录

日期为本机适配与验证日期。v1.x 表示手机端启动模块/镜像组合，不表示完整 iOS ROM 的稳定版本。

## 2026-11-07 · v1.5 / versionCode 6

- 增加 TrollStore 2.1.1 兼容层；小型原生 Ibili 完成 helper 签名、安装、注册，启动已接受。
- 应用和数据放入经核验的 `cust` 尾部；两份 256 MiB 归档交替保存，用户文件配额为 240 MiB。
- 补齐 Photos / FileProvider 等服务；真实 PhotoKit 测试资产在 Photos 可见且跨重启保留；Files Recents 不再崩溃，Browse 待验证。
- B 站修改版原生列表联网可用，部分缓存迁到 RAM；导航缺字和视频黑屏未修复，实验 CPU 视频显示仍为绿屏，默认不启用。
- 手机模块启动恢复应用、照片及 Wi-Fi，HTTP/HTTPS 返回 200；正常结束后回到 Android A，模块复检通过。
- 整理公开源码及安装文档，补充电脑端启动输入准备工具；数据安装脚本改为显式指定 `--serial`。

详见 [应用和数据适配](docs/APPS-AND-DATA-20261107.md)。

## 2026-11-06 · v1.4

- 接入 Adreno 650 GPU 缩放、物理 IOSurface、硬件三缓冲换帧及 2 ms 触摸轮询；用户确认更顺滑、撕裂减少。
- 实际面板计数约 60 Hz，未实现真正 120 Hz。
- 接入 K40 自身 ADSP / CS35L41 音频通路及 CoreAudio HAL；三秒 AudioQueue 播放、Speaker 路由、无丢帧和退出清理日志通过，听感待确认。
- 接入 QCA6390 蓝牙固件、H4 transport 与 iOS 16.5 bridge；初始化通过，扫描/配对/A2DP 待验证。
- 手机存储镜像读回一致，模块一键启动、Wi-Fi 恢复、HTTP/HTTPS 与返回 Android A 通过。

详见 [上游评估与本机验证](docs/UPSTREAM-REVIEW-20261106.md)。

## 2026-11-06 · 设置保存与一键启动

- `boot_b` 160–192 MiB 保存双槽设置归档，支持网络凭据跨重启恢复与自动重连。
- KernelSU / ReSukiSU 模块校验本机镜像及两槽固件后一次性切到 B 槽，结束后回到 Android A。

## 2026-11-01 · 首次启动与 Wi-Fi

- USB RAM 启动 iOS 16.5 桌面；Adreno 650 图形加速与 FocalTech 触摸通过真机验证。
- 完成 QCA6390 PCIe/MHI、WLFW、固件和 ath11k 扫描通路；2.4 GHz 联网通过，5 GHz 扫描可见但加入失败。
- 加入 CPU 调频和内存带宽控制；保留 USB loader 诊断入口。

当前完整功能状态以 [README](README.md) 为准。
