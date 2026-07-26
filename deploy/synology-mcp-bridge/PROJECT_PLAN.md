# 群晖小智 MCP 项目计划

最后更新：2026-07-25

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

## 当前进行中

- [ ] 添加并配置 Home Assistant 的 `Model Context Protocol Server` 集成。
- [ ] 只向小智开放明确授权的空调实体。

## 下一步

1. 创建 Home Assistant 长期访问令牌。
2. 从小智官方控制台取得 MCP 地址。
3. 仅在群晖本地的 `bridge.env` 中填写两项令牌，并重新构建项目。
4. 用华凌空调验证开关、制冷模式和温度设置指令。
5. 后续再添加灯、插座等设备，并逐个开放对应实体。

## 后续扩展（未开始）

- 天气、网页搜索、日历等常用 MCP 工具。
- 车票查询 MCP：需要选择可靠数据来源；若服务要求登录，凭据只保存在本地。
- Windows 笔记本本地代理：仅允许白名单应用启动、状态查询和摄像头拍照，不开放任意命令执行。

## 安全原则

- 不对公网暴露 Home Assistant 的 MCP 接口。
- 不把令牌、密码或摄像头内容提交到 GitHub。
- Home Assistant 只向 MCP 暴露明确授权的设备实体。
- 笔记本控制必须使用白名单，避免云端模型获得任意 PowerShell 或 Shell 权限。
