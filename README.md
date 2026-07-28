# 小智 ESP32 MQTT Logger

基于 [78/xiaozhi-esp32](https://github.com/78/xiaozhi-esp32) 的个人定制固件：在不改变小智官方 AI 通信逻辑的前提下，为设备增加独立、低优先级的 MQTT 日志上报与局域网自动发现能力。

> 这是个人维护的固件仓库，不是小智官方发布渠道。仓库仅包含固件，不包含后端服务端代码、模型文件或任何私有配置。

## 当前开发分支与项目入口

GitHub 默认首页只展示稳定基线；正在维护的功能位于以下独立分支。这样可以让固件、群晖部署和私有配置保持清晰边界。

| 内容 | 分支 | 当前状态 |
| --- | --- | --- |
| ESP32 AIoT 远程 MQTT 固件 | [`feature/aiot-remote-mqtt-firmware`](https://github.com/yyj7890/xiaozhi-esp32-mqtt-logger/tree/feature/aiot-remote-mqtt-firmware) | 独立日志、局域网发现及主动播报 MQTT/Opus 源码；主动播报已完成源码和构建验证，尚未真实 MQTT 联调或烧录。 |
| Synology MCP 桥接器 | [`feature/synology-mcp-bridge`](https://github.com/yyj7890/xiaozhi-esp32-mqtt-logger/tree/feature/synology-mcp-bridge/deploy/synology-mcp-bridge) | DS920+ 的官方小智 MCP → Home Assistant / 提醒 / 笔记本白名单工具桥接器；当前桥接器版本 `0.1.6`，包含提醒默认目标设备修复。 |

这两个组件不是同一个程序：固件不保存 NAS、Home Assistant 或小智的私密配置；桥接器不修改固件、IoT 后端、MQTT 协议或 Home Assistant。请勿将 `bridge.env`、`sdkconfig`、Token、设备编号、照片或运行日志提交到 GitHub。

## 本仓库改动

### 独立 MQTT 状态与事件日志

设备可通过一个独立的 MQTT 客户端向 AIoT 系统上报运行情况。该客户端在后台低优先级运行；连接、发布或发现失败时会退避重试或丢弃日志，**不会阻塞音频、唤醒、Wi-Fi 或官方小智 AI 连接**。

- 状态 Topic：`aiot/device/{deviceCode}/report`
- 事件 Topic：`aiot/device/{deviceCode}/log`
- 上报 Wi-Fi 连接/断开、启动、MQTT 连接或重连、官方 AI 连接、本地 AI 连接与故障等低频关键事件
- `deviceCode`、Broker、认证、端口、Client ID、重试时间均可在 `menuconfig` 配置
- Broker 地址为空时，日志上报保持关闭；官方小智功能不受影响

### 局域网 MQTT Broker 自动发现

Wi-Fi 连接后，固件可向局域网 UDP 端口 `19830` 发送 `aiot-mqtt-discovery-v1` 发现请求；收到经 nonce、可选共享 token 和私有 IPv4 地址校验的响应后，只将发现结果用于日志 MQTT。

- 默认超时：1500 ms；默认重试：3 次
- 若发现或连接失败，则回退到 `menuconfig` 中手动配置的 Broker
- **绝不修改**官方小智 AI 的服务器地址

### 局域网小智服务端发现与回退

固件还可在启动时通过 UDP `19831` 寻找兼容的本地 `xiaozhi-esp32-server`。本地服务端可用时优先使用；未发现、OTA 请求失败或 WebSocket 握手失败时，固件会回退到原有官方 OTA/AI 服务。

> 此能力需要局域网内运行兼容的服务端。本仓库不发布服务端源码；本机部署记录见 [docs/LOCAL_SERVER.md](docs/LOCAL_SERVER.md)。

## 与上游官方项目的关系

| 内容 | 来源与说明 |
| --- | --- |
| 小智语音交互、ASR/LLM/TTS、官方 WebSocket/MQTT+UDP 协议、OTA、MCP、显示与硬件适配 | 上游 [78/xiaozhi-esp32](https://github.com/78/xiaozhi-esp32) |
| 支持的开发板、烧录工具链、官方使用说明 | 上游项目及其 [中文 README](README_zh.md) |
| 独立 AIoT MQTT 日志、日志 Broker 自动发现、局域网本地服务端发现/回退 | 本仓库的定制改动 |

上游完整功能介绍和硬件资料请查看 [上游仓库](https://github.com/78/xiaozhi-esp32)。本仓库保留根目录 [LICENSE](LICENSE) 中的 MIT 许可证和上游版权声明。

## 当前固件目标

当前已验证的目标是 **ESP32-S3 N16R8 + `bread-compact-wifi` + SSD1306 128×32 OLED**。其他上游支持的开发板仍可按上游文档配置和编译，但未必包含本项目的真机验证结果。

## 从源码编译并烧录

### 1. 准备环境

安装 Espressif ESP-IDF（建议使用 **v5.5.4**；上游要求 v5.4 或更高）和 Git。请在 ESP-IDF 已启用的终端中执行以下命令：

```powershell
git clone https://github.com/yyj7890/xiaozhi-esp32-mqtt-logger.git
cd xiaozhi-esp32-mqtt-logger
idf.py set-target esp32s3
```

### 2. 配置开发板与日志功能

```powershell
idf.py menuconfig
```

在菜单中完成以下配置：

1. 选择当前开发板：`Xiaozhi Assistant` → 开发板类型，选择 `bread-compact-wifi`。
2. 选择显示与硬件参数：使用本机已验证的 `ESP32-S3`、SSD1306 128×32 OLED 配置；其他开发板请按上游资料选择。
3. 打开 `Xiaozhi Assistant` → `AIoT MQTT Log Reporting`：
   - 设置 MQTT Broker 主机和端口；不填写主机则不上报日志。
   - 按需填写用户名、密码、`deviceCode` 与 Client ID。
   - 按需启用 UDP `19830` 的 Broker 自动发现及共享 token。
4. 如需连接局域网服务端，设置 `Local AI Server Discovery`（UDP `19831`）的开关与共享 token；不需要时可关闭。

> `sdkconfig` 会保存本机 MQTT 密码、Broker 和网络配置，已被 `.gitignore` 排除。不要将它、`.env` 或任何密钥提交到 GitHub。

### 3. 编译、烧录和查看日志

将 `COMx` 替换为设备在 Windows 设备管理器中显示的串口号：

```powershell
idf.py build
idf.py -p COMx flash monitor
```

第一次烧录会写入应用、引导程序、分区表和所选资源。退出串口监视器可按 `Ctrl+]`。以后只重新编译和烧录时可执行：

```powershell
idf.py build
idf.py -p COMx flash
```

## 验证 MQTT 日志

设备连上 Wi-Fi 后，检查 Broker 是否收到：

```text
aiot/device/{deviceCode}/report
aiot/device/{deviceCode}/log
```

`report` 用于设备状态；`log` 用于启动、Wi-Fi、官方 AI、本地 AI 和日志 MQTT 的关键生命周期事件。固件在串口中也会输出上报和连接状态，便于排查配置问题。

## 文档

- [本地服务端部署记录](docs/LOCAL_SERVER.md)：只记录本机运行服务端的方法，不含服务端源码与密钥。
- [发布前检查清单](docs/PUBLISHING.md)：许可证、敏感信息与发布步骤。
- [上游中文说明](README_zh.md)：小智官方固件功能、硬件和开发资料。

## 许可证与致谢

本项目基于 [78/xiaozhi-esp32](https://github.com/78/xiaozhi-esp32)，遵循仓库根目录中的 [MIT License](LICENSE)。再发布或修改时，请保留原有版权和许可证声明；新增的代码、素材、模型或音频也应分别确认其授权条件。
