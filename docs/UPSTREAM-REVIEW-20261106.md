# 可合入上游评估（2026-11-06）

本轮目标：寻找能让 K40 更接近日常使用的现成实现。检查了作者仓库、固定提交、测试记录和本机代码；下述仓库比较描述作者在其设备上的记录；本机已实施的部分与实测结果另见末尾。

结论：最有价值的供体是 Mi 10T 的 `ios-apollo`，适合逐项移植显示、声音与蓝牙；当前查到的 K40 替代分支仍有明显缺口，没有找到换上后就能正常使用 Apple ID、App Store、相机与通话的完整版本。评估后已开始逐项移植，`sources.json` 的构建基线保持不变，通过本地源码覆盖层集成。

## 仓库与判断

| 项目及核对提交 | 已有进展 | 对 K40 的判断 |
| --- | --- | --- |
| [aTechGeekDev/ios-apollo](https://github.com/aTechGeekDev/ios-apollo/tree/1f75f381e31da96de537653fbac2252d2eb80418)，11 月 5 日 | A650 图形、GPU 缩放、QCA6390 Wi-Fi/蓝牙、扬声器、麦克风和震动有真机记录 | 功能供体首选；需移植到 Redwood 接口，不能直接应用 Sweet 补丁 |
| [enesuzun2002/hakios-apollo](https://github.com/enesuzun2002/hakios-apollo/tree/620b134a74335596e7cf8d824c4d88efe8e85143)，10 月 28 日 | iOS 16.5，A650、声音和麦克风、震动、亮度；声音是前者的来源 | 与当前 16.5 用户态更接近，可追溯音频原实现；板级参数仍须核对 |
| [HakiOS 主线](https://github.com/oosexgang/hakios-xnu-kernel/tree/63fc2b4633bac3af8e57dadab9f5a8d4cb940438)，11 月 6 日 | POCO X5 Pro / A642L 的 Metal 界面有帧率测量 | 图形架构可参考；其平台驱动和面板不是 K40。主线明确不支持 Apple 服务 |
| [UniXNU-alioth](https://github.com/FFT-infra/UniXNU-alioth/tree/f1883782125c9e4c6f5f3324c85ce980737caded) | K40 的 UFS、触控和 Metal 客户端已有推进 | [权威适配记录](https://github.com/FFT-infra/UniXNU-alioth/blob/f1883782125c9e4c6f5f3324c85ce980737caded/porting.toml)仍将网络、音频、电源列为 planned；GPU 存在内存分配失败。整体切换会丢失当前可用功能 |
| [UniXNU-pipa](https://github.com/xVanTuring/UniXNU-pipa/tree/cdb3d62771e31d48d832853ee0c90964346d78da)，10 月 26 日 | 同代 A650 上 Metal 已绘制界面，记录约 40–54 fps | 可参考 A650/Turnip 接口；仍有图形错误、偶发停止提交，且无声音与网络 |
| [IOS-26-Hot](https://github.com/nithinyadavamaraboina-online/IOS-26-Hot/tree/d249939b9c36380afaee1a76cf167aeee8347475)，11 月 1 日 | A640 Metal、应用安装工具、HotAuth 兼容层 | 单独评估用户态组件；SM8150、iOS 26 与当前内核/用户态差异大，不能作为 K40 成品 ROM |

还核对了 MrMoxing/ios-alioth 最新 `3be12165` 及其 API 列出的七个 fork；差异集中在启动、存储、语言、调频和参考代码整合，未找到完整补齐硬件与 Apple 服务的版本。UMI 上游仍是本项目已固定的 `77fa050f`。

## 优先合入的具体代码

### 1. GPU 缩放与显示输出

供体：Apollo 的 `0159-apollo-gpu-scaled-flip-frames.patch` 及配套 CoreAnimation bridge。作者在 boot 90 记录 GPU 缩放已运行；[测试笔记](https://github.com/aTechGeekDev/ios-apollo/blob/1f75f381e31da96de537653fbac2252d2eb80418/NOTES.md)同时保留了性能不足的结果。

当前 K40 已移植 `0156` 的换帧思路，但缩放仍在 CPU 上执行。`0159` 将显示缓冲以 IOSurface 形式交给 GPU，使用 64 字节行对齐，再等待 fence 后换帧。需要一起调整 MiniIOSurface、GPU 内存映射、framebuffer 及用户态 bridge；单贴显示补丁不会生效。现有缓冲隔离和 surface 生命周期修复应保留。

验收：连续滑动与设置列表无新增撕裂；显示帧计数和 fence 正常；没有 GPU fault；相同场景 CPU 缩放时间下降。GPU 缩放不会自动把 K40 面板切换到 120 Hz。

### 2. 扬声器、麦克风与震动

供体：Apollo `0138`、`0139`、`0146`、`0147`，`userland/audio/`；原实现见 [hakios-apollo 的 Mi 10T 设备目录](https://github.com/enesuzun2002/hakios-apollo/tree/620b134a74335596e7cf8d824c4d88efe8e85143/devices/mi-10t)。Apollo boot 93 有录音成功记录。

本机备份 DTB 实际确认：

| K40 器件 | DT 节点 |
| --- | --- |
| 双 CS35L41 功放 | `/soc/i2c@98c000/cs35l41@40`、`cs35l41@41` |
| AW8697 震动芯片 | `/soc/i2c@a8c000/aw8697_haptic@5A` |
| WCD938x codec | `/soc/qcom,msm-audio-apr/qcom,q6core-audio/bolero-cdc/…` |

这使音频供体比跨 SoC 驱动更合适，但尚未证明 GPIO、供电、时钟、DSP 路由及功放参数相同。须读取 K40 自身固件与校准，衔接现有 RPMh 锁、RAM 根文件读取及退出清理，补齐 CoreAudio 插件和 mediaserverd 依赖。先验证 DSP 与功放初始化，再验证播放、录音、反复开关及退出回 Android。

### 3. QCA6390 蓝牙

供体：Apollo `0136`、`0137`、`0141`–`0143`、`0145`，以及用户态 Bluetooth transport。作者 boot 76–78 记录了扫描、配对、A2DP、开关和配对保存。

不能只移植 UART 驱动：还包含 IOKit 属性、H4 transport、BlueTool 对接及固件下载；K40 的 UART、BT_EN、供电、固件版本需独立核对。当前存档白名单也需增加配对数据目录，测试与 Wi-Fi 同时工作。

### 4. Metal 作为独立实验链路

供体：Apollo `0170`–`0186` 的相关 GPU/显示部分、`userland/metal2/`；参考 [Metal2 的依赖和来源](https://github.com/aTechGeekDev/ios-apollo/blob/1f75f381e31da96de537653fbac2252d2eb80418/userland/metal2/README.md)。应按 Vulkan 自测 → Metal bridge 自测 → 单个应用 → 桌面排序验证。

这一组尚不适合替代当前默认路径：Apollo 最新测试记录包含 boot 121/123 的 DeviceLost、boot 124 无帧，最新 boot 125 修复仅构建完成、未记录启动验证。Pipa 也记录了圆角错误与偶发停止提交。[HakiOS 的较高帧率测量](https://github.com/oosexgang/hakios-xnu-kernel/blob/63fc2b4633bac3af8e57dadab9f5a8d4cb940438/README.md)来自 A642L，不能套用为 A650 实测。

UniXNU-alioth 的 README 与详细记录有部分过时解释：例如较早的 IOVA 窗口越界推测已被后续读数纠正。评估以 porting.toml 的较新证据为准，其中仍有 SpringBoard 的 OutOfMemory 和 BO pool 压力，尚未达到稳定日用。

## Apple ID、应用安装与“账号支持”

[HakiOS 主线声明](https://github.com/oosexgang/hakios-xnu-kernel/blob/63fc2b4633bac3af8e57dadab9f5a8d4cb940438/README.md)不支持激活、Apple ID、iCloud 和 App Store。当前真机认证错误也没有因此得到解决；此前补齐守护进程只是恢复服务启动，不应等同于账号支持接近完成。

IOS-26-Hot 的 HotAuth 有可参考的有限功能。[hotauthd 源码](https://github.com/nithinyadavamaraboina-online/IOS-26-Hot/blob/d249939b9c36380afaee1a76cf167aeee8347475/phone/auth/hotauthd.m)实现了应用专用密码的 CardDAV/CalDAV/IMAP 配置；[HotAuth 界面](https://github.com/nithinyadavamaraboina-online/IOS-26-Hot/blob/d249939b9c36380afaee1a76cf167aeee8347475/phone/auth/HotAuth.m)另提供 iCloud 网页入口。源码还包含依赖用户配置服务的 Anisette 桥，但本轮未找到证明其在当前 K40 上完成原生设置登录的测试依据。

因此可单独评估邮件、联系人和日历同步组件，不能承诺合入后设置里的 Apple ID、App Store、推送都会正常。其 HotStore 也是独立安装/目录工具；项目仍列明 FairPlay 加密应用不工作。应用下载、安装、启动、图形和云服务必须分别验证。

## 本轮留下的可复核材料

- 作者 README、提交元数据、七个 Alioth fork 差异、UniXNU 设备清单、K40 适配记录和 HotAuth 源码，保存在 `runtime/research/20261106-upstreams/`。
- 23 个候选补丁/说明文件保存在其 `candidates/apollo/`，`manifest.json` 记录固定提交及每个文件的 SHA-256；原始下载文件保留不动；选定功能通过源码覆盖层移植。
- 通过独立 `refs/remotes/review/main` 获取 Apollo 与 Alioth 更新，原有固定检出与工作区覆盖层保持原状。
- 实施顺序为 GPU 缩放、声音、蓝牙；Metal 继续作为独立实验。完整日常使用仍受到 Apple 服务、应用兼容性及未实现硬件功能限制。

## K40 本机移植验证（2026-11-06）

- GPU 缩放：已适配物理 IOSurface、三缓冲所有权、GPU fence、行跨度与 loader 截图接口。
  `gpu-scale01` 记录超过 3600 次 GPU 缩放、0 次 CPU 回退，常见缩放约 0.5–1.8 ms；用户确认“明显更顺滑，撕裂减少”。
  实际面板仍为约 60 Hz，没有把请求参数 `fb_hz=120` 当作 120 Hz 实测。
- 音频：使用 K40 自身 ADSP 固件，保留区调整为 `0x91700000 + 0x2500000`，功放调整为主扬声器 `0x41`、听筒 `0x40`。
  `audio-dsp01` 验证 DSP/APR 服务，`audio-stream01` 验证 TDM 流约 48 kHz、0 错误/欠载；`audio-tone01` 验证双功放启动、短音流与关闭。
  `audio-bt01` 验证 CoreAudio HAL 打开 `/dev/alioth-audio` 并建立 iOS 播放通路。`audio-bt04` 在补齐 NanoRegistry 后验证 AudioQueueStart=0、144000/144000 帧、Speaker 路由、双功放按非静音启动并在静音后关闭；退出时 3467 个音频周期无发送失败、0 DSP 错误、0 欠载，DSP 与 SMMU 正常清理。实际听感尚待用户确认。
  未使用供体扬声器校准，保护 DSP 入口关闭；当前播放限幅、低增益，未验证麦克风与震动。
- 蓝牙：使用 K40 自身 `htbtfw20.tlv`、`htnv20.bin`。`audio-bt01` 真机确认 ROM 响应、921600 波特率、固件 800 段下载、运行版本 `0x27ec`，
  H4 连接至 native bluetoothd，BlueTool 就绪握手和 iOS 初始化命令完成。16.5 的兼容表来自固定 UMI，H4 表及日志 GOT 重新核对并增加 UUID 校验。
  配对、音频传输和配对跨重启保存仍需实测；存档覆盖 classic 偏好、Keychains 与 LE MobileBluetooth 目录。
- 验证日志仅存放在被忽略的 `runtime/validation/20261106/`，固件和设置原始输入不随源码提交。

本轮存储镜像已写入并读回匹配，模块更新至 v1.4；GPU 已有用户确认，声音听感及蓝牙扫描/配对仍保留为待验证项。
