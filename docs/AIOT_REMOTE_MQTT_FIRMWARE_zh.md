# AIoT 日志远程 MQTT 固件版本记录

更新时间：2026-07-16

## 1. 文档目的

本文记录小智 ESP32 固件新增“AIoT 日志远程 MQTT 固件版本”的设计、实现范围、配置方式、验证状态和后续测试结果。

该版本仅扩展独立的 AIoT 日志 MQTT 通道，不替换现有局域网日志固件，也不修改小智官方 AI、OTA、WebSocket、本地 AI 回退、语音识别、语音合成或音频通道。

## 2. 版本关系

现有局域网固件继续保留：

```text
小智 → UDP 19830 自动发现 → 本地 Mosquitto TCP 1883 → AIoT 后端
```

新增远程 MQTT 固件版本：

```text
小智 → HiveMQ Cloud MQTT TLS 8883 → AIoT 后端 → MySQL → Vue
```

远程模式不使用 UDP 广播，不要求设备与后端位于同一 Wi-Fi 或局域网。

建议的版本标识：

```text
开发分支：feature/aiot-remote-mqtt-firmware
固件版本：v2.2.6-aiot-remote-mqtt.4
候选标签：firmware-v2.2.6-aiot-remote-mqtt.4
```

当前状态：本地开发分支已创建；尚未执行 commit、push、创建 GitHub 远程分支或标签。原 `main` 分支的已有局域网版本未被覆盖。

本分支生成的是“远程能力版”固件：同一个固件镜像在配网页中保留局域网和远程两个独立配置档案，通过运行时模式开关选择当前日志通道。原局域网固件仍可从原分支独立构建。

## 3. 固定的数据约束

日志 Topic：

```text
aiot/device/{deviceCode}/log
```

状态上报 Topic：

```text
aiot/device/{deviceCode}/report
```

必须保证以下三处的 `deviceCode` 完全一致：

1. MQTT Topic 中的设备编号；
2. JSON Payload 中的 `deviceCode`；
3. AIoT 管理端已创建的设备编号。

`logType` 只允许：

- `RUNNING`
- `ERROR`
- `MAINTENANCE`
- `INSPECTION`

不得使用 `SYSTEM` 作为 `logType`。

## 4. 公开配置与私密配置

公开代码、示例配置和文档只能出现以下占位符：

```text
HIVEMQ_HOST=<private-host>
HIVEMQ_PORT=8883
MQTT_USERNAME=<private-username>
MQTT_PASSWORD=<private-password>
```

不得写入代码、日志、文档或 Git 的内容包括：

- 真实 HiveMQ 域名、用户名、密码或 Token；
- 设备 MAC、局域网 IP、真实 `deviceCode`；
- 证书私钥、运行日志、数据库数据；
- 包含上述信息的 `sdkconfig`、NVS 导出文件或截图。

当前阶段允许本人设备共用一套全局 MQTT 凭证，不实现每台设备独立账号。

## 5. 配网页与 NVS 设计

远程版本继续使用现有 `Xiaozhi-xxxx` 配网页中的独立 IoT 日志区域，不新增另一套 Wi-Fi 配网系统。

页面提供两种模式：

### 局域网模式

- UDP `19830` 自动发现；
- 本地 Mosquitto TCP `1883`；
- 手工 Broker 作为发现失败后的兜底；
- 可配置发现 Token；
- 保持现有启动验证和配网行为。

### 远程模式

- 必须填写 HiveMQ 完整域名；
- 固定使用 TLS `8883`；
- 禁止填写 IPv4、IPv6 或 HiveMQ 解析后的 IP；
- 强制 CA 证书、主机名验证和 SNI；
- 不发送 UDP `19830`；
- 日志连接失败仅停止或重试日志上传，不阻止小智启动和语音功能。

已在同一个页面中分别保存局域网和远程配置，切换模式不会相互覆盖：

```text
LAN 配置：lan_host、lan_port、lan_username、lan_password、lan_discovery_token
远程配置：remote_host、remote_port、remote_username、remote_password
当前模式：mode=lan 或 mode=remote
```

