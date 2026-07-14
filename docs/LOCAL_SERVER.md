# 本地服务端部署记录

本仓库只发布固件。服务端使用上游项目
[xinnan-tech/xiaozhi-esp32-server](https://github.com/xinnan-tech/xiaozhi-esp32-server)，不将其源码、模型、密钥或运行数据提交到本仓库。

以下是 Windows 本机以源码方式运行 **Server-only** 的简要记录；完整且随上游更新的说明以
[上游部署文档](https://github.com/xinnan-tech/xiaozhi-esp32-server/blob/main/docs/Deployment.md) 为准。

## 1. 准备环境

安装 Anaconda 或 Miniconda。在 **Anaconda Prompt** 中逐步执行：

```powershell
conda create -n xiaozhi-esp32-server python=3.10 -y
conda activate xiaozhi-esp32-server
conda install libopus -y
conda install ffmpeg -y
```

## 2. 安装依赖

服务端代码单独存放在本机目录，不放入本固件仓库。将下方的 `<服务端根目录>` 替换为你的本机路径：

```powershell
conda activate xiaozhi-esp32-server
cd <服务端根目录>\main\xiaozhi-server
pip install -r requirements.txt
```

按上游文档下载所需的语音识别模型文件。

## 3. 保存私有配置

在 `main/xiaozhi-server` 下创建 `data` 目录，并建立 `data/.config.yaml`。将模型服务、API Key、设备地址等个人配置放在该文件中；它必须只保留在本机或私有的密钥管理处。

不要把下列内容复制到固件仓库或公开 GitHub：

- `data/.config.yaml`、`.env` 与任何 API Key
- 模型文件、运行日志、数据库和语音数据
- Wi-Fi 密码、设备标识、内网地址及端口映射信息

## 4. 启动和验证

```powershell
conda activate xiaozhi-esp32-server
cd <服务端根目录>\main\xiaozhi-server
python app.py
```

保持该窗口运行，按服务端日志中的实际监听地址和端口配置固件。涉及外网访问时，先在局域网验证，再单独评估防火墙、反向代理和密钥保护。
