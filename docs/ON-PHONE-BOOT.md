# 不接电脑、在手机上选择进入 iOS

首次安装从 [安装与构建指南](INSTALL.md) 开始。本文包含开发机布局与历史记录，分区尺寸、备份路径和哈希约束不能直接套用到另一台手机。默认存储安装仅保存设置；v1.5 应用数据还需要单独核验并初始化 cust 尾部，见指南第 8 步。

目标：开机默认进 Android；需要时从 Android 切到 iOS 启动一次，iOS 从手机自己的存储加载，不再需要电脑传 2.4 GB；
在 iOS 里重启或关机后回到 Android。

## 当前状态（2026-11-07，v1.5 已安装）

v1.5 / code 6 已通过手机模块启动、桌面、Wi-Fi 自动关联、HTTP/HTTPS 200、退出回到 Android A 和模块复检。
本次从手机读取并校验系统及 payload 用时 5.445 秒。`ios`、`exaid` 和 `boot_b` 写入后均读回匹配；实际只更新了 `boot_b`。
恢复的数据存档为 generation 226，79,651,496 字节；其中保存了 TrollStore、Ibili 及一张真实 Photos 测试资产。
证据：`runtime/validation/20261107/apps-stored/boot02/`、`returned-android.txt` 与 `logs/apps-store-write-20261107.log`。

这版保留 v1.4 显示、音频和蓝牙组件，增加应用安装与数据归档。`cust` 的原文件系统前 8 MiB 保持原样，
16–512 MiB 为只读应用镜像，512–1024 MiB 为两份 256 MiB 存档，用户文件配额 **240 MiB**。
旧 `boot_b[160:192 MiB]` 设置区保留，首次迁移后不再是 v1.5 的保存目标。
模块还会核验 cust 的前 512 MiB，允许后半部分数据正常保存。
B 站可重建的 modmgr 缓存放在 RAM，不挤占文档和照片；后续启动会重新下载。

手机端安装器为 TrollStore 2.1.1。B 站列表可以联网，导航缺字和视频显示仍未解决；实验视频显示默认关闭。
详细安装来源、限制和备份见 [应用和数据适配](APPS-AND-DATA-20261107.md)。
生成当前版本使用：

```bash
python3 scripts/prepare_wifi_root.py --display-bridge build/k40/userland/obj/libredwood-gl.dylib --audio --bluetooth --app-data --bili-adapter
python3 scripts/install_ios_store.py build --app-data --bili-software-decode
```

其中 `--bili-software-decode` 仅请求软件解码偏好，尚不能保证客户端实际采用正确的解码/显示通路。
常驻版不含自动打开应用、自动点击、测试图导入和测试音。

以下为此前已验证的启动机制及 v1.4 历史记录。

已在真机验证：

- iOS 系统卷、内核 payload 和 loader 都已存放在手机上，三处数据都按 SHA-256 读回核对过。
  Android 在 super 中的 6 个分区写入前后哈希一致。
- 在 Android 里执行 `bootctl set-active-boot-slot 1`，再正常重启：ABL 从 B 槽启动 loader，loader 用 5.4 秒从存储加载并校验 2.52 GB，
  iOS 进入桌面，可打开设置，时间正确。全程不接电脑传数据。
- loader 启动时会把 `boot_b` 的剩余尝试次数清零（日志：`slot: boot_b has no tries left`）。

补充验证与未验证项：

- 2026-11-06 已实测从 B 槽 iOS 正常结束内核后，ABL 自动回退 A 槽，Android 启动完成；
  `ro.boot.slot_suffix=_a`、`bootctl get-current-slot=0`、A 槽启动成功，B 槽不可启动。
  `fastboot oem stop` 和 iOS「设置 → 通用 → 关机」两条路径均已实测；后者由 SpringBoard 发起关机，用户确认回到 Android。
  iOS 菜单中的重启操作尚未单独实测。
- 一键切换已提供 KernelSU / ReSukiSU 模块，见下一节。
- 不接 USB 线时的启动：B 槽里的 loader 会启动 USB 设备模式以便电脑抓取日志，未接线时的表现还没测。

## 手机端一键进入 iOS

