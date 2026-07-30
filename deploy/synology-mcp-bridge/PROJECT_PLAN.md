# 群晖小智 MCP 项目计划

最后更新：2026-07-26

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

## 当前进行中

- [x] 添加并配置 Home Assistant 的 `Model Context Protocol Server` 集成。
- [x] 只向小智开放明确授权的空调实体。
- [x] 将笔记本 MCP 代理接入群晖桥接器，并在小智控制台验证电脑和摄像头工具。
- [x] 桥接器 `0.1.6` 已部署：提醒工具默认优先使用私有 `DEFAULT_DEVICE_CODE`，避免投递到旧设备。

## 下一步

1. 在小智控制台验证默认提醒目标：创建和查询提醒应始终落到 `DEFAULT_DEVICE_CODE` 指向的设备。
2. 后续添加灯、插座等设备，并逐个开放对应实体。
3. 真实 MQTT 主动播报联调需单独授权后再进行；不与桥接器默认提醒修复混为一项部署。

## 后续扩展（未开始）

- 天气、网页搜索、日历等常用 MCP 工具。
- 车票查询 MCP：需要选择可靠数据来源；若服务要求登录，凭据只保存在本地。
- Windows 笔记本本地代理：仅允许白名单应用启动、状态查询和摄像头拍照，不开放任意命令执行。

## 安全原则

- 不对公网暴露 Home Assistant 的 MCP 接口。
- 不把令牌、密码或摄像头内容提交到 GitHub。
- Home Assistant 只向 MCP 暴露明确授权的设备实体。
- 笔记本控制必须使用白名单，避免云端模型获得任意 PowerShell 或 Shell 权限。

## 2026-07-28 更新：提醒默认目标设备（桥接器 0.1.6）

- 根因：旧桥接器未把 `DEFAULT_DEVICE_CODE` 传给 `aiot-reminders` 子进程，且提醒
  工具没有统一解析默认目标，因此重建容器仍可能投递到官方调用中残留的旧设备。
- 修复：创建和查询提醒时，私有 `bridge.env` 中的非空 `DEFAULT_DEVICE_CODE` 始终优先；
  未配置时才回退调用方传入的 `device_code`；两者为空则安全报错。
- 隐私：真实设备编号、Token 与包含私密参数的 URL 不写入桥接器或审计日志；审计目标
  仅保留由最终目标派生的不可逆摘要。
- 范围：仅修改桥接器，不修改 IoT 后端、小智固件、MQTT 协议或 Home Assistant。

## 2026-07-30 更新：已验证的卧室空调控制（桥接器 0.1.7）

- 排查确认：在 Home Assistant 开发者工具中直接调用 `climate.set_temperature` 能够实际改变
  美的空调温度，因此设备和 Home Assistant 集成本身正常；旧通用 MCP 工具的问题是仅确认
  API 调用已接受，未确认设备最终状态。
- 修复：新增卧室空调的已验证设温度、风速、上下摆风和左右摆风 MCP 工具。每项控制均使用
  Home Assistant REST 动作后回读状态；未确认则返回失败，不再错误报告成功。
- 为避免小智继续选择旧工具，桥接器会隐藏非验证的 `HassClimateSetTemperature`，并在只有
  一个 `climate.*` 实体时自动发现目标；多空调时才需在私有 `bridge.env` 设置
  `DEFAULT_CLIMATE_ENTITY_ID`。
- 范围：仅修改桥接器；未修改 Home Assistant、美的集成、小智固件、IoT 后端或 MQTT 协议。
