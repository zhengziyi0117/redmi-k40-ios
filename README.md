# Redmi K40 iOS

在 **Redmi K40（alioth / Snapdragon 870 / Adreno 650 / QCA6390）** 上运行 iOS 16.5 的适配实验。仓库包含内核与用户态源码覆盖层、构建脚本、手机端启动模块和真机验证记录；以 [MrMoxing/ios-alioth](https://github.com/MrMoxing/ios-alioth) 为基线，移植 Apollo / UMI 的相关实现。

**最新进度：2026-11-07，手机端启动模块 v1.5 / versionCode 6。** 已从手机存储进入 iOS 桌面，恢复应用、照片和 Wi-Fi，HTTP/HTTPS 返回 200，正常结束后回到 Android A 槽。

**这是源码实验项目，目前没有可直接下载刷入的完整 ROM。** Apple 系统文件、原机固件、设备备份、IPA、工具链和生成镜像不随仓库分发，需要自行准备。安装脚本仍包含开发机的分区尺寸、路径和校验约束；同为 alioth 也必须核对自己的分区布局。

## 怎么安装

完整步骤见 **[安装与构建指南](docs/INSTALL.md)**，包括环境、设备备份、系统输入、构建、USB 试启动、手机存储安装、启动模块和恢复方法。

```bash
git clone https://github.com/zhengziyi0117/redmi-k40-ios.git
cd redmi-k40-ios
python3 scripts/prepare_workspace.py
```

这一步只下载固定源码并组装 `build/k40`，不会安装 iOS。随后按指南准备工具链、Apple 系统和自己的固件。

| 你要做什么 | 入口 |
| --- | --- |
| 第一次从源码准备和试启动 | [安装与构建指南](docs/INSTALL.md) |
| 已安装后，在 Android 中一键进入 iOS | ReSukiSU → 模块 → **K40 · 进入 iOS** → **操作** |
| 了解手机存储布局、切槽和回到 Android | [手机端启动说明](docs/ON-PHONE-BOOT.md) |
| 安装 IPA、照片保存与容量限制 | [应用和数据适配](docs/APPS-AND-DATA-20261107.md) |
| 查看版本变化 | [更新记录](CHANGELOG.md) |

## 当前能用到哪里

以下为本机真机记录，区分了界面/日志验证与仍需实测的功能。

| 功能 | 当前状态 |
| --- | --- |
| iOS 桌面、设置、触摸 | 已启动；用户确认可滑动桌面、打开设置 |
| GPU 与显示 | Adreno 650 加速、GPU 缩放、硬件三缓冲换帧、2 ms 触摸轮询已接入；用户确认更顺滑、撕裂减少。实际面板约 **60 Hz**，真正 120 Hz 尚未实现 |
| 2.4 GHz Wi-Fi | 已连接；WPA2 握手、密钥安装、实际收发包、重启后自动连接均已验证 |
| 5 GHz Wi-Fi | 能扫描到热点，加入失败，待修复 |
| 应用联网 | Safari 已打开网页；直接 IP、DNS、HTTP、HTTPS 探测通过 |
| 手机端启动 | ReSukiSU 模块从 B 槽加载本机存储；正常结束后回到 Android A，已验证。完全拔线启动尚未单独验证 |
| 原生 IPA 安装 | TrollStore 2.1.1 界面已打开；小型 Ibili 经 helper 签名、安装、注册，启动已接受。其他 IPA 兼容性需逐个验证 |
| 照片 | 真实 PhotoKit 导入、Photos 界面显示、重启后保存均已验证 |
| 文件 | Recents 正常显示空状态；完整 Browse 尚未验证 |
| 设置和应用数据 | `cust` 尾部双槽存档，持久化文件配额 **240 MiB**；部分缓存使用 RAM。不是 Android 剩余空间共享 |
| B 站修改版 | 列表联网可用；导航缺字、视频黑屏仍未修复，实验 CPU 显示出现绿屏。不是可正常播放视频的完成版 |
| 扬声器 | DSP、TDM、CoreAudio、三秒应用播放与 Speaker 路由日志通过；HAL 无丢帧。实际听感待确认，保持低音量限制 |
| 蓝牙 | 原机固件启动、H4 与 iOS 初始化通过；搜索、配对、音频传输及配对保存尚待实测 |
| 麦克风、震动 | 尚未验证，默认关闭 |
| Apple ID / iCloud / App Store | 认证初始化仍失败，尚未支持完成登录与商店使用 |
| 蜂窝、通话、相机、完整休眠 | 尚未验证可用 |
| 长期稳定性与功耗 | 有调频与内存带宽控制，尚未完成长期测试 |

Android A 槽和 userdata 在本机安装中保留；手机端安装会使用 `super` 的空闲区、`exaid`、`boot_b`，v1.5 应用数据另使用经核验的 `cust` 尾部。系统卷仍加载到 RAM。具体写入范围、OTA 影响和撤销方法见安装指南。

## 仓库结构

- `port/`：相对固定 alioth 基线的最终源码覆盖层，含 Wi-Fi、显示、音频、蓝牙、数据存档与安装器适配。
- `scripts/prepare_workspace.py`：下载固定提交、应用覆盖层并组装构建目录。
- `scripts/prepare_boot_inputs.py`：从固定基线 payload 和本地系统卷准备 USB 启动输入及 SHA-256 清单，只操作电脑文件。
- `scripts/build_*.sh`：内核、loader、GPU bridge、音频、蓝牙和数据工具构建。
- `scripts/start_ios.sh`、`run_wifi_probe.py`、`capture_ios.py`：USB 临时启动、内核迭代和画面/日志采集。
- `scripts/install_ios_store.py`、`phone_boot_module/`：手机端存储安装与一键启动模块。
- `scripts/prepare_app_data.py`、`install_app_data.py`：开发机特定的 `cust` 应用/数据区初始化工具，保留原始备份校验约束。

详细实现和历史记录：[Wi-Fi 与硬件适配](docs/WIFI-PORT.md)、[本机运行记录](RUNNING.md)、[上游功能评估](docs/UPSTREAM-REVIEW-20261106.md)、[B 站 IPA 评估](docs/BILIBILI-IPA-REVIEW-20261106.md)。文档中的 `runtime/`、`logs/`、`backups/` 是开发机本地证据路径，不是仓库提供的下载内容。

## 来源与许可证

构建基线提交固定在 [sources.json](sources.json)。本项目整合多个上游，不为整个仓库声明单一许可证；各部分的来源、APSL / MIT / GPL 等声明见 [LICENSES.md](LICENSES.md) 和源文件头部。