密码和 Token 必须保持只写不回显。当前固件未启用 NVS Encryption、Flash Encryption 或 Secure Boot，保存在 NVS 中的 MQTT 凭证存在被物理读取的风险。

## 6. TLS 实现要求

远程日志客户端使用 ESP-IDF ESP-MQTT 和 MbedTLS：

- `MQTT_TRANSPORT_OVER_SSL`；
- ESP-IDF X.509 CA bundle；
- 服务端证书链验证；
- HiveMQ 域名主机名验证；
- TLS SNI；
- DNS 域名连接，不允许使用 IP 字面量；
- 不提供关闭证书验证或跳过主机名验证的配置。

优先使用 ESP-IDF CA bundle，不嵌入单张 HiveMQ 服务器证书，以降低公共 CA 或服务器证书轮换造成的维护风险。

当前工程继续全局关闭 `CONFIG_MBEDTLS_HAVE_TIME_DATE`，避免设备冷启动时间仍为 1970 时破坏官方 OTA HTTPS 取时流程。远程 AIoT 日志通道单独叠加证书日期检查：先调用 ESP-IDF CA bundle 原始校验回调，再在同一次 TLS 握手中根据可信 UTC 时间为证书链每一级设置 `FUTURE` 或 `EXPIRED` 校验标志。校验失败时握手终止，MQTT 用户名和密码尚未发送。

远程日志在系统时间早于可信阈值时不发起 TLS，只由低优先级日志任务后台退避重试；官方 OTA 从 HTTPS 响应取得服务器时间后，日志任务才会继续连接。如果官方取时不可用，受影响的只有远程日志上传，小智启动、语音和官方网络通道不被阻塞。`CONFIG_MBEDTLS_SSL_KEEP_PEER_CERTIFICATE` 仍保持关闭，不增加所有 TLS 通道的常驻证书内存。

该实现使用当前 ESP-IDF v5.5.4 / MbedTLS 的 `MBEDTLS_PRIVATE(f_vrfy)` 和 `MBEDTLS_PRIVATE(p_vrfy)` 字段包装 CA bundle 回调；升级 ESP-IDF 或 MbedTLS 时必须重新审查、编译并做真机 TLS 失败测试。

## 7. 当前实现状态

已确认：

- 当前目标为 ESP32-S3、`bread-compact-wifi`、16 MB Flash；
- ESP-IDF 为 v5.5.4；
- 已启用 MbedTLS TLS 客户端和完整 CA bundle；
- ESP-IDF 支持服务端证书验证、主机名验证和 SNI；
- 已创建本地分支 `feature/aiot-remote-mqtt-firmware`，未提交、未推送；
- LAN 与远程 NVS 配置已拆为两个档案，并兼容读取、镜像旧 NVS 键；
- 配网页已拆为局域网和远程两个配置区，密码与 Token 只写不回显；
- 远程 Host 会拒绝 IP、URL、端口和非法 DNS 标签，端口固定为 `8883`；
- 远程模式强制 ESP-IDF CA bundle、证书域名检查和 SNI，且跳过 UDP 发现；
- 远程日志已在 CA bundle 握手回调中独立检查整条服务端证书链的生效和过期时间；
- 冷启动未取得可信时间时远程日志只等待和重试，不进入启动门禁；
- 远程模式不进入小智启动门禁，连接失败由低优先级日志任务退避重试；
- 局域网模式继续使用 UDP `19830`、TCP `1883` 和原启动验证行为；
- `/log` Payload 已显式映射合法 `logType`：错误为 `ERROR`，其他运行事件为 `RUNNING`；
- `.gitignore` 已放行 `components/aiot_log_config/` 和本地覆盖的 `components/esp-wifi-connect/`，仍忽略生成的 `.component_hash`；
- ESP-IDF v5.5.4 完整构建已通过，生成 `build/xiaozhi.bin`。

当前待解决问题：

