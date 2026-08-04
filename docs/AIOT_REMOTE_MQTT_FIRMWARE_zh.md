# AIoT 日志远程 MQTT 固件版本记录

更新时间：2026-07-17

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

当前状态：功能实现基线提交 `d891417` 已推送到 `feature/aiot-remote-mqtt-firmware` GitHub 远程分支；后续允许仅用于同步记录的文档提交继续推进该分支。本地与远程保持同步；尚未合并到 `main`，未创建 Git 标签、GitHub Release 或 PR。原 `main` 分支的已有局域网版本未被覆盖。

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
- 已创建并推送功能分支 `feature/aiot-remote-mqtt-firmware`，功能实现基线提交为 `d891417`；
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

1. 已通过配网页写入私有远程配置，`.1` 真机曾成功经 HiveMQ 上传到 IoT 后端，`.2` 至 `.4` 已分别完成普通 Wi-Fi 下的远程连接验证；
2. 不同 Wi-Fi/异地网络已经证明可以上传日志，并暴露过连接噪声和页面刷新延迟；`.4` 修复后仍需重新执行手机热点或异地网络冷启动和运行中切换测试；
3. 尚未完整验证短暂断网退避、运行中 Wi-Fi 切换，以及持续 Broker 不可达达到阈值后的真实故障/恢复事件；
4. 群晖远程后端镜像已部署并观察到“小智 → HiveMQ Cloud → 群晖后端 → MySQL → Vue”日志链路；页面自动刷新时效性属于 IoT 前后端的独立问题，不作为固件 TLS 链路失败判断依据；
5. 当前 NVS 未加密，凭据存在物理读取风险；
6. 本地覆盖组件已完整纳入提交，包括 `components/aiot_log_config/` 和 `components/esp-wifi-connect/`；生成的 `.component_hash` 继续忽略；
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

1. 已创建本地 `feature/aiot-remote-mqtt-firmware` 分支，完成实现后创建提交 `d891417` 并推送到同名 GitHub 远程分支；
2. 已修复 `.gitignore` 的组件跟踪规则；
3. 已将 LAN 与远程配置拆分保存；
4. 已完成严格域名校验、TLS、CA、主机名验证和 SNI；
5. 已让远程日志失败退出小智启动门禁，局域网门禁保持不变；
6. 已补充合法 `logType`；
7. 已完成 ESP-IDF v5.5.4 配置与完整编译；
8. 已为远程日志通道增加可信时间门控和握手期证书有效期检查，未开启全局日期检查；
9. 已完成配网页 JavaScript 语法检查、敏感信息扫描和 `git diff --check`；
10. 用户已明确允许在 `COM7` 烧录；`.1` 至 `.4` 已完成对应阶段的真机验证，`.2`、`.3` 和 `.4` 的结果分别记录在第 14、15、16 节；
11. 已完成提交前隐私清理和敏感信息复查，并将最终提交与推送状态记录在第 17 节。

`COM7` 烧录、本地 Git commit 和功能分支 push 均已分别获得用户明确授权并完成。未经用户另行明确允许，不合并 `main`、不创建标签或 Release、不再次烧录或部署。

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

## 17. 隐私清理、Git 提交与 GitHub 推送

2026-07-17 在 `.4` 真机验证完成后，对准备公开提交的源码再次执行隐私和敏感信息审查，并完成以下清理：

- 删除 SmartConfig 接收凭据时向串口输出 Wi-Fi 密码的旧日志，只保留不含密码的 SSID 提示；
- 删除配网页中具体 `deviceCode` 的前端兜底值，未取得配置时显示为空；
- 将 `CONFIG_AIOT_MQTT_LOG_DEVICE_CODE` 的 Kconfig 具体默认值改为空，继续允许用户在本机 `sdkconfig` 中私密配置；
- 清理纳入版本管理的 Wi-Fi 组件原文件中的行尾空格，使 `git diff --check` 通过。

对实际提交内容执行的复查结果：

- 未发现真实 HiveMQ Cloud 域名；
- 未发现 MQTT 用户名、密码、Token 或密码格式化日志；
- 未发现具体设备 `deviceCode`、设备 MAC、私网 IPv4 地址或证书私钥；
- 本机 `sdkconfig`、`dependencies.lock`、`build/`、固件 `.bin`、私有备份目录和 `.component_hash` 均未被 Git 跟踪；
- 仓库中的 `sdkconfig.defaults*` 和开发板专用 `sdkconfig.*` 属于原项目公开板级构建配置，不是本机私密 `sdkconfig`。