模块采用 KernelSU 的 [action.sh](https://kernelsu.org/guide/module.html) 入口，不需要后台服务或系统挂载。
本机使用 ReSukiSU 4.2.0-rc1。源码在 `scripts/phone_boot_module/`。

```bash
python3 scripts/build_phone_boot_module.py
adb push runtime/store/k40-ios-boot.zip /data/local/tmp/
adb shell /data/adb/ksud module install /data/local/tmp/k40-ios-boot.zip
```

安装完成后重启一次 Android，使本机管理器应用暂存的模块。
打开 ReSukiSU → 模块 → **K40 · 进入 iOS** → **操作**。
脚本先确认 alioth、root、当前为 Android A 槽且 A 槽已成功启动、无快照合并，再检查：

- 手机上的 `ios`、`exaid` 及 `boot_b` 前 160 MiB 与本地已构建镜像的 SHA-256 一致；
- Android 的 `boot_a` 与本机备份一致；
- 两槽的 26 组配套固件哈希一致。

检查通过才调用 `bootctl set-active-boot-slot 1` 和 `reboot`。镜像变化（包括 OTA）后会拒绝启动，需重新核对安装并生成模块。
更新存储镜像后也应重新生成和安装模块。移除模块不删除 iOS 镜像。
只读检查入口：`sh /data/adb/modules/k40_ios_boot/action.sh --check`。

2026-11-06 已实测模块入口完整启动：约 5.4 秒读取并校验存储中的系统与 payload；
正常结束后回到 Android A 槽。设置存档的两份头部和内容 CRC 均在电脑上读回验证，
再次启动恢复 generation 5 的 73 个文件，Wi-Fi 自动关联、DHCP 成功，随后 HTTP 和 HTTPS 均返回 200。
上一版 v1.3 模块启动再次恢复 generation 25 的 85 个文件，自动连接后 HTTP 约 0.94 秒、HTTPS 约 3.01 秒返回 200；
Wi-Fi 配置变化触发了 generation 26 提前存档。该次桌面截图和日志在 `runtime/validation/20261106/final-save/boot03/`。

v1.4 对应本轮 GPU 缩放、低音量音频及蓝牙组件。系统卷第三段由 48 MiB 后移到 60 MiB，
为 payload 留出 52 MiB；设置区仍为 160–192 MiB。诊断版的自动测试音任务不进入常驻镜像。
本轮三个存储分区均读回匹配；模块启动后恢复 generation 185，Wi-Fi 自动连接，HTTP 约 0.93 秒、HTTPS 约 3.14 秒返回 200，
GPU 缩放、音频 DSP/播放通路及蓝牙 H4 重新就绪。
同版从存储会话正常退出后，Android A 启动完成、启动 HAL 确认槽位 0，设置双槽 CRC 有效；再通过 v1.4 模块进入 iOS。
证据在 `runtime/validation/20261106/feature-stored/boot01/`。

### 设置存档

2026-11-06 增加设置保存：`boot_b` 的 160–192 MiB 预留给两份 16 MiB 的 CRC 存档。
系统卷尾段结束于约 156.75 MiB，不与设置区重叠；loader 校验 `XNUSTORE` 清单及 CRC 后才提供写权限。
内核与 loader 分别检查目标为 `boot_b`、大小 192 MiB、4096 字节块，只允许写块 40960–49151。
Android A 槽、iOS 系统卷、payload 和 loader 都在写入白名单之外。

`var_store` 沿用上游双槽归档，只保留 Keychains、keybags、preferences、wireless、手机/系统偏好、Accounts 和 MobileBluetooth 目录，
不把整个 `/var` 塞进这个小存档。启动时校验并恢复较新的一份，损坏则回退另一份或使用空设置。
保存进程仍在时，正常结束会话会请求保存；另每 60 秒保存一次。
Wi-Fi 配置文件变化稳定 3 秒后会提前保存，不必等到下一分钟。
实测 iOS 菜单关机会先结束保存进程：内核检测到进程已退出时保留最近一次存档，避免空等 20 秒。
在提前保存完成前关机可能丢失刚修改的 Wi-Fi 配置；其他设置在强制断电或进程已退出时可能丢失最后一分钟的变更。
本机原始存档包含凭据，只放在被 git 忽略的 runtime/backups 下。

构建先运行 `bash scripts/build_prefs_store.sh`，再重建 Wi-Fi 根。`install_ios_store.py build` 要求手机处于 Android，
读取并保留最新的 32 MiB 设置区，同时更新时间偏移。模块校验 boot_b 前 160 MiB，允许设置区正常变化。
更新镜像前仍需执行 `check`；镜像更新后重新生成和安装启动模块。

## 约束（2026-11-01 只读核实）

- **Android 只能由小米原 bootloader（ABL）直接启动。** ABL 负责给 keymaster 设置信任根、选择并修补设备树。
  iOS loader 无法反过来启动 Android。ABL 唯一可用于选择系统的是 A/B 槽位。
- **切到 B 槽是安全的。** B 槽 28 个固件分区与 A 槽逐字节相同。非活动槽的分区使用 Qualcomm 的"非活动"类型 GUID（77036cd4-…），
  切槽就是一次完整的固件槽切换，UFS 启动 LUN 也会从 1 换到 2。B 槽在 super 中的分区为空，Android 只在 A 槽。
- **空间。** iOS 系统卷 2,492,203,008 字节，容器已满，几乎不能压缩。`/data` 启用了元数据加密，loader 读不了。
  能用的原始空间是 super 的空闲区（1.99 GB）、全零未使用的 `exaid`（384 MiB），以及 `boot_b` 中 loader 之后的部分。
  三者合计 2.58 GB，需要 2.52 GB。
- **Android 内核的 `baseband_guard`** 拒绝 root 进程直接写分区（super、exaid、boot_b 都被拒，
  日志为 `deny write to protected partition`）。官方的启动 HAL（`bootctl`）不受影响。这个保护保持开启，写入改在 Android 之外完成。

## 实现

### 存放布局

| 位置 | 内容 |
|---|---|
| `ios`：super 中的逻辑分区，`default` 组，1896 MiB，6 段（填入 Android 分区之间的空隙和末尾空闲区） | 系统卷第 1 段 |
| `exaid`（384 MiB） | 系统卷第 2 段 |
| `boot_b` 偏移 0 | loader 启动镜像：已验证的 alioth 镜像，只替换 payload 为 `loader.efi`，加 `store` 条目 `boot_b 400000 usb` |
| `boot_b` 偏移 4 MiB | 清单（`struct store_manifest`，版本 2）：payload 和各段的位置、长度、CRC-32，以及时钟偏移 |
| `boot_b` 偏移 8 MiB | 内核 payload（最大 52 MiB）：GPU 缩放、Wi-Fi、音频与蓝牙内核、`runtime/wifi/root.apfs`，参数同 `run_wifi_probe.py --steps 12 --seconds 0 --display-flip 1 --gpu-scale --sound-level 5 --bt-step 6 --prefs-store`，但不带 `epoch=` |
| `boot_b` 偏移 60 MiB | 系统卷第 3 段（96.75 MiB） |
| `boot_b` 偏移 160 / 176 MiB | 两份设置存档，每份 16 MiB |

### 写入（`scripts/install_ios_store.py`）

- `build`：在 `runtime/store/` 生成三个分区镜像和清单。各段拼接后与 `system.apfs` 的 SHA-256 一致。
- `check`：只读检查：槽位、两槽固件是否一致、是否有待完成的更新，以及三个目标分区的内容
  （出厂备份、全零、本工具写入过、或未知；内容未知时拒绝写入）。
- `partition`：经 fastbootd 创建 `ios`（`fastboot create-logical-partition`）。
- `write`：`exaid` 和 `boot_b` 用 ABL 的 `fastboot flash` 写入。`ios` 由内存启动的 loader 写入：
  ABL 不能刷写逻辑分区；fastbootd 能调整其大小，但打不开它（`Could not open partition`）；Android 把逻辑分区映射为只读。
  具体做法是 `fastboot oem ramdisk`、`fastboot stage ios.img`，再执行 `fastboot oem storewrite ios 0`：
  loader 按 super 的元数据找到各段并写入，再读回比对 CRC。最后回到 Android，按 SHA-256 核对三个分区。
- `uninstall`：用备份恢复 `boot_b`，把 `exaid` 清零，经 fastbootd 删除 `ios`。

### loader（`port/src/hackios-xnu-kernel-main/xnuloader/loader.c`）

- `store_find`、`store_read`、`store_write`：按名字定位分区。GPT 分区直接使用；super 中的逻辑分区通过解析 liblp 元数据找到各段。
  读写都经过 Qualcomm 的 UEFI UFS 驱动。`store_crc32` 使用 CPU 的 CRC 指令，结果与 zlib 相同。
- `store_boot`（payload 中有 `store` 条目时执行）：
  1. 先清零当前槽的尝试次数；
  2. 2 秒内按音量下可回到 Android；
  3. 读取清单，把 payload 放到 `fastboot stage` 会放的位置，把系统卷放到 `oem ramdisk` 会放的常驻区，逐段校验 CRC；
  4. 运行内核。

  以上任何一步失败，或内核结束，都会复位；复位后 ABL 会启动 Android。
- 测试命令：`oem readtest NAME [BYTES]`（读取并报告 CRC 和速度）、`oem storewrite NAME OFFSET`（只允许 `ios`、`exaid`、`boot_b`）。

### 时间

UEFI 的 `get_time()` 在这台手机上不是实时时钟，而是从 2018-12-31 这个固定日期起算。真正一直走的是 PM8150 的 RTC（`rtc@6000`，即 Android 的 `rtc0`），
但 Android 不能设置它，它从电池上次耗尽时开始计数，Android 自己记录它与真实时间的差。

做法：`build` 时从 Android 读取 `date +%s` 与 `/sys/class/rtc/rtc0/since_epoch` 的差，写入清单。
loader 经 PMIC 仲裁器的只读 observer 通道读取 RTC（与内核电池驱动的读法相同），把两者相加，作为 `boot-epoch` 交给内核。

真机读数：PMIC 20045 s，加偏移 1793530800，得到 iOS 时间 2026-11-02 00:34:21，与电脑一致。
电池再次耗尽后 RTC 会归零，这时要重新执行 `build` 和 `write`，否则 iOS 的时间会不对。

### 一次性切槽

- Android 一侧只调用启动 HAL：`bootctl set-active-boot-slot 1`。实测结果：`boot_b` 变为活动、尝试 7 次、未标记成功；
  固件分区的类型 GUID 切到 B 槽；UFS 启动 LUN 从 1 变为 2；没有被 `baseband_guard` 拦截。
- ABL 启动 B 槽时会扣掉一次尝试。loader 随后把 `boot_b` GPT 条目的尝试位（第 51–53 位）清零，两份分区表都改：
  先改主表并读回校验，再改备份表。只有当 `boot_b` 是活动槽、未标记成功、也未标记不可启动时才会修改；
  用 `fastboot boot` 从内存启动时不会动分区表。
  这段 C 代码先在电脑上用本机 sde 两份 GPT 的完整副本测试过：活动态下只改动 `boot_b` 条目的第 54 字节和两个 CRC 字段；
  非活动态不写；重复执行不再写。
- 尝试次数用完后，下次无论以何种方式复位，ABL 都会把 B 槽标为不可启动，并自己切回 A 槽（包括换回各分区的 GUID），
  与一次没装成功的系统更新后的回退相同。

## 风险与撤销

- ROM 的 OTA 会写入非活动槽，覆盖 `boot_b` 中的 loader，之后需要重新安装（`check` 会先确认两槽固件一致、没有待完成的更新）。
  super 的空闲区被 `ios` 占用后，Virtual A/B 的快照会全部放在 `/data`。
- 最坏情况是手机停在 fastboot 界面，用 `fastboot set_active a` 恢复。
- 全部撤销：`python3 scripts/install_ios_store.py uninstall`。

## 第 1 步记录：从存储读取的速度与正确性

真机结果，CRC 均与 Android 下读取同一分区的结果一致：

| 分区 | 字节 | 读取 | CRC-32 |
|---|---|---|---|
| boot_b（GPT） | 201,326,592 | 341 ms（563 MB/s） | 4cdfe88b（与备份一致） |
| exaid（GPT） | 402,653,184 | 373 ms（1027 MB/s） | 51e96494（全零） |
| system_a（super 逻辑分区） | 1,008,525,312 | 1096 ms（876 MB/s） | 51a3fc5e |
| vendor_a（super 逻辑分区） | 1,251,250,176 | 1382 ms（863 MB/s） | 4d0b58dc |
| product_a 前 2.52 GB | 2,520,776,704 | 3295 ms（729 MB/s） | f8b4603c |

同一份日志显示，这台手机上 hypervisor 看门狗的调用都返回 "not supported"，看门狗没有启用。

## 复现

```bash
bash scripts/build_loader.sh
python3 scripts/install_ios_store.py build
python3 scripts/install_ios_store.py check
python3 scripts/install_ios_store.py partition
python3 scripts/install_ios_store.py write
```

`build_loader.sh` 在没有 `lld-link` 时改用 Rust 自带的 LLD。`mk_loader_boot.py` 只替换已验证的 `runtime/boot/alioth-boot.img` 中的 payload，
shim、alioth UEFI 和设备树保持原样。
