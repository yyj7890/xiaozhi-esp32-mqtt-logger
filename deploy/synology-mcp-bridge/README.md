# 群晖小智 MCP 桥接器

适用于 Synology DS920+ / DSM 7.2 Container Manager。

它使用小智官方示例项目的 `mcp_pipe.py`，将 `xiaozhi.me` 官方 MCP 接入点连接到群晖本机的 Home Assistant MCP Server。

## 架构

```text
小智官方 AI
  -> xiaozhi.me MCP 接入点
  -> 群晖 xiaozhi-mcp-bridge
  -> Home Assistant /api/mcp -> 灯、空调、插座等已开放实体
  -> Windows MCP 代理 -> 白名单笔记本操作
  -> IoT API -> 提醒持久化/排程 -> HiveMQ -> 小智固定 Opus 主动播报
```

连接器只主动访问互联网，不需要公网 IP、DDNS 或路由器端口映射。

## 部署

1. 将整个目录上传到群晖，例如 `/volume1/docker/xiaozhi-mcp-bridge`。
2. 若已导入预构建镜像 `synology-mcp-bridge-amd64-0.1.1.tar`，将 `compose.prebuilt.yaml` 改名为 `compose.yaml`。
3. 在 Container Manager 的“项目”中，以该目录的 `compose.yaml` 创建项目。
4. 访问 `http://群晖局域网IP:8123`，完成 Home Assistant 初始设置。
5. 在 Home Assistant 添加 `Model Context Protocol Server` 集成，只向 Assist 暴露需要控制的实体。
6. 复制 `bridge.env.example` 为 `bridge.env`，填入小智 MCP 地址和 Home Assistant 长期访问令牌，然后重新构建项目。
7. 如需接入笔记本代理，在同一文件增加 `PC_MCP_URL=http://笔记本Tailscale地址:8765/mcp`。该项留空时不提供笔记本工具。
8. 仅在 IoT 后端已按对应版本运行时，设置 `IOT_API_URL`，桥接器才会提供 `aiot_create_reminder`、`aiot_list_reminders` 和 `aiot_cancel_reminder`；同时 Home Assistant 与笔记本工具的执行会以不含参数和凭证的摘要写入 IoT 审计记录。

## 密钥安全

`bridge.env` 已被 `.gitignore` 忽略，绝不能提交到 GitHub。不要公开 MCP 地址中的 token 或 Home Assistant 长期访问令牌。

## 仓库与分支说明

本目录保存在主项目的 `feature/synology-mcp-bridge` 分支中。Git 分支会保留主项目的完整基础文件；本分支相对基础分支新增的内容只有 `deploy/synology-mcp-bridge` 目录。这样可以将群晖 MCP 部署与小智主项目放在同一个版本历史中，同时不影响其他分支。

部署进度和后续计划请查看 [PROJECT_PLAN.md](PROJECT_PLAN.md)。

## 笔记本功能

群晖负责长期在线的智能家居工具。打开 Windows 应用或调用笔记本摄像头，仍需笔记本上运行独立代理；笔记本离线时这些工具不可用。只应提供白名单操作，例如 `pc_open_app`、`pc_get_status`、`pc_take_photo`，不要开放任意 Shell 或 PowerShell 执行能力。

## 提醒工具与边界

提醒工具经本桥接器调用 IoT 后端 REST API；IoT 后端才负责保存提醒、到时发布 MQTT 和记录设备 ACK。创建工具需要明确的 `device_code`、文本和 ISO-8601 时间，并从这三项生成稳定的幂等 ID，安全重试不会重复创建。

本仓库的桥接器只提供源码与配置样例：未部署群晖 TTS，未替换官方小智 AI，也不会自行发布音频。当前固件/后端阶段只验证固定测试 Opus；动态文本转 Opus、条件提醒和生产部署均须在后续人工核对后单独启用。