提交与推送结果：

```text
分支：feature/aiot-remote-mqtt-firmware
提交：d891417 feat: add independent remote AIoT MQTT firmware mode
远程：origin/feature/aiot-remote-mqtt-firmware
```

功能实现提交完成时，本地分支与远程分支均指向 `d891417`，工作区干净；后续仅文档记录提交可以继续推进该功能分支，不改变 `d891417` 作为已验证功能实现基线的含义。`main` 仍停留在原局域网基线提交 `440476d`，尚未创建 PR、合并、标签或 GitHub Release。

需要注意：设备上当前运行并完成真机验证的仍是第 16 节记录的 `.4` 二进制，SHA-256 为 `5929175DF1F9AD6B38F4148045A1BE2CB7316F28BC634521D6A14772FE244B06`。上述三项公开提交前隐私清理发生在该固件烧录之后，因此提交 `d891417` 的源码与设备上的 `.4` 二进制不是完全相同的源代码快照。本次已完成 `wifi_configuration_ap.cc` 定向语法编译、配网页内嵌 JavaScript 语法检查、敏感信息扫描和 `git diff --check`；由于旧构建目录缺少 Ninja 增量日志，构建系统会重新执行约 2200 个步骤，提交前未再次完成新的全量固件构建。

因此，下次生成可发布固件或再次烧录前，必须基于提交 `d891417` 或其后续提交执行一次干净、完整的 ESP-IDF 构建，重新记录固件大小和 SHA-256，再进行真机启动、远程 MQTT、配网页和局域网回归。不得把当前设备中的 `.4` 二进制哈希当作提交 `d891417` 重新构建产物的哈希。

## 18. 语音创建提醒与 MQTT 主动播报（固定 Opus，待重新验证）

### 目标

保留官方小智 AI 的日常对话、现有 MCP、Home Assistant 控制和独立远程日志上报；在无人唤醒时，由小智设备主动播放提醒语音。

用户通过自然语言创建提醒，而不是手动预先填写日志系统规则。例如：“今天晚上八点提醒我拿书”“室温超过三十度提醒我开空调”。

### 最终架构

```text
用户语音
  -> 官方小智 AI
  -> 提醒 MCP 工具
  -> IoT 日志系统（保存提醒、条件、冷却时间与历史）
  -> 到时/条件满足
  -> 群晖 TTS 服务生成 Opus 音频
  -> 既有 HiveMQ Cloud MQTT 通道
  -> 小智设备播放语音，并回传播放结果
```

Home Assistant 只作为设备控制/状态来源之一；提醒的创建、排程、条件判断和播报记录由 IoT 日志系统负责。

### 现有基础与边界

- 已有远程 MQTT 通道保持为：小智设备 <-> HiveMQ Cloud <-> IoT 后端，且不替换官方 AI、官方 OTA 或现有语音对话。
- `MqttLogClient` 已保留原有 `/report` 与 `/log` 上报，并新增设备专属 announcement 下行订阅和 ACK；原有上报处理未替换。
- 小智硬件/源码复用既有扬声器、Opus 解码和音频播放队列；主动播报只在官方状态为 `Idle` 时开始，默认不打断官方对话。
- 官方公开 MCP/语音接入未确认提供“外部文字直接触发指定设备主动原声播报”的接口。因此主动提醒使用自建 TTS 音频，音色不会保证与官方对话 TTS 完全一致。

### 已完成范围

1. 固件实现固定测试 Opus 的 manifest、分帧接收、ACK 与播放调度；这不是动态 TTS，真实 HiveMQ 下行订阅和播放仍须以本节的诊断步骤重新验证。
2. 固件新增 `AnnouncementManager`，订阅 `aiot/device/{deviceCode}/announcement/command` 与 `.../audio/#`，并向 `.../ack` 发布 QoS 1、`retain=false` 的 `received`、`played`、`failed` 回执。
3. 音频是 16 kHz、单声道、60 ms 的裸 Opus packet；不使用 Ogg/Base64。manifest 使用 `aiot-announcement-v1`，逐帧校验 index、bytes 与 CRC32。
4. MQTT 分段数据按总长度和偏移重组；任务 ID、过期时间、内容指纹、容量与优先级均受限，QoS 1 重投安全去重，冲突、过期、队列满和中断均返回安全失败原因。

