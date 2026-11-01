# Alioth Wi-Fi 适配记录

记录日期：2026-11-01。测试机为 Redmi K40 / M2012K11AC / alioth / 8 GB，QCA6390。

## 已验证的边界

1. 原机 DTB 核对了 PCIe0 的 BAR、PERST/CLKREQ/WAKE、wlan-en、MSI、SID、PHY 初始化表和六路供电。
2. PCI ID `17cb:1101`，BAR0 `0x60300000`，MHI 冷启动签名正确。
3. BHI/BHIe 下载原机 `amss20.bin`，固件进入 mission mode，QRTR 宣告 WLFW 服务 `0x45`。
4. ath11k 完成 WLFW 握手、板级数据/M3 加载、冷启动校准和固件重启。
5. 首次独立扫描得到 28 个 2.4 GHz 热点。后续 iOS 扫描也收到 5 GHz 热点，包括信道 36、44、56、64、149、157。
6. 用户确认 2.4 GHz 可用。驱动日志显示 WPA2 消息 1/2/3/4、密钥安装成功、`THE NETWORK IS JOINED`，
   并观察到 433 帧发送、326 帧单播接收，发送失败数为 0。这是单次短时样本，不是吞吐测试。
7. 用户确认 5 GHz 热点可见，但点击加入失败。当前没有证据证明 5 GHz 关联成功，也没有确定根因。
8. 退出测试的实际日志确认驱动停止、PCIe 设备进入 reset、SMMU SMR7/context6 恢复。
   关闭固件时出现过 mode-off 超时，随后仍完成硬件 reset；应继续完善关闭流程。

原始日志和屏幕截图仅保存在本机 `runtime/wifi/`，包含附近 SSID、BSSID、IP 等信息，不随源码上传。
不能从“扫描可见”直接推断收发、认证和联网均已工作。

### 5 GHz 加入失败（进行中）

- station02 会话结束时的请求汇总：`set 20 (ASSOCIATE)` 2 次（均为 2.4 GHz），`set 22 (DISASSOCIATE)` 4 次。
  5 GHz 的加入请求没有到达驱动，是 iOS 用户态（wifid/IO80211）放弃的，与射频、认证和 WPA 握手无关。
- 目标 5 GHz 热点位于 DFS 信道（52–64）。驱动在 CN 规则下把这些信道报为 `0x112`（DFS、被动），
  而扫描结果此前固定报 `0x92`（主动、非 DFS），两处不一致。现已改为扫描结果与信道表使用相同标志；
  这只是可能的原因，尚未验证。
- wifid 根据 802.11d 推出 `CN` 后反复设置 COUNTRY_CODE 被拒；现改为接受并记录，射频仍使用固件选择的规则。
- 新增诊断：断开、加入或按名称扫描后的 20 秒内记录全部 Apple80211 请求（带 UTC 时间）；
  扫描摘要打印每个热点的 AKM、RSN capabilities 与 HT/VHT/HE；首次应答时打印发给 iOS 的信道表。
- `python3 scripts/prepare_wifi_root.py --wifid-console-log` 给 wifid 加 `OS_ACTIVITY_DT_MODE`，
  把 WiFiManager 的 os_log 镜像到 `/dev/console`，用来看到它拒绝加入的原因。默认 RAM 根不变。

### 发热与耗电

- 所有已保存的会话日志（包括 `runtime/captures/stable` 稳定基线）里，SpringBoard 每秒约 4000 次
  查找 `com.apple.sleepd.sleepserver` 失败（station03 一次 30 分钟会话约 200 万次）。查找方是
  `Sleep.framework`，失败后立即重试、不退避，SpringBoard 和 launchd 因此各占一个核心。
- 原因：RAM 根只有少量 launchd 任务，没有 stock 的 `com.apple.sleepd`；系统卷里有 sleepd 程序。
  `prepare_wifi_root.py` 现在加入该任务，并去掉 `RunAtLoad`、`KeepAlive` 和 `LaunchEvents`，
  只在有人查找时才启动。重建后的根只多出这一个 plist，`check-image.sh` 读回 129 个条目。
  修复前的镜像备份在 `backups/wifi-root/root-before-sleepd-20261101.apfs`。
