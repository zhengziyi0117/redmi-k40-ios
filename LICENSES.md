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
- 用户态构建脚本沿用基线 MIT 许可证，全文在 `port/src/hackios-xnu-userland-main/LICENSE`。
- 构建时下载的 Linux ath11k 为 BSD-3-Clause-Clear，FreeBSD LinuxKPI 保留各文件许可证；
  两者源码和对象文件均不随本仓库提交。

本次 Alioth 修改日期：2026-11-01。修改内容包括硬件参数核对、Redwood 接口衔接、设备本地固件读取、
返回 loader 前停止 DMA/复原 SMMU、iOS 网络服务启动和实验脚本。内核修改沿用所在文件的 APSL 2.0；
Wi-Fi 兼容层修改沿用 MIT。

Apple iOS 系统、设备固件及校准数据未包含在本仓库。