### 尚未实施范围

1. 未部署群晖中文 TTS；当前提醒仍使用固定、合法的测试 Opus 资源，不能据此视为动态文本播报已经完成。
2. 温湿度、设备日志和天气条件提醒、静音时段和冷却策略仍待后续实现。
3. 提醒 MCP、Home Assistant/电脑 MCP 操作审计已在受管桥接器整合分支中实现，尚未部署到群晖或接入官方 MCP 生产连接。

### 安全与发布约束

- 下行 Topic 必须使用现有 TLS MQTT 连接和 Broker ACL；设备只能订阅自己的 Topic。
- 播报任务必须带唯一 ID 和过期时间；后端必须记录已播报 ID，防止 QoS 1 重投造成重复播报。
- 新固件在后续发布或再次烧录前，仍必须基于当前源码完成干净、完整构建，并进行远程网络/手机热点恢复和官方 AI 对话回归。
- 本节不代表已部署 TTS 或群晖服务；真实 MQTT 固定语音互操作已验证，但任何后续部署、烧录或生产启用都须用户明确授权。

### 18.1 固定 Opus 协议与实现记录

复用既有远程 HiveMQ TLS 连接。连接成功后订阅本机专属 Topic：

```text
aiot/device/{deviceCode}/announcement/command
aiot/device/{deviceCode}/announcement/audio/#
```

命令使用 `aiot-announcement-v1` manifest；音频 Topic 为 `aiot/device/{deviceCode}/announcement/audio/{taskId}/{frameIndex}`，payload 是原始二进制裸 Opus packet，不使用 Base64 或 Ogg。只接受 16 kHz、单声道、60 ms 帧的 Opus；每个任务最多 40 帧，并限制单帧和总接收内存。必须校验协议、`deviceCode`、`taskId`、优先级、ISO 8601 `expiresAt`、帧数、帧序号、声明字节数与 CRC32；ESP-MQTT `MQTT_EVENT_DATA` 的 Topic 和 payload 分段必须按总长度与偏移重组，不能假设一个事件等于一条完整消息。

为避免 Topic 歧义，`taskId` 仅接受 1–64 个 ASCII 字母、数字、`-`、`_` 或 `.`；固定测试任务使用该兼容子集。

ACK 使用既有 TLS MQTT 客户端，以 QoS 1、retain=false 发布到：

```text
aiot/device/{deviceCode}/announcement/ack
```

ACK 协议同为 `aiot-announcement-v1`，只使用 `received`、`played` 或 `failed`。所有失败均在 `reason` 中给出安全的固定原因，例如 `expired`、`id_conflict`、`too_large`、`queue_full`、`superseded`、`interrupted` 或 `decode_failed`；不记录二进制音频、密码、Token 或私密配置。

任务在 manifest 与全部帧校验完成后才 ACK `received`。RAM 中保留有容量上限与 TTL 的任务终态记录：同 taskId 的 QoS 1 重投返回已知状态且不重复播放；同 taskId、不同 manifest 返回 `failed/id_conflict`。收到任务和开始播放前均检查过期时间，过期返回 `failed/expired`。最多只缓存一条完整待播任务；新任务仅能以更高优先级替换低优先级待播任务，被替换任务返回 `failed/superseded`，其他新任务返回 `failed/queue_full`。

MQTT 回调只负责有界接收、校验和投递，不等待播放或执行耗时解码。新增独立主动播报调度，不把播报硬套进 `Speaking`：因为现有 `HandleStateChangedEvent()` 在 `Speaking` 会调用 `AudioService::ResetDecoder()`。仅在官方状态为 `Idle` 时把已校验的 Opus packet 投入 `AudioService::PushPacketToDecodeQueue()`；`Listening` 或 `Speaking` 时不启动。用户唤醒或主动开始对话会中断播报并 ACK `failed/interrupted`。

`played` 不以队列清空判断。音频层会为主动播报末帧携带完成标记，并只在该帧 PCM 已实际调用扬声器输出后通知应用层发送 `played`。解码、投递或输出失败均返回 `failed`。

已新增 `AnnouncementManager`，并修改 `MqttLogClient`、`Application` 与 `AudioService`。远程 TLS MQTT 连接成功后会订阅 command 与 audio Topic；ACK 继续复用同一 MQTT 客户端并以 QoS 1、retain=false 发布。数据接收按 `MQTT_EVENT_DATA` 的总长度与偏移重组并设置上限；任务管理器校验固定音频参数、帧数/长度、CRC32、UTC 过期时间、任务 ID 和 manifest 内容指纹，并在 RAM 中以有界终态记录抑制重复任务或报告 `id_conflict`。