1. 已通过配网页写入私有远程配置，`.1` 真机曾成功经 HiveMQ 上传到 IoT 后端，`.2` 已确认普通 Wi-Fi 下稳定建立远程日志连接；
2. 尚未使用手机热点或其他异地网络测试；
3. 尚未验证断网退避、Wi-Fi 恢复和 Broker 不可达后的真实故障/恢复事件；
4. 群晖尚未恢复，未验证完整后端链路；
5. 当前 NVS 未加密，凭据存在物理读取风险；
6. 本地覆盖组件目前仍是未跟踪文件，未来提交时必须确认完整加入，不能只提交 `main/`；
7. 标准 `export.ps1` 默认仍引用已不存在的 `idf5.5_py3.11_env`，而当前 `D:\Espressif\tools` 的约束文件和工具包目录布局也不能被同一个 `IDF_TOOLS_PATH` 直接识别，后续需要单独修复开发环境配置；本次未安装、删除或重置 ESP-IDF。

## 8. 计划修改范围

预计只涉及：

- 独立 AIoT 日志 MQTT 客户端；
- AIoT 日志 NVS 配置组件；
- 配网页中的独立 IoT 日志区域；
- 必要的构建依赖和 `.gitignore` 规则；
- 本文档及相关公开说明。

明确不涉及：

- 小智官方 MQTT/UDP AI 协议；
- 官方 OTA；
- 官方 WebSocket；
- 本地 AI 发现与回退；
- ASR、LLM、TTS；
- 音频采集、播放、唤醒词和编解码通道。

## 9. 实施记录

1. 已创建本地 `feature/aiot-remote-mqtt-firmware` 分支并保留原有未提交修改；
2. 已修复 `.gitignore` 的组件跟踪规则；
3. 已将 LAN 与远程配置拆分保存；
4. 已完成严格域名校验、TLS、CA、主机名验证和 SNI；
5. 已让远程日志失败退出小智启动门禁，局域网门禁保持不变；
6. 已补充合法 `logType`；
7. 已完成 ESP-IDF v5.5.4 配置与完整编译；
8. 已为远程日志通道增加可信时间门控和握手期证书有效期检查，未开启全局日期检查；
9. 已完成配网页 JavaScript 语法检查、敏感信息扫描和 `git diff --check`；
10. 用户已明确允许在 `COM7` 烧录；`.1` 与 `.2` 均已完成对应真机验证，`.2` 的烧录和冷启动结果记录在第 14 节。

本次 `COM7` 烧录已获得用户明确授权并完成。未经用户另行明确允许，不执行 Git commit、Git push、创建 GitHub 远程分支、创建标签、再次烧录或部署。

## 10. 真机测试顺序

1. 静态检查远程配置和日志调用链；
2. 编译局域网版本，确认原构建方式没有回归；
3. 编译远程版本；
4. 烧录远程版本；
5. 使用普通 Wi-Fi 测试 TLS 连接和小智主功能；
6. 使用手机热点或其他不同网络测试远程日志；
7. 使用 HiveMQ/MQTTX 确认消息到达；
8. 群晖恢复后验证“小智 → HiveMQ Cloud → 群晖后端 → MySQL → Vue”；
9. 切回局域网模式，验证 UDP `19830` 和本地 Mosquitto TCP `1883`；
10. 验证断网、恢复、凭证错误和 Broker 不可用时，小智主功能仍符合对应模式的设计。

## 11. 验证记录

### 2026-07-15

- 修改范围：仅独立 AIoT 日志客户端、独立 NVS 配置组件、配网页 AIoT 区域、组件跟踪规则和本文档；
- 编译环境：ESP-IDF v5.5.4、Python 3.11.5、GNU Xtensa 14.2.0、目标 ESP32-S3；
- 编译结果：成功生成 `build/xiaozhi.bin`，大小 `0x290a90`，最小应用分区剩余约 35%；
- 固件版本建议：`v2.2.6-aiot-remote-mqtt.1`；
- TLS 静态结果（当日状态）：CA bundle、主机名验证、SNI、DNS Host 限制已实现；证书日期检查随后已于 2026-07-16 按第 6 节方案补齐；
- Git：无 commit、无 push、无 GitHub 远程分支、无标签；
- 未执行：烧录、配网页真机检查、普通 Wi-Fi/手机热点/异地网络、HiveMQ/MQTTX 真机消息、群晖/MySQL/Vue 链路、最终局域网回归。

### 2026-07-16

