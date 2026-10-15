# 独立项目迁移（2026-10-15）

项目根目录：`/Users/yanglipeng/funny/smbwonder-ios`。

- 从参考仓库 `d2r-mobile/ports/smbwonder` 移入全部 Wonder 源码、工具、测试、配置与文档，并将它们提升到独立项目根目录。
- 原 `funny/smbwonder-work` 约19 GB工作文件整体移入本项目 `work/`，没有复制或重新提取游戏内容。
- `vendor/d2r-mobile/dm` 保存参考实现依赖的本地源码快照，许可证在 `vendor/d2r-mobile/LICENSE`。当前保留其目录结构，以便原有改写/验证/着色器工具内部导入仍可工作；该目录不是本项目的开发入口。
- 构建器、XcodeGen 模板、测试和真机脚本均改为从本项目解析路径。今后不应在旁边的 D2R 参考目录修改 Wonder 代码。
- `funny/smbwonder-work` 仅保留一个指向新 `work/` 的兼容符号链接，用于现有 Python 虚拟环境的绝对 shebang 及历史日志/脚本路径。该链接不把实际工作文件留在项目外。
- 原 Downloads 游戏压缩包与个人密钥属于用户原始资料，保持原位置；本项目已有资源副本在 `work/`。
- 原始日志、哈希清单和历史验证 JSON 保留旧路径字符串，避免篡改验收证据。迁移前的源码哈希清单位于 `work/analysis/project-migration/before-source-hashes.json`。
- Git 仅管理源码、测试、文档、配置和依赖源码。`work/`、游戏包、密钥、签名材料和构建产物均忽略。没有创建远程仓库或上传文件。

## 迁移验收

- 新目录执行45项宿主测试全部通过。
- 使用新目录中的构建器和 XcodeGen 成功生成诊断工程；工程中不存在原参考仓库或原工作目录的绝对源码路径。
- 正常 xcodebuild（generic iOS，Debug）通过，签名严格校验通过。
- 本轮只验收迁移、构建和签名，没有重复安装/真机运行；迁移前的最新真机停点仍为 BeginRecording。
- 检查 Git 暂存范围，工作目录、游戏包、密钥与签名材料均未收录。
- 结果文件位于 `work/analysis/project-migration/`。