应用层只在 Idle 将完整任务投递至既有 Opus 解码队列；不会进入官方 `Speaking`。用户开始对话、切换聊天或唤醒时会清除当前主动播报并发送 `failed/interrupted`。音频服务为解码 packet 携带完成回调，末帧仅在 `OutputData()` 返回后 ACK `played`；Opus 解码失败回 `failed/decode_failed`。

此前已执行过 `git diff --check` 和 ESP-IDF v5.5.4 构建；该历史构建结果不能代替本次排障后的重新构建与设备验证。未部署 TTS；动态文本的音频生成、真实 MQTT 互操作、烧录及生产回归均待用户明确授权。

### 18.3 2026-07-28 下行订阅排障记录

真实联调中，设备可以通过既有远程 TLS MQTT 连接上报启动日志，但 MQTTX 向 announcement command Topic 发布故意无效的 manifest 后，未看到预期的 `failed/invalid_manifest` ACK。这只能说明上行连接可用，不能证明 announcement 下行订阅、Broker ACL、MQTT 数据事件和 ACK 发布链路可用。

本次固件修复与诊断仅在 `remote_mode=true` 时生效：

- 保存 command 和 audio/# 两次订阅调用返回的 message ID，并在 `MQTT_EVENT_SUBSCRIBED` 中分别记录 Broker 确认；不会打印实际 Topic、设备编号、主机、账号或凭证。
- `MQTT_EVENT_DATA` 仅记录 command 或 audio frame 的长度、音频帧索引和处理结果；不记录 JSON 原文、taskId、二进制音频或私密配置。分段重组增加首段元数据、空数据和边界检查。
- ACK 日志只记录 `received`、`played` 或 `failed` 是否成功进入 MQTT 发布队列，不记录 taskId、reason、Topic 或 payload。
- 可安全取得 taskId 与 deviceCode 的 manifest 即使在协议、时间或其他基础字段校验失败时，也会回 `failed/invalid_manifest`；完全无法安全解析的 JSON 不会伪造 ACK。
- manifest 的 UTC `expiresAt` 同时接受整秒 `...SSZ` 和 IoT v1.2.2 使用的小数秒 `...SS.sssZ`；非零小数秒按秒级时钟向上取整，避免任务在声明到期点之前被误判为过期。
- UTC 解析使用固定宽度 ISO-8601 格式和 UTC civil-date 到 Unix epoch 的换算，不调用本地时区相关的 `mktime()`。固件内置可重复校验：整秒、`.001Z`、`.999Z`、跨分钟、跨日，以及 IoT 实际 `.sssZ` 形式的固定基准“未来五分钟”；预期 epoch 均为独立硬编码标准值，不复用被测 civil-date 算法。`.001Z` 的未来五分钟样例因秒级向上取整，比整秒基准大 301 秒，仍严格为未来时间。
- 远程模式启动时会输出 `Announcement expiry diagnostics version=2`，用于确认当前二进制包含这套诊断。每个已成功解析 `expiresAt` 的 command 都只额外输出安全诊断 `nowEpoch`、`expiresEpoch` 和 `deltaSeconds`；当前测试日期均在 2038 年前，日志显式转换为 `int32_t` 并固定使用 `%d`，不使用任何 64 位 printf 或格式宏拼接，也不输出 taskId、设备编号、JSON 或私密配置。该三项使用同一 UTC Unix epoch 基准，便于直接确认是设备当前时钟还是 expiresAt 换算异常；实际 IoT “当前时刻 + 5 分钟”任务应产生正值、接近 300 秒的 `deltaSeconds`。
- 本次真机证据确认问题不在 IoT、MQTT 或固定 Opus 帧：OTA `server_time.timestamp` 是 UTC 毫秒时间，旧固件在 `settimeofday()` 前又叠加了 `timezone_offset`，使 `time(nullptr)` 在 UTC+8 环境中快约八小时。现已改为只将 UTC timestamp 写入系统 Unix epoch；`timezone_offset` 不参与系统时钟写入。新增独立固定基准自检，验证 `2026-07-28T00:00:00Z` 写入的 epoch 为 `1785196800`，而不是 UTC+8 偏移后的值，因此未来五分钟任务不会因时区误判 `expired`。本次未烧录、未部署，仍须按下列步骤完成真机验证。
- command 处理使用明确的安全结果类型；拒绝时仅记录 `Announcement command rejected. reason=<reason>`。可见 reason 仅限 `expired`、`invalid_manifest`、`too_large`、`id_conflict` 或 `queue_full`，不会输出原始 payload 或私密标识。
- 设备在 Listening 或 Speaking 时收齐的有效任务会保留为一条待播任务；官方状态回到 Idle 后再次调度，仍不打断官方 AI 对话。
- 2026-07-28 真机排障发现，旧实现只在末帧注册完成回调，且 `AudioOutputTask` 无条件将 `OutputData()` 后的回调标为成功。因此即使前序 Opus 解码、PCM 输出或扬声器写入失败，仍可能错误回 `played`。现已改为每个 announcement 帧都经过既有 `PushPacketToDecodeQueue()` → Opus 解码 → `audio_playback_queue_` → `AudioOutputTask` → `AudioCodec::OutputData()` 的官方播放路径；16 kHz、单声道、60 ms 帧要求至少 960 PCM samples，空 PCM、解码、重采样、入队或实际输出失败都会回 `failed/playback_failed`，不会伪造成 `played`。只有所有帧按 FIFO 成功写入扬声器输出路径且末帧输出完成才回 `played`。串口只记录安全的 decode 返回码、PCM sample 数、入队、播放开始及完成结果，不记录任务或设备标识。本次尚未烧录、未部署。
- 后续真机回归发现，上述 16 kHz announcement 专属校验最初误用于官方对话下行包，导致官方小智回复无声。已将该校验与“重采样不可用即失败”判定严格限定到 announcement 帧；官方对话恢复原有的通用解码和扬声器路径。修复版已完整构建并烧录，写入均通过 Hash 校验、设备自动复位。用户确认本轮完成：官方小智对话与固定 Opus 主动播报均应分别使用各自正确路径；主动播报仍只在所有帧成功输出后发送 `played`。