- 远程日志在可信系统时间可用前不发起 TLS，保持后台退避，不阻塞小智主功能；
- ESP-IDF CA bundle 原始信任链校验、主机名验证和 SNI 保持不变；远程日志专用回调在握手期间追加证书链 `valid from`/`valid to` 检查；
- 全局 `CONFIG_MBEDTLS_HAVE_TIME_DATE` 和 `CONFIG_MBEDTLS_SSL_KEEP_PEER_CERTIFICATE` 均未开启，官方 OTA、WebSocket 和其他 TLS 通道未改；
- 最新增量对象编译、组件归档、ELF 链接、BIN 生成和分区尺寸检查通过：`build/xiaozhi.bin` 大小 `0x290bd0`，最小应用分区剩余 `0x15f430`（35%）；
- 标准 ESP-IDF `export.ps1` 仍因旧 Python 虚拟环境路径及非标准工具目录布局而失败；本次复用已生成的 ESP-IDF v5.5.4 构建图和实际可用 Python 3.11.5 / Xtensa 14.2.0 工具链完成等价构建步骤，未安装或重置环境；
- 已将该构建烧录到用户指定的 `COM7` 测试设备，五个分区均通过 esptool 写入后哈希校验，未整片擦除；
- 首次启动观察到 Wi-Fi 站点、配网页服务和网络连接启动；后续 30 秒稳定性观察没有再次复位、Panic、看门狗或错误级日志；
- 未执行：配网页远程私有配置、真实 HiveMQ 连接、凭证错误、过期/尚未生效证书、手机热点/异地网络和群晖全链路测试。

## 12. 局域网回退基线

由于原自定义 `components/aiot_log_config/` 和 `components/esp-wifi-connect/` 曾被 `.gitignore` 忽略，Git 提交 `440476d` 本身不包含完整的局域网组件源码。2026-07-15 已根据该 Git 提交和 2026-07-13 本地历史，在独立 detached worktree 中恢复并重新编译纯局域网版本。

本地私有备份目录：

```text
D:\AI\XiaoZhi\firmware-backups\xiaozhi-lan-baseline-2026-07-15-440476d
```

备份内容包括完整烧录分区文件、私有 `sdkconfig`、依赖锁、自定义组件源码压缩包、恢复说明和 SHA-256 清单。该目录位于 Git 仓库之外，不得上传其中的 `sdkconfig.private` 或固件二进制。

局域网基线编译结果：`xiaozhi.bin` 大小 `0x28efa0`，最小应用分区剩余约 35%。该基线尚未烧录和真机回归。

## 13. 远程 MQTT 测试基线

2026-07-16 已为当前未提交的远程 MQTT 工作区创建独立私有备份，版本标识为 `v2.2.6-aiot-remote-mqtt.1`：

```text
D:\AI\XiaoZhi\firmware-backups\xiaozhi-remote-mqtt-baseline-2026-07-16-v2.2.6-aiot-remote-mqtt.1
```

备份位于 Git 仓库之外，包含完整烧录分区、烧录参数、依赖锁、私有 `sdkconfig`、本地组件源码快照、说明和 SHA-256 校验值。`xiaozhi.bin` 大小为 `0x290bd0`，SHA-256 为 `10F99AF0D55F1C66F0CBAEA5C682F29240E72E01D06BB0CC0F287A8843FEA5DD`。

该备份作为真机测试的可恢复基线，未创建 Git commit、GitHub 远程分支或标签。2026-07-16 已成功烧录到用户指定的 `COM7` 测试设备；不得上传其中的 `sdkconfig.private`、固件二进制或任何可能包含本机配置的文件。

## 14. 远程 MQTT 假异常修复基线

`.1` 真机已经成功通过 HiveMQ 上传日志，证明远程 TLS、凭证权限、Topic 和 IoT 后端订阅链路可用。首次启动汇总中出现“连接失败 → 恢复 → 已连接”的不合理序列，已定位为独立日志客户端状态机问题，而不是官方 AI 通道或 HiveMQ 入库失败。

`v2.2.6-aiot-remote-mqtt.2` 修复内容：

