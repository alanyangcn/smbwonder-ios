# Super Mario Bros. Wonder — Native iOS Port

《超级马里奥 惊奇》1.2.0 的原生 iOS 移植研究项目。当前目标为 iPhone 18 Pro（iOS 27），渲染走 Metal 3 / MSL 3.2。

当前进度：可在 iPhone 18 Pro 上从标题一路玩到各世界关卡，画面、文字、声音、存档、触屏与手柄输入正常；最重的世界地图视角约 60 fps（长时间满载后发热降频）。详见 [STATUS.md](STATUS.md)。

**移植方法论**（从零到可玩的流程、难点与解法，可用于其他游戏 / 平台）：[docs/PORTING-METHODOLOGY.md](docs/PORTING-METHODOLOGY.md)。

**新机器搭建（工具链、导入自己的游戏包、着色器普查迁移）见 [SETUP.md](SETUP.md)。**

## 目录

- `runtime/`：Wonder 专用内核、SDK 服务、NVN 与 Metal 适配。
- `ios-probe/`：iOS 宿主界面和 XcodeGen 工程模板。
- `tools/`：构建、真机验证、资源与着色器分析工具。
- `tests/`：宿主回归测试和 GPU 诊断。
- `vendor/d2r-mobile/`：本地保存的 D2R Mobile 依赖源码及原许可证；构建不读取旁边的参考仓库。
- `work/`：本地游戏资源、工具链、构建产物、设备日志与历史验证证据，全部排除出 Git；由 `tools/setup_env.sh`、`tools/import_game.sh`、`tools/local_state.sh` 重建（见 SETUP.md）。

进度见 [STATUS.md](STATUS.md)，审查见 [REVIEW-2026-10-15.md](REVIEW-2026-10-15.md)，迁移说明见 [MIGRATION.md](MIGRATION.md)。

## 常用命令

在项目根目录执行：

```sh
python3 -m unittest discover -s tests -q
./tools/device_round.sh <新的轮次名>
```

只构建诊断工程：

```sh
export DEVELOPER_DIR=$(xcode-select -p)
python3 tools/build_loader_probe.py --stage main --graphics-backend metal2 \
  --xcodegen "$PWD/work/tools/xcodegen/xcodegen/bin/xcodegen"
```

生成的 Xcode 工程位于 `work/build/loader-probe/project/WonderLoaderProbe.xcodeproj`。真机轮次用 `xcrun devicectl device process launch --console` 启动并记录 App 输出。

本地开发者 Team ID、设备 ID 与 App ID 当前延续已验证配置。游戏文件和密钥由使用者自行在本地提供；本仓库不包含这些内容。原始 Downloads 文件不因项目迁移而移动。
