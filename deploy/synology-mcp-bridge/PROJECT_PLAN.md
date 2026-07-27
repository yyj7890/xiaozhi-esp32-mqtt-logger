# 群晖小智 MCP 项目计划

最后更新：2026-07-27

## 项目目标

让官方小智 AI 通过群晖上持续运行的 MCP 桥接器，安全调用 Home Assistant 来控制家中的灯、空调和其他已授权设备。

笔记本应用控制和摄像头功能将由笔记本本地代理单独提供；笔记本离线时，这部分功能不可用。

## 已完成

- [x] 确定架构：官方小智 AI → 群晖 MCP 桥接器 → Home Assistant → 智能家居设备。
- [x] 为 DS920+ / DSM 7.2 准备 Docker Compose 部署文件。
- [x] 构建并导入 `local/xiaozhi-mcp-bridge:0.1.0` 预构建镜像。
- [x] 诊断小智接入点反复返回 HTTP 401：镜像安装了不兼容的 `websockets 16.1.1`。
- [x] 在 `0.1.1` 镜像中固定 `websockets==14.2`，兼容官方小智 MCP WebSocket 接入点。
- [x] 在群晖 Container Manager 创建 `xiaozhi-mcp-bridge` 项目。
- [x] 将部署文件与说明提交到 `feature/synology-mcp-bridge` 分支。
- [x] 排除 `bridge.env`，避免 MCP 地址和 Home Assistant 令牌进入 GitHub。
- [x] 离线导入并启动 Home Assistant，完成管理员账号初始化。
- [x] 安装 `Midea Smart AC 2026.7.1`，接入华凌空调并验证状态读取与控制。
- [x] 通过小智官方 MCP 成功控制空调。
- [x] 完成 Windows 笔记本 MCP 本地代理：状态、白名单应用启动、当前摄像头画面和拍照。
- [x] 为常用应用加入独立语音启动工具，避免通用应用工具被误判为管理员操作。
- [x] 为 WeGame、无畏契约、英雄联盟加入“笔记本本机确认后启动”工具，不授予云端管理员权限。
- [x] 将远程 MQTT 主动播报固件与桥接器源码整合到 `feature/aiot-mcp-reminder-integration`。
- [x] 增加 `aiot_create_reminder`、`aiot_list_reminders`、`aiot_cancel_reminder` 工具；桥接器以 IoT REST API 保存和查询提醒，不保存 MQTT 凭据。
- [x] 为 Home Assistant 与笔记本 MCP 增加安全操作摘要审计，失败不影响原有工具调用。

## 当前进行中

- [x] 添加并配置 Home Assistant 的 `Model Context Protocol Server` 集成。
- [x] 只向小智开放明确授权的空调实体。
- [ ] 在群晖本地配置 `PC_MCP_URL` 和 `IOT_API_URL`，并在小智控制台验证笔记本工具、提醒工具与审计记录。

## 下一步

1. 在笔记本的 `agent-status.txt` 取得 Tailscale MCP 地址，并在群晖私有 `bridge.env` 设置 `PC_MCP_URL`、`IOT_API_URL`。
2. 在用户明确授权后重建桥接容器；本次代码提交不执行部署。
3. 在小智控制台验证 `pc_get_status`、白名单应用、`pc_see_camera`、`pc_take_photo` 与三项提醒工具。
4. 在 IoT 页面核对只记录工具名、目标摘要、结果摘要和状态，且不记录工具参数、音频或凭据。
5. 后续添加灯、插座等设备，并逐个开放对应实体。

## 后续扩展（未开始）

- 天气、网页搜索、日历等常用 MCP 工具。
- 车票查询 MCP：需要选择可靠数据来源；若服务要求登录，凭据只保存在本地。
- Windows 笔记本本地代理：仅允许白名单应用启动、状态查询和摄像头拍照，不开放任意命令执行。
- 群晖本地 TTS：生成 16 kHz 单声道、60 ms 裸 Opus packet；动态语音上线前需要人工试听、MQTT 互操作和官方 AI 回归。
- 温湿度、日志和天气条件提醒，以及静音时段、冷却与重试策略。

## 安全原则

- 不对公网暴露 Home Assistant 的 MCP 接口。
- 不把令牌、密码或摄像头内容提交到 GitHub。
- Home Assistant 只向 MCP 暴露明确授权的设备实体。
- 笔记本控制必须使用白名单，避免云端模型获得任意 PowerShell 或 Shell 权限。
- 审计仅记录有限长度的工具名、目标与结果摘要；不得记录令牌、密码、原始音频、摄像头内容或完整对话。
