# 发布前检查清单

## 许可证与署名

- 保留仓库根目录的 `LICENSE`，不得删除或替换上游版权声明。
- README 明确说明本项目基于 [78/xiaozhi-esp32](https://github.com/78/xiaozhi-esp32)。
- 对新增的第三方字体、图片、模型、代码或开发板定义，确认各自的许可证允许再发布。

## 不提交的本机文件

现有 `.gitignore` 已排除 `build/`、`sdkconfig`、`.env`、`dependencies.lock` 等本机生成或私有文件。提交前仍要检查暂存区，确保没有：

- API Key、访问令牌、私钥、证书、Wi-Fi 密码或设备序列号；
- `sdkconfig`、构建输出、二进制固件、日志、缓存；
- 从本地服务端复制来的 `.config.yaml`、模型和数据库文件。

## 首次发布步骤

1. 在此目录运行 `git init -b main`。
2. 运行 `git add .` 后，先用 `git status` 人工核对即将提交的文件。
3. 用说明性的提交信息创建首个提交，例如 `git commit -m "Initial release: customized XiaoZhi firmware"`。
4. GitHub 新建一个空仓库（不预置 README、License 或 `.gitignore`），添加远程地址后推送。
5. 推送后在 GitHub 网页再次检查代码、提交历史和 Actions 日志，确认没有敏感信息再改为公开。

首次提交建议先建为私有仓库。公开后，每次发布前重复执行 `git status` 和 `git diff --cached`。