- 连接结果区分为已连接、等待条件和真实失败；等待可信系统时间不再记为 MQTT 失败；
- 主动停止 MQTT 客户端时忽略预期的断开事件；
- 真实失败恢复后立即设置首次连接标志，不再继续重复上报“日志 MQTT 已连接”；
- 真实 Broker 意外断开后，即使没有待发送业务记录也会由低优先级日志任务主动重连；
- 失败事件类型改为后端已识别的 `mqtt_connection_failed`；
- 局域网启动门禁、UDP `19830`、Mosquitto TCP `1883` 和小智官方通道保持不变。

构建验证：MQTT 对象编译、完整 `libmain.a` 归档、ELF 链接、BIN 生成和分区检查通过。`xiaozhi.bin` 大小 `0x290c30`，最小应用分区剩余 `0x15f3d0`（35%），SHA-256 为 `1B52BDDE61849C294AFC643F4E0B5DDC13D8FCA9BAA8A748FEF3B9093494D1CC`。

新的私有备份目录：

```text
D:\AI\XiaoZhi\firmware-backups\xiaozhi-remote-mqtt-baseline-2026-07-16-v2.2.6-aiot-remote-mqtt.2
```

`.2` 已于 2026-07-16 烧录到用户指定的 `COM7` 测试设备。烧录前核对的 `xiaozhi.bin` SHA-256 与上述值一致；本次只写入 `ota_data_initial.bin`（`0x00d000`）和 `xiaozhi.bin`（`0x020000`），写后哈希校验均通过。未整片擦除、未改写 NVS，现有 Wi-Fi 和远程 MQTT 配置得到保留。

烧录后通过串口控制线执行普通硬复位并进行脱敏观察。连续两次完整启动均只发布一次 `mqtt_connected`，没有发布 `mqtt_connection_failed` 或 `mqtt_reconnected`；设备正常联网，未出现 Panic、看门狗、TLS 错误、日志队列发布失败或远程配置无效。泛化错误关键字检查命中的唯一一条记录已进一步确认为 `local_ai_discovery_failed`，它是原有本地 AI 未发现后的官方 AI 回退事件，不是日志 MQTT 故障，本次未修改该链路。

仍需验证真实 Broker 故障/恢复、Wi-Fi 断开恢复、手机热点或异地网络，以及最终局域网 UDP `19830` / Mosquitto TCP `1883` 回归。IoT 后端的故障恢复状态修复尚未部署，因此“待处理 → 已解决”的页面结果要等后端更新后再验证。

## 15. 远程连接状态文案修复候选版

`v2.2.6-aiot-remote-mqtt.3` 用于让 IoT 日志明确显示当前连接的是远程 TLS 日志通道。固件继续使用兼容的 `mqtt_connected`、`mqtt_connection_failed` 和 `mqtt_reconnected` 事件类型，仅根据当前配置档案区分消息内容：

- 远程首次连接：`Remote log MQTT TLS connected`；
- 远程连接失败并重试：`Remote log MQTT TLS connection failed and was retried`；
- 远程连接恢复：`Remote log MQTT TLS connection recovered`；
- 局域网模式继续使用原有通用消息，不改变 UDP `19830`、Mosquitto TCP `1883` 或启动门禁。

该设计不在 Payload 中加入 Host、用户名、密码、Token、IP 或其他私密配置。IoT 后端和前端需要为三个远程消息增加中文映射，建议显示“远程日志 MQTT（TLS 8883）已连接/连接失败，正在重试/连接已恢复”；同时应在运行事件汇总处忽略同一启动批次内相邻且完全相同的事件，避免 MQTT QoS 1 重复投递形成重复文案。不要仅通过降低 QoS 规避重复。

`.2` 真机脱敏串口观察证明每次启动只由固件发布一次 `mqtt_connected`。因此页面中相邻的两条“日志 MQTT 已连接”不应再通过增加固件状态标志处理，重复抑制应放在 IoT 后端的入库汇总边界。

构建验证：`mqtt_log_client.cc` 编译、完整 `libmain.a` 归档、ELF 链接、BIN 生成和分区检查均通过。`.3` 的 `xiaozhi.bin` 大小为 `0x290e20`（2,690,592 字节），最小应用分区剩余 `0x15f1e0`（35%），SHA-256 为 `A30D2A6DEDFE39485FBAB60FDBBE6058668B03FC423A1EF1767F007CDD1DDF96`。