重新构建并经用户明确授权烧录后，使用 MQTTX 按以下顺序验证：

1. 重启设备，串口应依次出现两条 subscription requested 日志及两条 subscription confirmed 日志；若没有 confirmed，应检查 Broker ACL 或订阅请求，而不是提醒排程。
2. 向 command Topic 发布包含安全 taskId/deviceCode、但协议或音频字段故意无效的 JSON；应看到 `Announcement command rejected. reason=invalid_manifest` 和 ACK queued（`status=failed`），同时在 ACK Topic 收到 `failed/invalid_manifest`。
3. 再发布一个未过期、带 `Z` 或 `.sssZ` UTC 时间的完整固定 Opus manifest 和全部 frame；应看到 `Announcement command accepted`、audio 接收日志、`received` ACK，以及设备 Idle 时的 `played` ACK。若设备正处于对话中，结束对话回到 Idle 后才开始播放。

本轮仅修改固件源码与文档并已烧录验证；未部署、未修改 IoT 后端、群晖、桥接器或 MQTT 凭证。

### 18.2 官方 MCP 的自然语言提醒适配

官方小智语音不会可靠地附带设备编号，也不应要求用户先把“两个小时后”换算成绝对时间。因此群晖桥接器的 `aiot_create_reminder` 工具将 `message` 与 `time_expression` 作为主要参数，`device_code` 改为可选。未指定设备时，桥接器只从私有 `bridge.env` 的 `DEFAULT_DEVICE_CODE` 读取默认目标；真实设备编号不进入源码、镜像或文档。

`DEFAULT_DEVICE_CODE` 必须由桥接启动器显式传入提醒 MCP 子进程，不能只存在于容器父进程环境；否则语音未给出设备编号时会错误报告提醒工具不可用。该转交已在 `0.1.5` 镜像中修复。

桥接器以 `Asia/Shanghai` 当前时间解析 `两分钟后`、`一小时后`、`两天后`、`今天/明天/后天` 的早中晚具体时刻，以及 ISO-8601 时间。无法安全确定的词语（例如“过一会儿”）必须拒绝创建而非猜测。解析后的设备、文本与准确时间继续生成稳定幂等 ID，保持 IoT 后端的重试去重语义。此改动只更新群晖 MCP 桥接器；IoT 容器、小智固件、HiveMQ Topic 和固定 Opus 播报协议均不改变。