- 真机验证（sleepd01，2026-11-01，桌面待机、USB 充电、电量 9%）：约 9 分钟内 sleepd 查找失败 0 次，
  也没有 sleepd 崩溃记录。GPU 旁的 SoC 传感器稳定在 42–45 °C，此前同类会话为 55–62 °C
  （station03 开机约 2 分钟时已到 52.9/56.7 °C）。启动后约 20 秒内 GeneralMapsWidget 有一阵
  geod/navigationService 查找，随后停止，旧会话同样有。未测量交互使用时的温度和耗电。
- 另一原因：原 `cpufreq=1` 把三个 CPU 簇固定在最高频率（1804/2419/3187 MHz），
  内存带宽投票固定为最大值 16383，没有任何降频。现在 `redwood_cpufreq.c`（overlay 新增）在开机
  拉满后交给调频线程：每 20 ms 按各核空闲时间选档；核心忙于 80% 或触摸/按键后 500 ms 内用最高档，
  否则选让最忙核心约 70% 忙的最低档，下降每次最多一半。小核簇不低于 614 MHz（loader 在该簇的一个
  核上不停轮询）。内存：触摸期间或大核平滑负载超过 40% 时投最大票，否则 1 秒后降到 1024；开机第一分钟
  不动。RPMh 槽位 0 加了共享锁，`alioth_wlan_rpmh.c` 也使用。每分钟一行
  `cpufreq: the last 60 s: ...` 记录平均频率和内存高档比例。`cpufreq_pin=1` 恢复旧的固定最高档；
  可调：`cpufreq_ms`、`cpufreq_boost_ms`、`cpufreq_floor0`、`ddr_idle`、`cpufreq_log`。
  修改前的内核备份在 `backups/kernel/mach-before-governor-61a2dc29.bin`。
- 真机验证（gov01/gov02，2026-11-01，电量 7–9%、USB 充电）：未见 panic，Wi-Fi 正常连接。
  桌面待机时三簇平均 615/713/844 MHz。第一版的内存判断用单个 20 ms 采样，合成器每帧几毫秒的 CPU
  就足以触发，内存一直是 100% 高档；改为平滑负载后，轻度使用时内存高档 27–35%，持续操作时 74–88%，
  三簇平均 1.2–1.3/1.7–1.8/2.1–2.3 GHz。操作期间传感器 45–51 °C。用户确认操作手感没有变差。

## 改动

- 新增 `alioth_wlan_*` PCIe/MHI/QRTR/QMI/WLFW 模块，保留原基线的其他硬件驱动。
- 使用固定 Apollo 源码中的 QCA6390 Linux 兼容层，映射至 Redwood API；Linux ath11k 本身保持原样。
- 原来的 AHB QMI/LX/STA/Apple80211 实现在 Alioth 下停用，接入同一套 QCA6390 station 与 iOS ioctl 通路。
- 固件通过 `redwood_image_file("fw", ...)` 从 RAM 根读取，不依赖 Apollo 的设备树固件打包方式。
- 使用 K40 原机的 `bd_k11a.elf` 作为 `board.bin`。供电和 PHY 参数核对的是本机 DTB，不能假定别的机型相同。
- 补齐 BSD 构建对象列表以及 Wi-Fi 初始化调用。
- 在定时返回、主机停止和 panic 返回路径重置无线设备，释放设备 DMA 后再复原 SMMU。
- iOS 没有 macOS 的 kext daemon：在 BSD 启动阶段完成对应 IOKit check-in，解除 configd 等待 registry quiet 的阻塞。
- 在 RAM 根加入 stock configd、mDNSResponder、securityd、keybagd 的 launchd 定义。
  configd 已在 1.9 秒内创建 en0 网络服务，未强行另起第二套 DHCP。
- 采用上游的开关状态保留修正：驱动尚未就绪时也记住 iOS 的电源请求。

## 准备工具链和源码

```bash
python3 scripts/prepare_workspace.py
```

上游检出的固定提交见根目录 `sources.json`。`port/` 是最终源码覆盖层，正常构建无需重跑参考代码提取脚本。
`extract_apollo_wifi.py`、`port_wifi_pcie.py`、`finish_wifi_port.py`、`stage_ath11k.py` 用于记录这次移植过程；
重新从参考生成时，先提取到空目录，再运行完整移植和 finish 脚本。

