# 群晖小智 MCP 桥接器

适用于 Synology DS920+ / DSM 7.2 Container Manager。

它使用小智官方示例项目的 `mcp_pipe.py`，将 `xiaozhi.me` 官方 MCP
接入点连接到群晖本机的 Home Assistant MCP Server。

## 架构

```text
小智官方 AI
  -> xiaozhi.me MCP 接入点
  -> 群晖 xiaozhi-mcp-bridge
  -> Home Assistant /api/mcp -> 灯、空调、插座等已开放实体
  -> IoT /api/reminders -> HiveMQ MQTT -> 小智主动播报
```

连接器只主动访问互联网，不需要公网 IP、DDNS 或路由器端口映射。

## 1. 准备文件

将整个目录上传到群晖，例如：

```text
/volume1/docker/xiaozhi-mcp-bridge
```

目录中已经包含待填写的 `bridge.env`。先不用填写，等 Home Assistant
初始化完成后再替换其中的两个 `REPLACE_ME`。

## 2. 在 Container Manager 创建项目

### 在线构建

1. 打开“Container Manager”。
2. 进入“项目”，选择“新增”。
3. 项目名称填写 `xiaozhi-mcp`。
4. 路径选择上传后的目录。
5. Compose 文件选择目录中的 `compose.yaml`。
6. 构建并启动项目。

首次构建需要群晖可以访问 GitHub 和 PyPI。

### 使用预构建镜像

如果同时拿到了 `synology-mcp-bridge-amd64-0.1.6.tar`：

1. 在 Container Manager 的“映像”页面从文件导入该 TAR。
2. 创建项目时选择 `compose.prebuilt.yaml`。

这种方式不需要群晖从 GitHub 构建桥接器，但仍需拉取 Home Assistant
官方镜像。

## 3. 初始化 Home Assistant

浏览器访问：

```text
http://群晖局域网IP:8123
```

创建管理员账号，然后添加家里的灯、空调、插座等集成。

在 Home Assistant 中：

1. 打开“设置 -> 设备与服务”。
2. 点击“添加集成”。
3. 搜索并添加 `Model Context Protocol Server`。
4. 只向 Assist 暴露希望小智控制的实体。
5. 在用户资料的“安全”页面创建长期访问令牌。

Home Assistant 只允许 MCP 控制已暴露的实体。

## 4. 配置桥接器

编辑 `bridge.env`：

```dotenv
MCP_ENDPOINT=wss://小智官方地址/?token=真实令牌
HA_MCP_URL=http://127.0.0.1:8123/api/mcp
HA_TOKEN=Home_Assistant_长期访问令牌
IOT_API_URL=http://127.0.0.1:8080
DEFAULT_DEVICE_CODE=你的当前小智设备编号
```

`DEFAULT_DEVICE_CODE` 是提醒默认投递目标。设置后，它会优先于官方小智
MCP 调用中可能残留的设备编号；因此创建和查询提醒都会使用该默认目标。
它必须只写在群晖私有的 `bridge.env`，不要上传到 GitHub，也不要截图公开
其中的令牌或设备编号。

桥接器额外注册 `aiot_create_reminder`、`aiot_list_reminders` 和
`aiot_cancel_reminder`。这些工具只调用 IoT 后端的提醒 API；创建提醒时会
由桥接器根据设备、文本和时间生成稳定幂等标识，避免官方 MCP 重试重复创建。
IoT 后端不可用时，仅提醒工具返回错误，Home Assistant 和笔记本工具不受影响。
Home Assistant、笔记本和 IoT 提醒工具的调用会向 IoT 后端写入脱敏操作记录；只记录工具名、目标摘要和成功/失败，不记录 Token 或完整参数。

在 Container Manager 中重新构建项目，以便重建容器并加载新的环境变量。
仅点击“重启容器”不会重新读取修改后的 `bridge.env`。

## 5. 验证

桥接器日志应依次出现：

```text
Home Assistant is reachable at 127.0.0.1:8123
Starting Xiaozhi MCP bridge for Home Assistant
Successfully connected to WebSocket server
```

然后在小智控制台查看 MCP 工具状态，并用一盏灯进行测试：

```text
打开客厅灯
关闭客厅灯
```

确认成功后，再开放空调、插座等实体。

## 笔记本功能

群晖负责长期在线的智能家居工具。打开 Windows 应用和调用笔记本摄像头，
仍由笔记本上的 MCP 代理提供；笔记本离线时这些工具不可用。

建议笔记本代理只提供白名单工具，例如：

```text
pc_open_app
pc_get_status
pc_take_photo
```

不要向云端模型开放任意 Shell 或 PowerShell 执行能力。

## 变更记录

### 0.1.6（2026-07-28）

- 修复提醒目标仍使用旧设备的问题：桥接器会把私有 `bridge.env` 中的
  `DEFAULT_DEVICE_CODE` 显式传给提醒 MCP 子进程。
- 创建提醒和查询提醒均先解析最终目标：默认编号非空时始终优先；未设置默认
  编号时才回退到官方小智 MCP 调用传入的 `device_code`；两者皆空时安全报错。
- 请求幂等编号、IoT API 的 `deviceCode` 与审计目标摘要均基于最终目标；审计
  摘要只保存不可逆指纹，不保存真实设备编号、Token 或含私密参数的 URL。
- 新增单元测试，覆盖默认优先、回退传入编号、无目标报错与摘要脱敏。
- 此版本只修改桥接器；未修改 IoT 后端、小智固件、MQTT 协议或 Home Assistant。
