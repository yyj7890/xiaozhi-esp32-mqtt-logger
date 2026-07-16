# IOT 远程 MQTT 状态文案与重复事件修复提示词

请继续开发 `D:\AI\IOT` 中的 AIoT-Log-System。开始前完整读取该目录下的 `AGENTS.md`、`PROJECT_CONTEXT.md`、`docs/project-status.md`、`docs/hivemq-remote-mqtt-version.md` 及其他适用代理规则，并检查当前 Git 分支、状态和已有未提交修改。保留所有已有修改，不覆盖、不删除、不重置。

本次只修改 IOT 后端、前端、测试和项目记录，不修改小智固件，不部署群晖，不执行 Git commit、push、创建分支或标签，除非我之后明确允许。不要读取、输出或写入任何真实 HiveMQ 域名、用户名、密码、Token、设备 MAC、局域网 IP、真实 deviceCode 或运行日志正文。公开配置继续只使用占位符。

## 背景

小智远程固件候选版 `v2.2.6-aiot-remote-mqtt.3` 保留现有事件类型以兼容后端状态逻辑，但远程模式使用新的消息文本：

```text
eventType=mqtt_connected
message=Remote log MQTT TLS connected

eventType=mqtt_connection_failed
message=Remote log MQTT TLS connection failed and was retried

eventType=mqtt_reconnected
message=Remote log MQTT TLS connection recovered
```

局域网模式继续发送原消息：

```text
Log MQTT connected
Log MQTT connection failed and was retried
Log MQTT connection recovered
```

Topic、deviceCode 一致性、QoS 和 Payload 其他字段不变。`logType` 仍只允许 `RUNNING`、`ERROR`、`MAINTENANCE`、`INSPECTION`，不得使用 `SYSTEM`。

真机串口脱敏观察已经证明固件每次启动只发布一次 `mqtt_connected`。页面却出现相邻两条“日志 MQTT 已连接”，说明后端运行日志汇总缺少幂等保护；MQTT QoS 1 允许至少一次投递，不能假定消息永不重复。也需要人工确认实际环境没有同时运行多个写入同一数据库的后端订阅实例。

## 修改目标

1. 在后端 `LogServiceImpl.resolveDeviceRuntimeContent` 增加三个远程消息的中文映射：

```text
Remote log MQTT TLS connected
→ 远程日志 MQTT（TLS 8883）已连接

Remote log MQTT TLS connection failed and was retried
→ 远程日志 MQTT（TLS 8883）连接失败，正在重试

Remote log MQTT TLS connection recovered
→ 远程日志 MQTT（TLS 8883）连接已恢复
```

2. 在前端 `LogListView.vue` 的运行日志内容兼容映射中增加同样三项，确保历史英文内容或未经新版后端转换的数据也能正确显示。

3. 保留 `mqtt_connected`、`mqtt_connection_failed`、`mqtt_reconnected` 事件类型及现有故障状态逻辑：
   - 真实失败为 `PENDING`；
   - 恢复事件为 `RESOLVED`；
   - 高严重度作为事件历史保留；
   - 新的非 INFO 事件可以重新变为 `PENDING`。

4. 修复运行事件汇总重复：
   - `startup` / `firmware_started` 仍必须无条件开启新的启动批次；
   - 对准备追加到同一 30 秒汇总窗口的普通运行事件，先生成本次规范化摘要；
   - 如果已有汇总内容的最后一行与本次摘要完全相同，则视为相邻重复投递，不追加内容、不增加“n 条”计数，直接返回现有记录；
   - 不要删除历史记录，不要对不同批次去重，不要把相隔其他事件的相同状态误删；
   - 不要仅根据 Paho `MqttMessage.isDuplicate()` 丢弃消息，因为首次处理失败后的合法重投不能丢失；
   - 不要把 MQTT QoS 从 1 降为 0。

5. 检查是否可能同时运行多个后端 MQTT 订阅实例。只报告实例数量和风险，不输出 Broker、Topic 中的真实设备编号、凭证或日志 Payload；不要擅自停止服务或容器。

## 预期页面结果

远程模式一次正常启动应显示为：

```text
固件开始初始化 → Wi-Fi 已连接 → 官方 AI 协议已连接 → 远程日志 MQTT（TLS 8883）已连接
```

即使 Broker 对同一 QoS 1 消息发生相邻重复投递，也只能保留一个远程连接状态。

局域网模式仍显示原通用状态，不得改成远程状态，也不得影响 UDP `19830` 和本地 Mosquitto TCP `1883`。

## 验证要求

1. 增加或更新后端测试，至少覆盖：
   - 三个远程消息的中文映射；
   - 相邻完全相同事件不重复追加；
   - 不同事件正常追加；
   - `startup` 仍创建新批次；
   - MQTT 失败后为 `PENDING`、恢复后为 `RESOLVED`、之后新错误重新为 `PENDING`。
2. 执行后端 Maven 测试。
3. 执行前端类型检查或生产构建。
4. 执行敏感信息扫描和 `git diff --check`。
5. 更新 `PROJECT_CONTEXT.md`、`docs/project-status.md`、`docs/hivemq-remote-mqtt-version.md`，记录修改、测试和仍需部署验证的事项。
6. 最终明确列出仍需人工完成的新镜像构建、群晖部署、页面验证和多实例检查。

先检查再修改；可以完成代码和测试，但本次不要部署、烧录、提交或推送。