`.3` 已于 2026-07-16 在用户明确允许后烧录到 `COM7` 测试设备。烧录前确认当前构建产物与私有备份哈希一致；只写入 `ota_data_initial.bin`（`0x00d000`）和 `xiaozhi.bin`（`0x020000`），写后哈希校验均通过，未整片擦除或改写 NVS。

烧录后普通硬复位的 65 秒脱敏观察结果：设备完整启动、正常联网、可信系统时间就绪并只发布一次 `mqtt_connected`；没有发布 `mqtt_connection_failed` 或 `mqtt_reconnected`，也没有 TLS 错误、日志发布排队失败、远程配置无效、Panic、看门狗或 Wi-Fi 掉线。串口只显示事件类型，不能直接看到 MQTT Payload 文案；二进制已静态确认包含新的远程 TLS 三条状态消息，最终中文显示和后端相邻去重仍需在更新后的 IOT 页面确认。

## 16. 网络切换稳定性修复候选版

异地 Wi-Fi/手机热点实测表明远程链路可以成功上传，但网络切换期间可能出现 MQTT 失败/恢复噪声、可信时间就绪后仍受指数退避延迟、启动事件超过 8 条时队列不足，以及固件 `WARN` 与 IoT 标准 `WARNING` 不一致。`v2.2.6-aiot-remote-mqtt.4` 针对这些问题进行以下修改：

- 日志队列深度由 8 增加到 16，降低网络尚未就绪时启动事件被丢弃的风险；
- 等待可信系统时间的 `Deferred` 状态改为固定 1 秒复查，不再进入 5/10/20/40 秒连接失败指数退避；
- Wi-Fi 断开和重新连接回调立即设置网络切换标志，切换期间 ESP-MQTT 产生的预期断开/错误事件不再算作 Broker 故障；
- 远程模式只有连续至少 3 次连接失败或持续失败达到 30 秒才标记为可上报故障；短暂 DNS、路由或 TLS 抖动只后台重试，不产生失败/恢复日志；
- `mqtt_failure_pending_` 只负责驱动后台重连，新增独立的可上报故障状态，避免“需要重连”等同于“必须生成事故日志”；
- 固件运行事件统一发送 `INFO`、`WARNING`、`ERROR`，内部仍兼容旧调用中的 `WARN` 并在发送前规范为 `WARNING`；
- 局域网模式仍保持原有即时故障语义、UDP `19830`、Mosquitto TCP `1883`、发现回退和启动验证门禁。

构建验证：`mqtt_log_client.cc`、`application.cc` 对象编译，完整 85 对象 `libmain.a` 归档、ELF 链接、BIN 生成和分区检查均通过。`.4` 的 `xiaozhi.bin` 大小为 `0x291070`（2,691,184 字节），最小应用分区剩余 `0x15ef90`（35%），SHA-256 为 `5929175DF1F9AD6B38F4148045A1BE2CB7316F28BC634521D6A14772FE244B06`。

`.4` 已于 2026-07-17 在用户明确允许后烧录到 `COM7` 测试设备。烧录前确认当前构建产物、私有备份与记录的 `xiaozhi.bin` SHA-256 一致；本次只写入 `ota_data_initial.bin`（`0x00d000`）和 `xiaozhi.bin`（`0x020000`），写后哈希校验均通过，未整片擦除或改写 NVS，原有 Wi-Fi 和远程 MQTT 配置得到保留。

烧录后普通硬复位的 70 秒脱敏观察结果：设备完成联网，可信系统时间就绪并只发布一次 `mqtt_connected`；没有发布 `mqtt_connection_failed` 或 `mqtt_reconnected`，没有日志队列丢弃、发布排队失败、远程配置无效、TLS 错误、Panic 或看门狗。观察到两条 Wi-Fi 断开相关启动/切换记录，但设备随后正常联网且未被上报为 MQTT 故障，符合本候选版将网络切换与 Broker 故障分离的目标。

仍需人工验证手机热点/异地 Wi-Fi 冷启动、运行中切换网络、短暂断网恢复、持续 Broker 不可达阈值，以及最终局域网 UDP `19830` / Mosquitto TCP `1883` 回归。
