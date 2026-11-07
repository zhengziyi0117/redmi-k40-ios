# B 站 iOS IPA 候选核对（2026-11-06）

本轮按用户要求搜索并下载原生 iOS 包，未安装、运行或改变手机镜像。检查范围为发布来源、SHA-256、Info.plist、Mach-O 架构和加密标记。

| 候选 | 发布来源 | 包版本 | 最低 iOS | IPA / 解包大小 | 检查结果 |
| --- | --- | --- | --- | --- | --- |
| BiliBiliMApp_8.89.0.v3.1.0.ipa | [作者 Release](https://github.com/TouchFriend/BiliBiliMApp/releases/tag/3.1.0) | 8.89.0 | 13.0 | 283.8 / 879.9 MB | 主程序 arm64、iPhoneOS；6 个 Mach-O 未见非零加密标记 |
| FocuBili-v1.8.1-ios-unsigned.ipa | [作者 Release](https://github.com/L1Xu4n/FocuBili/releases/tag/v1.8.1) | 1.8.1 | 13.0 | 21.0 / 53.8 MB | 主程序 arm64、iPhoneOS；25 个 Mach-O 未见非零加密标记 |
| Ibili-unsigned_010.ipa | [作者 Release](https://github.com/pa-jesusf/Ibili/releases/tag/release) | 0.1.0 | 16.0 | 5.1 / 11.8 MB | 主程序 arm64、iPhoneOS；1 个 Mach-O 未见非零加密标记 |

- BiliBiliMApp 是作者基于官方客户端制作的去广告等修改版，不是 B 站官方发布的原版包。其 3.1.0 修改版包含 B 站 8.89.0。
- Ibili 是原生 Swift/SwiftUI 与 Rust 的开源第三方客户端；FocuBili 是 Flutter 第三方客户端，不能当作官方 B 站。
- 三个包的下载长度与 GitHub Release asset 的 SHA-256 均匹配；这只确认下载完整性，不等于已审计安全性或确认 K40 兼容。
- 当前 K40 是 iOS 16.5：上述最低版本和主程序架构满足基本条件；新 SDK 符号、渲染、视频解码、登录、安装注册与数据保存均未真机验证。
- BiliBiliMApp 解包约 880 MB；目前 boot_b 的 32 MiB 设置存档不能用来存放此应用，需要另行安排应用存放位置。
- 原始包、Release 元数据及逐文件检查 JSON 保存在被忽略的 `runtime/research/20261106-bilibili-app/`，不随源码分发。

## 文件哈希

- `BiliBiliMApp_8.89.0.v3.1.0.ipa`：`a52531ab3b5f441a54051b18cf59446ded3d06d98c2345c9c9dc4dd54c2f142c`
- `FocuBili-v1.8.1-ios-unsigned.ipa`：`f276277234b3a2e3198f4027537d8a88098e9442236e79027c52e66dd482af06`
- `Ibili-unsigned_010.ipa`：`7ed63860e55cff9f4fb1c4e8d78399cfef5ca44857493cfce719320085a134e3`
