# Redmi K40 iOS Wi-Fi 适配实验

Redmi K40 / alioth / Snapdragon 870 / QCA6390 的 iOS 16.5 Wi-Fi 适配源码和复现脚本。
以 MrMoxing 的 alioth 桌面移植为基线，接入 Apollo / UMI 的 PCIe、MHI、QRTR、QMI 和 ath11k 通路。

2026-11-01 的本机结果：

| 项目 | 状态 |
| --- | --- |
| iOS 桌面、Adreno 650 加速、FocalTech 触摸 | 真机启动，用户确认可滑动和打开设置 |
| QCA6390 PCIe 枚举 | 成功，PCI ID `17cb:1101` |
| 原机固件启动、WLFW、冷启动校准 | 成功 |
| 热点扫描 | 成功；2.4 GHz 和 5 GHz 均有扫描结果 |
| 2.4 GHz 连接 | 用户确认可用；日志确认 WPA2 握手、密钥安装、实际收发包 |
| 5 GHz 连接 | **待修：能看到热点，用户尝试加入失败** |
| 重启后自动连接、长期功耗、稳定性 | 尚未验证 |

这是源码实验项目，当前通过 `fastboot boot` 和 RAM 系统运行。启动脚本不执行分区刷写或清除；
loader 会把诊断日志写入手机已有的 `logdump` 分区。原 Android 和 userdata 保留，iOS 设置未配置持久化。

## 仓库内容

- `port/`：相对固定 alioth 基线新增或修改的源码，目录结构可直接覆盖到上游检出。
- `scripts/prepare_workspace.py`：下载固定提交、应用源码覆盖层、组装 `build/k40`。
- `scripts/build_wifi_kernel.sh`：生成 WLFW 编码表并构建；检查新内核确实生成，防止误用旧产物。
- `scripts/prepare_wifi_root.py`：保留已验证的桌面启动程序，加入本机提取的无线固件和网络服务。
- `scripts/run_wifi_probe.py`：在已经驻留完整系统的 XNU loader 中启动 Wi-Fi 测试。
- `scripts/start_ios.sh`、`scripts/capture_ios.py`：原桌面 RAM 启动与日志采集。
- [适配记录与复现说明](docs/WIFI-PORT.md)、[本机会话说明](RUNNING.md)。

设备备份、无线固件、Apple iOS 文件、工具链、生成的镜像、原始日志和截图由 `.gitignore` 排除。
这些输入须在自己的机器上准备；本仓库不是可直接刷入的 ROM 包。

## 从源码准备

```bash
python3 scripts/prepare_workspace.py
```

之后按 [构建说明](docs/WIFI-PORT.md) 准备工具链、iOS 系统和自己设备的固件。
仅有内核文件不足以启动 iOS，也不足以验证 Wi-Fi。

来源固定在 [sources.json](sources.json)。各部分的许可证和来源见 [LICENSES.md](LICENSES.md)。
