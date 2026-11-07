# 安装与构建指南

适用基线：Redmi K40 / alioth，iOS 16.5 / 20F66。最新真机组合为 2026-11-07 的启动模块 v1.5 / code 6。

本仓库提供源码覆盖层和复现工具，**不提供完整 ROM、Apple 系统文件、固件、IPA 或通用刷机包**。先准备系统并做 USB 试启动，确认自己的分区、备份和输入匹配后，再继续手机存储安装。文档依据开发机已有验证整理，尚未在另一台全新环境完成全流程复现。

## 1. 设备与电脑

- 已验证设备：Redmi K40（M2012K11AC），alioth，8 GB RAM。其他 alioth 变体、内存容量和 ROM 的兼容性未验证。
- Bootloader 已解锁，Android 可正常启动，USB 调试已授权；存储安装还要求 **root adbd**，即 `adb shell id -u` 返回 `0`，仅有 `su` 不够。
- 手机端安装要求 Android 位于 A 槽，A 槽已标记启动成功、无待完成 OTA/快照合并，两槽配套固件一致。
- Linux x86_64，Python **3.11+**（使用 `hashlib.file_digest`）、Bash、Git、curl、tar、xz、make、C/C++ 编译器、clang/lld、flex、bison、m4、perl、autoconf、automake、zlib 开发文件、ripgrep，以及 `adb` / `fastboot`。
- 7-Zip 23+，必须支持 APFS。系统提取脚本使用 `7z`；补充账号服务的脚本还使用 `tools/7zip/7zz`，请把同版本的 7zz 放在该路径或创建符号链接。
- 本机内核编译环境是 clang 22.1.8；iOS SDK 和交叉工具链见第 3 步。源码、工具链、提取系统和多套镜像会占数十 GB。

可以先只读采集 Android 元数据：

```bash
python3 scripts/probe_android.py --serial ANDROID_SERIAL
```

`ANDROID_SERIAL` 替换为 `adb devices` 中自己手机的标识。以下会自动切换启动状态的脚本应只连接这一台手机。

## 2. 获取源码与准备本机备份

```bash
git clone https://github.com/zhengziyi0117/redmi-k40-ios.git
cd redmi-k40-ios
mkdir -p logs
python3 scripts/prepare_workspace.py
```

固定提交见 [sources.json](../sources.json)。脚本把 `port/` 覆盖到基线后组装 `build/k40`；已有源码检出若不是指定提交会停止。该步骤会覆盖生成目录中的对应源码，自己的开发修改应先保存在 `port/` 或 Git 中。

手机存储安装前，应保存原始 `boot_a`、`boot_b`、`exaid`、`cust`、分区表和 `super` 逻辑分区元数据，并核对读出的文件长度和 SHA-256。至少保留能够返回原 Android 的启动/固件备份。备份含设备校准和个人数据，留在本地。

现有工具使用目录名 `backups/k40-before-ios-20261101/`，这是开发机的路径约定，**不是让你下载或使用他人的备份**。存储安装器要读取其中的 `boot_b.img`，模块构建还读取 `manifest.json` 中的 `boot_a`；清单格式为：

```json
{
  "partitions": {
    "boot_a": {"size": 201326592, "sha256": "自己备份的完整SHA256"},
    "boot_b": {"size": 201326592, "sha256": "自己备份的完整SHA256"}
  }
}
```

尺寸仅为开发机示例，必须以自己的设备为准；不能靠修改清单使不匹配的布局通过检查。完整存储布局在 [手机端启动说明](ON-PHONE-BOOT.md)。

## 3. 工具链与内核

将以下输入放到项目内：

| 输入 | 目录 |
| --- | --- |
| Theos iPhoneOS16.5 SDK | `tools/theos/sdks/iPhoneOS16.5.sdk` |
| Linux iOS 交叉工具链（cctools ld64、ldid 等） | `tools/theos/toolchain/linux/iphone/` |

