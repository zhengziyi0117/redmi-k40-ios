# 不接电脑、在手机上选择进入 iOS

目标：开机默认进 Android；需要时从 Android 切到 iOS 启动一次，iOS 从手机自己的存储加载，不再需要电脑传 2.4 GB；
在 iOS 里重启或关机后回到 Android。

## 当前状态（2026-11-02）

已在真机验证：

- iOS 系统卷、内核 payload 和 loader 都已存放在手机上，三处数据都按 SHA-256 读回核对过。
  Android 在 super 中的 6 个分区写入前后哈希一致。
- 在 Android 里执行 `bootctl set-active-boot-slot 1`，再正常重启：ABL 从 B 槽启动 loader，loader 用 5.4 秒从存储加载并校验 2.52 GB，
  iOS 进入桌面，可打开设置，时间正确。全程不接电脑传数据。
- loader 启动时会把 `boot_b` 的剩余尝试次数清零（日志：`slot: boot_b has no tries left`）。

尚未验证或尚未实现：

- 从 iOS 重启后由 ABL 自动回退到 A 槽、进入 Android：按 ABL 的 A/B 逻辑应当如此，但还没在真机上走过一遍。
  万一没有回退，可以用 `fastboot set_active a` 恢复。
- 一键切换：Android 里的按钮（KernelSU 模块）还没做。目前需要在 root shell 中执行：
  `bootctl set-active-boot-slot 1 && reboot`。
- 不接 USB 线时的启动：B 槽里的 loader 会启动 USB 设备模式以便电脑抓取日志，未接线时的表现还没测。

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
| `boot_b` 偏移 8 MiB | 内核 payload：Wi-Fi 内核、`runtime/wifi/root.apfs`，参数同 `run_wifi_probe.py --steps 12 --seconds 0`，但不带 `epoch=` |
| `boot_b` 偏移 48 MiB | 系统卷第 3 段（96.75 MiB） |

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
