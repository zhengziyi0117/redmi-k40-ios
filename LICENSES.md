# 来源与许可证

本项目包含多种许可证下的源码覆盖层，各文件保留原有声明。

- Alioth 基线：[MrMoxing/ios-alioth](https://github.com/MrMoxing/ios-alioth)，提交见 `sources.json`。
- PCIe/MHI、Wi-Fi 整合：[aTechGeekDev/ios-apollo](https://github.com/aTechGeekDev/ios-apollo)，
  使用其 `xnu/patches/0126` 至 `0135` 中选定的 Wi-Fi 文件，并适配到 Alioth 的 Redwood API、固件文件系统和生命周期。
- 原始 SM8250/QCA6390 实现：[JopangJovan/hackios-xnu-kernel-umi](https://github.com/JopangJovan/hackios-xnu-kernel-umi)，
  以及 [oosexgang/hackios-xnu-kernel](https://github.com/oosexgang/hackios-xnu-kernel)。相关源码沿用 APSL 2.0。
- 内核覆盖层的 APSL 2.0 全文在 `port/src/hackios-xnu-kernel-main/LICENSE`。
- `wifi/lx` 兼容层沿用 HackiOS 的 MIT 许可证，全文在
  `port/src/hackios-xnu-kernel-main/wifi/LICENSE.HackiOS`。
- Alioth 显示换帧基于同一固定 Apollo 源码的 `0156-apollo-page-flip-no-tearing.patch`，
  该补丁引用 Mi 10T 的 APSL 2.0 实现；本项目适配到 Redwood framebuffer。
  SM8250 时钟寄存器核对 [Linux GCC](https://github.com/torvalds/linux/blob/master/drivers/clk/qcom/gcc-sm8250.c)
  和 [Linux DISPCC](https://github.com/torvalds/linux/blob/master/drivers/clk/qcom/dispcc-sm8250.c) 定义；只使用硬件地址事实。
- 用户态构建脚本沿用基线 MIT 许可证，全文在 `port/src/hackios-xnu-userland-main/LICENSE`。
- 构建时下载的 Linux ath11k 为 BSD-3-Clause-Clear，FreeBSD LinuxKPI 保留各文件许可证；
  两者源码和对象文件均不随本仓库提交。

- GPU 缩放和物理 scanout IOSurface 来自 Apollo `0159`，固定提交
  `1f75f381e31da96de537653fbac2252d2eb80418`；内核沿用 APSL 2.0，用户态沿用 MIT。
- K40 音频基于 [enesuzun2002/hakios-apollo](https://github.com/enesuzun2002/hakios-apollo/tree/620b134a74335596e7cf8d824c4d88efe8e85143/devices/mi-10t)
  的 Mi 10T 实现（`620b134a`），内核沿用 APSL 2.0；重定位 ADSP 内存、调整功放地址和共享时钟投票。
- QCA6390 蓝牙移植 Apollo `0136`、`0137`、`0141`–`0143`、`0145` 的最终代码，以及固定 UMI
  `77fa050f` 的 iOS 16.5 兼容初始化。UMI 的 MIT 声明全文保留在 [licenses/UMI-MIT.txt](licenses/UMI-MIT.txt)。
  iOS 16.5 的 H4 表和日志符号地址从本机二进制重新核对，使用 UUID 校验保护。
- 显示 bridge 构建使用 Khronos OpenGL Registry `gl.xml` 提交
  `6af574a14089ccfee87efe230ebcdd8742859813`；其许可证保留在下载文件头部。生成物与 Mesa 库不提交。
- 原生 IPA 安装测试使用 [opa334/TrollStore 2.1.1](https://github.com/opa334/TrollStore/releases/tag/2.1.1)
  和作者发布的 ldid；下载二进制只保存在本机忽略目录。TrollStore 的 MIT 与其 uicache 的 BSD-4-Clause
  声明保留在 [licenses/opa334-TrollStore-LICENSE](licenses/opa334-TrollStore-LICENSE)。本项目的安装器兼容库
  为新增代码，仅对 UUID 和原始指令均匹配的 2.1.1 helper 做内存适配。
- 容器安装应用的 FrontBoard 启动适配参考 [akemin-dayo/AppSync](https://github.com/akemin-dayo/AppSync)
  固定提交 `235aca6cddfbdc9fa87fcb5b2aec2df37ed6d65a` 的 `FBSSignatureValidationService` hook。
  独立的 `inject_app_trust.m` 与其动态库沿用 GPL-3.0-or-later，并将范围限制为本机 TrollStore 安装记录。
  作者声明与完整许可证见 [licenses/AppSync-LICENSE](licenses/AppSync-LICENSE)、[licenses/GPL-3.0.txt](licenses/GPL-3.0.txt)。

本次 Alioth 修改日期：2026-11-01、2026-11-06、2026-11-07。修改内容包括硬件参数核对、Redwood 接口衔接、设备本地固件读取、
返回 loader 前停止 DMA/复原 SMMU、iOS 网络服务启动和实验脚本。内核修改沿用所在文件的 APSL 2.0；
Wi-Fi 兼容层修改沿用 MIT。

Apple iOS 系统、身份服务程序、设备固件及校准数据未包含在本仓库；构建时从本机已有输入提取。