本机使用的归档来源：[Theos SDK master-146e41f](https://github.com/theos/sdks/releases/tag/master-146e41f) 中的 `iPhoneOS16.5.sdk.tar.xz`；[iOSToolchain test-210562a](https://github.com/L1ghtmann/llvm-project/releases/tag/test-210562a) 中的 `iOSToolchain-x86_64.tar.xz`。解包后核对目录层级。

从项目根目录执行：

```bash
export THEOS="$PWD/tools/theos"
bash build/k40/xnu-deps/setup.sh --stage1
bash build/k40/xnu/apply.sh
chmod u+x build/k40/xnu-deps/host/bin/{codesign,iig,install,plutil,sw_vers,xcrun,xnu-clang}
bash build/k40/xnu-deps/setup.sh --stage2
bash build/k40/tools/mk-ath11k.sh
bash scripts/build_wifi_kernel.sh
bash scripts/build_loader.sh
```

loader 构建需要 `lld-link`，没有时脚本尝试使用本机 Rust 工具链的 `rust-lld`。内核构建日志写到 `logs/wifi-kernel-rebuild.log`；脚本只在新内核确实链接成功且无编译/链接错误时接受已知的可选 DriverKit 导出检查报错。不要绕过失败检查使用旧内核。

## 4. Apple 系统与 USB 启动输入

使用基线的提取脚本准备 iPhone X 的 iOS 16.5 / 20F66 系统。可设置 `LOCAL_IPSW` 为自己本机的对应 IPSW 绝对路径；不设置时脚本从 Apple CDN 提取所需内容。保留 `rootfs.dmg` 供后续补齐原生服务。

```bash
KEEP=1 bash build/k40/ios165/fetch.sh
PREBUILT=1 SYSTEM_ONLY=1 CHECK=1 bash build/k40/userland/build.sh
python3 scripts/prepare_boot_inputs.py
```

`SYSTEM_ONLY=1` 保留固定基线 payload 中已配对的内核/RAM 根，仅构建缺失的完整系统卷。`CHECK=1` 会用 APFS 检查器及内核 reader 核对生成的系统卷；应看到 clean 和逐项读回匹配。

`prepare_boot_inputs.py` 只操作电脑文件：校验固定基线提交和 payload/启动镜像/UEFI 哈希，从 payload 提取配对的 kernel/root，将完整系统卷和配套 `uefi-alioth.fd` 放到 `runtime/boot/`，生成 1800 秒 USB 会话 payload 及 `manifest.json`。UEFI 文件也供后续打包手机端 loader 使用。不要拿基线目录里其他名字相近的独立内核混用。该工具的 APFS 头部检查不能替代前面的完整 APFS 检查。

若 `runtime/boot` 已存在，工具会拒绝覆盖。现有已核验输入可继续使用；另做准备时用 `--out runtime/boot-new`，核验后再自行切换。默认系统卷路径为 `build/k40/userland/system.apfs`，可用 `--system PATH` 指定本地输入。

## 5. 自己设备的固件与用户态组件

无线、音频与蓝牙必须使用自己的原机文件。现有路径约定：

| 路径（相对项目根） | 文件 |
| --- | --- |
| `backups/k40-before-ios-20261101/firmware/wifi/qca6390/` | `amss20.bin`、`m3.bin`、`regdb.bin`、`bd_k11a.elf` |
| 同一备份下 `firmware/audio/` | 本机 ADSP 的 `adsp.mdt`、`adsp.b00` 等分段文件及原机音频固件 |
| 同一备份下 `firmware/bluetooth/` | `htbtfw20.tlv`、`htnv20.bin` |

这些文件来自本机 Android 的 vendor/固件挂载目录，具体位置随 ROM 不同；保留原名，校验拉取副本。不使用其他机型的 board 文件或扬声器校准。

先生成基础 Wi-Fi 根，从中取得已配套的 Mesa，再编译增强组件：

```bash
bash scripts/build_prefs_store.sh
python3 scripts/prepare_wifi_root.py
bash scripts/build_display_bridge.sh
bash scripts/build_audio_plugin.sh
bash scripts/build_bluetooth_bridge.sh
python3 scripts/prepare_wifi_root.py --display-bridge build/k40/userland/obj/libredwood-gl.dylib --audio --bluetooth
bash build/k40/tools/apfs/check-image.sh runtime/wifi/root.apfs redwood=runtime/wifi/root
```

`prepare_wifi_root.py` 每次重建 `runtime/wifi/root`，不要在该目录保存唯一一份输入。显示 bridge 默认链接第一次生成的 `runtime/wifi/root/lib/libOSMesa.8.dylib`，也可用 `DISPLAY_MESA` 指定自己的配套输入。

## 6. 先做 USB 临时启动

手机在 Android 或原始 fastboot 中，从根目录执行：

```bash
bash scripts/start_ios.sh ANDROID_SERIAL
python3 scripts/run_wifi_probe.py --steps 12 --seconds 0 --display-flip 1 --gpu-scale --sound-level 5 --bt-step 6 --out runtime/validation/manual
python3 scripts/capture_ios.py --out runtime/validation/manual/capture
```

第一条校验输入、机型和解锁状态，用 `fastboot boot` 启动基线并传完整系统卷到 RAM，不安装手机端存储镜像。第二条要求完整系统已驻留且设备枚举为 `xnuloader`，等旧内核 GPU 初始化完成后换成新内核。上述命令未启用设置/应用数据写入；loader 仍会使用已有 `logdump` 写日志。

`--seconds 0` 实际为一周；首次基线会话为 1800 秒，到时返回 loader，画面转圈不再变化。Wi-Fi 密码在 iOS 设置中输入。音频保持当前低音量限制，扬声器听感和蓝牙配对仍需实测。

在 iOS 关机、重启或五指按住屏幕约 1.5 秒会结束会话。USB loader 返回 Android：

```bash
fastboot -s xnuloader reboot
```

## 7. 可选：安装到手机存储

**这一部分沿用开发机布局，不是任意 alioth 通用安装器。** 开发机的 `boot_b` 为 192 MiB、`exaid` 为 384 MiB，在 `super` 空闲区创建 1896 MiB 的 `ios` 逻辑分区；系统卷尾段和 payload 放入 `boot_b`，设置双槽位于其 160–192 MiB。Android userdata 不用于存放 iOS。

`install_ios_store.py` 中的 `super` 总扇区数、布局尺寸、备份路径等仍是固定值。自己的分区尺寸、空闲区、固件及备份未逐项核对一致时，停在 USB 试启动阶段；不要删掉断言或哈希检查强行安装。

只有匹配的布局与输入全部就绪、手机回到 root Android A 后，执行：

```bash
python3 scripts/install_ios_store.py build
python3 scripts/install_ios_store.py check
python3 scripts/install_ios_store.py partition
python3 scripts/install_ios_store.py write
python3 scripts/build_phone_boot_module.py
adb push runtime/store/k40-ios-boot.zip /data/local/tmp/
adb shell /data/adb/ksud module install /data/local/tmp/k40-ios-boot.zip
```

`build` 在电脑生成镜像，同时从 Android 读取现有设置区和 RTC 偏移；`check` 只读核对目标内容。`partition` 修改 `super` 的逻辑分区元数据，`write` 写入 `ios`、`exaid`、`boot_b` 并读回校验。**默认组合使用 boot_b 设置保存，不会初始化 v1.5 的应用数据区。**

在 ReSukiSU 中重启一次 Android 使模块生效，然后进入 **模块 → K40 · 进入 iOS → 操作**。模块先校验本机镜像、A 槽和两槽固件，再切 B 槽启动。模块包绑定本机镜像哈希，不应分发给其他设备。

## 8. 可选：v1.5 应用与照片持久化

v1.5 额外使用 **经核验的 1 GiB cust 尾部**，保留原始前 8 MiB，存放只读应用镜像和两份数据归档；可保存文件总额为 **240 MiB**。双槽是容错副本，不叠加容量。

目前 `prepare_app_data.py` 固定了开发机原始 `cust` 的哈希、文件系统几何、测试 IPA 与 TrollStore 归档哈希；这些输入未分发。**另一台手机不能原样执行，也不能通过替换一个哈希跳过布局核验。** 这是需要先审查并适配自己输入的开发流程。

原始输入路径及哈希在该脚本中，来源见 [应用和数据适配](APPS-AND-DATA-20261107.md) 和 [IPA 评估](BILIBILI-IPA-REVIEW-20261106.md)。还需自行提供 `runtime/research/20261106-app-installer/ldid-arm64`。对应流程为：

```bash
bash scripts/build_app_data.sh
python3 scripts/prepare_app_data.py
python3 scripts/check_app_data_guard.py
python3 scripts/mk_loader_boot.py --loader build/k40/xnuloader/loader.efi --usb -o runtime/app-test/loader.img
python3 scripts/install_app_data.py --serial ANDROID_SERIAL
```

最后一条默认只读检查。**仅首次初始化、确认原始 cust 完整备份及所有检查通过后**，使用同一命令加 `--write`；它会刷入 cust 并从 USB loader 读回校验，结束时停在 loader。回 Android 后才继续构建存储镜像：

```bash
fastboot -s xnuloader reboot
# 等待 Android 启动完成并恢复 root ADB，再执行下面各项
python3 scripts/prepare_wifi_root.py --display-bridge build/k40/userland/obj/libredwood-gl.dylib --audio --bluetooth --app-data --bili-adapter
python3 scripts/install_ios_store.py build --app-data --bili-software-decode
python3 scripts/install_ios_store.py check
python3 scripts/install_ios_store.py write
python3 scripts/build_phone_boot_module.py
```

重新安装上一步生成的启动模块并重启 Android。已经使用的数据区不能再次刷入初始 `cust-app-data.img`；初始化器会拒绝覆盖已变化的 cust。更新正常启动镜像也不应重刷 cust。

`--bili-software-decode` 仅请求解码偏好，不表示视频已修复。默认不启用实验绿屏显示、自动打开/点击、测试图导入或测试音。TrollStore 仅尝试未加密 ARM64 IPA，App Store/FairPlay 和 Android APK 不属于已实现能力。

## 9. 返回 Android、更新与卸载

- 手机存储启动时，loader 的启动窗口内按音量下可返回；正常结束 iOS 后由 ABL 回退 Android A。已验证 iOS 设置中的关机和主机停止路径，菜单重启、完全不接 USB 线仍需单独验证。
- 若停在原始 fastboot，可执行 `fastboot set_active a`，再 `fastboot reboot`，恢复从 Android A 启动。
- OTA 会改非活动槽及配套固件。更新后先重新核验布局/固件及备份、构建并安装，再生成对应模块；不要继续使用旧哈希模块。
- 电池彻底耗尽导致 PMIC RTC 归零后，需要在 Android 中重新 build/write 更新时间偏移。
- 正常保存是周期性 RAM 归档；强制断电可能丢失最近变更，应用数据库同时写入的一致性仍有限制。
- `python3 scripts/install_ios_store.py uninstall` 从本机原备份恢复 boot_b、清零 exaid、删除 ios 逻辑分区；移除启动模块本身不会删除镜像。**此命令不恢复 cust**，v1.5 数据区另需先完整备份，再按原 cust 备份恢复。

当前已验证功能和待修项目见 [README](../README.md)，历史变化见 [更新记录](../CHANGELOG.md)。原始日志与备份含凭据和设备信息，不要上传到 issue。