测试环境为 Linux x86_64、clang 22.1.8、Python 3，使用 cctools ld64 和 Theos iPhoneOS16.5 SDK。
系统需有 git、curl、tar、xz、make、C/C++ 编译器、flex、bison、m4、perl、autoconf 等上游依赖。

本机工具链隔离在 `tools/theos/`，未写入全局 Theos 目录：

- SDK：[Theos SDK release](https://github.com/theos/sdks/releases/tag/master-146e41f)，`iPhoneOS16.5.sdk.tar.xz`。
- Linux 工具链：[iOSToolchain release](https://github.com/L1ghtmann/llvm-project/releases/tag/test-210562a)，`iOSToolchain-x86_64.tar.xz`。
- 目录分别是 `tools/theos/sdks/iPhoneOS16.5.sdk` 与 `tools/theos/toolchain/linux/iphone`。

准备好上述目录后：

```bash
export THEOS="$PWD/tools/theos"
bash build/k40/xnu-deps/setup.sh --stage1
bash build/k40/xnu/apply.sh
chmod u+x build/k40/xnu-deps/host/bin/{codesign,iig,install,plutil,sw_vers,xcrun,xnu-clang}
bash build/k40/xnu-deps/setup.sh --stage2
bash build/k40/tools/mk-ath11k.sh
bash scripts/build_wifi_kernel.sh
```

ath11k 对象使用固定 Linux 7.0.14 和 FreeBSD 15.1 归档，上游构建脚本验证归档 SHA256。
本次编译包含 18 个驱动文件，0 个剩余函数 stub，53 个内核导入名称。
内核配置的可选 DriverKit export 检查会报错；构建脚本仅在没有编译/链接错误且确实生成新内核时接受这一已知情况。

## 系统与固件输入

需要另行准备并核验：

- `runtime/boot/system.apfs`：完整系统 RAM 镜像；本机 2,492,203,008 字节，通过 APFS 校验和 62,356 条目读回。
- `runtime/boot/alioth-boot.img`：配套 UEFI / XNU loader。
- `runtime/boot/root.apfs` 和 `kernel-payload.bin`：从固定基线 `boot/run-payload.bin` 内提取的配套 RAM 根和内核。
  不要以旁边名字相近的独立内核代替 payload 内的内核。
- `runtime/boot/manifest.json`、`try-payload.bin`：启动入口所需的本地校验清单和已核验桌面 payload。
- `backups/k40-before-ios-20261101/firmware/wifi/qca6390/` 中自己的
  `amss20.bin`、`m3.bin`、`regdb.bin`、`bd_k11a.elf`。
- `build/k40/ios165/rootfs`：构建脚本从 Apple 系统输入提取的文件，用于生成 stock 网络服务定义。

基线的 `ios165/fetch.sh`、`userland/build.sh` 和 `boot/inspect_payload.py` 提供系统准备和 payload 检查入口。
此处的镜像配对是本次实际使用的配对，不承诺不同版本或其他人的预编译包可以混用。

```bash
python3 scripts/prepare_wifi_root.py
bash build/k40/tools/apfs/check-image.sh runtime/wifi/root.apfs redwood=runtime/wifi/root
```

当前带网络服务的 RAM 根有 129 个条目（含 sleepd），已通过 APFS 检查及内核 reader 逐项读回。

## 真机运行

手机从 Android / 原 fastboot 启动已核验的桌面基线：

```bash
bash scripts/start_ios.sh ANDROID_SERIAL
```

系统镜像驻留 RAM 后，用新内核测试，下面的命令只支持已经枚举为 `xnuloader` 的设备：

```bash
# 11：驱动启动、扫描、停止；12：保持 en0，允许从 Settings 加入网络
python3 scripts/run_wifi_probe.py --steps 12 --seconds 1800 --out runtime/wifi/manual
python3 scripts/capture_ios.py --out runtime/wifi/manual/capture
```

`aliothwifi=N` 默认关闭，测试脚本显式启用。步骤 1–10 用于硬件和固件诊断，11 是扫描，12 是 iOS station。
脚本自动停止上一内核，核实返回后才下载新 payload；原来的完整系统仍驻留 RAM。
步骤 12 使用 `wifi=1 wifi_quiet=1`，configd 负责网络配置。
测试会话持续 1800 秒，随后返回 loader；返回 Android 用 `fastboot -s xnuloader reboot`。

密码直接在手机 Settings 输入。凭据文件与本机原始日志不应加入版本控制。
