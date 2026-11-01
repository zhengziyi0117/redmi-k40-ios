2026-11-01：本机 Redmi K40（alioth，M2012K11AC，8 GB）已通过 USB 临时启动 iOS 16.5。

已确认的实机结果：

- 原 bootloader 确认 `product: alioth`、`unlocked: yes`。
- XNU loader 正常枚举，2,492,203,008 字节的系统卷已成功传入内存。
- iOS 桌面和天气/日历小组件已显示，截图保存在 `runtime/captures/second/screen.png` 和 `runtime/captures/stable/screen.png`。
- 日志显示 `OpenGL ES 3.2 Mesa 25.0.7; FD650` 及 `the server has an accelerated renderer`，确认 Adreno 650 界面渲染通路已工作。
- 触摸控制器识别为 FocalTech 5652，最新日志累计记录 764 个手指位置、0 次报告错误；用户已实机确认可左右滑动桌面、打开 Settings。操作后的截图和日志在 `runtime/captures/touch-confirmed/`。
- 采集时 loader 状态为 `running 0 0`，未发现 panic 记录。这不代表已完成长期稳定性测试。

这次采用内存启动，原 Android 系统及 userdata 保留。没有执行 flash、erase、解锁或 GPT 修改；loader 会向已有 `logdump` 分区写运行日志。iOS 数据没有配置写入 `darwin`，本次会话的设置不保证跨重启保留。

测试会话都有时限（`start_ios.sh` 1800 秒，`run_wifi_probe.py --seconds N` 为 N 秒）。时间到后内核交还 GPU、回到 loader，
画面停在 backboardd 重启时的转圈图标上不再变化：这是会话结束，不是开机卡住（2026-11-01 一次 900 秒的测试会话即如此）。
日常使用请不限时启动带 Wi-Fi 的内核（`--seconds 0` 实际设为一周，已在真机验证）：

```bash
bash scripts/start_ios.sh ANDROID_SERIAL
python3 scripts/run_wifi_probe.py --steps 12 --seconds 0 --out runtime/wifi/daily
```

第二条会先等正在运行的内核完成 GPU 初始化再停掉它；在其初始化中途停掉，会让下一个内核卡死在 GPU 上。
在 iOS 里关机或重启、五指按住屏幕 1.5 秒，也会结束会话、回到 loader。想返回 Android，可在电脑执行：

```bash
fastboot -s xnuloader reboot
```

该命令已核对 loader 的实现，尚未在本次运行中执行，以便继续体验。也可以长按手机电源键强制重启。

从 Android 再次启动：

```bash
cd redmi-k40-ios-wifi
bash scripts/start_ios.sh ANDROID_SERIAL
```

脚本会校验镜像哈希和完整系统卷、核对目标机型和 BL 状态，再用 `fastboot boot` 临时启动，传输系统需要约 100 秒。这个入口要求手机在 Android 或原始 fastboot，不能在 iOS 正运行时重复执行。当前启动参数的时间种子来自本次准备过程，后续重新打包可更新时间。

抓取当前画面和日志：

```bash
python3 scripts/capture_ios.py --out runtime/captures/manual
```

构建与来源：

- Git 检出：`upstream/ios-alioth`，固定基线 `9870135bc32de51f64e2fc62f498b2afb654357a`。
- 使用随仓 payload 内部的配套 kernel/root；它的 kernel 与旁边 `kernel-ios.bin` 不同，未混用两者。四份 GPU 固件已与本机 Android 的对应文件逐字节比较，全部相同。
- Apple iPhone X iOS 16.5 / 20F66 系统输入由 `ios165/fetch.sh` 从 Apple CDN 提取。
- 上游 `userland/build.sh` 的预编译模式会复制仓库未附带的测试程序，导致失败；已添加 `SYSTEM_ONLY=1`，只生成缺失的系统卷，保留配套 payload 的根镜像。
- 系统卷通过 `apfsck -cw`，内核 APFS reader 逐项读回 62,356 个条目，与构建目录一致。日志在 `logs/build-system.log`。
- 当前镜像及启动参数的哈希清单在 `runtime/boot/manifest.json`；首次启动日志在 `logs/first-ios-boot.log`。
- 已核验 14 个原始分区备份，共 610,926,592 字节，在 `backups/k40-before-ios-20261101/`，附带本机原 DTB 和 GPU/Wi-Fi 固件。该目录含设备专属校准资料，应留在本地。

Wi-Fi 后续适配已取得真机结果：QCA6390 的 PCIe/MHI、原机固件、WLFW 和 ath11k 校准/扫描均成功；
iOS 已识别 en0，用户确认 2.4 GHz 可用，日志确认 WPA2 握手、密钥安装和实际收发包。
5 GHz 热点可见，但用户尝试加入失败，尚未解决。适配代码、网络服务和复现命令见 `docs/WIFI-PORT.md`；
原桌面启动入口仍使用 `runtime/boot` 的稳定基线，Wi-Fi 新内核通过 `scripts/run_wifi_probe.py` 单独运行。
音频、蜂窝、相机、完整休眠和长期功耗仍未验证。
