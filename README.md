# Super Mario Bros. Wonder — Native iOS Port

《超级马里奥 惊奇》1.2.0 的原生 iOS 移植研究项目。当前目标为 FlyMe（iPhone 8 Plus / A11 / iOS 16.7.16），先推进 Metal 2 / MSL 2.1。

当前真机停点：`nvnCommandBufferBeginRecording`（main+0x5c388）。已通过 SDK 启动、资源读取、采样器设置与首次控制/命令内存绑定；尚无游戏首帧，也未达到可玩状态。

## 目录

- `runtime/`：Wonder 专用内核、SDK 服务、NVN 与 Metal 适配。
- `ios-probe/`：iOS 宿主界面和 XcodeGen 工程模板。
- `tools/`：构建、真机验证、资源与着色器分析工具。
- `tests/`：宿主回归测试和 GPU 诊断。
- `vendor/d2r-mobile/`：本地保存的 D2R Mobile 依赖源码及原许可证；构建不读取旁边的参考仓库。
- `work/`：本地游戏资源、工具链、构建产物、设备日志与历史验证证据，全部排除出 Git。

进度见 [METAL2-STATUS.md](METAL2-STATUS.md)，审查见 [REVIEW-2026-10-15.md](REVIEW-2026-10-15.md)，迁移说明见 [MIGRATION.md](MIGRATION.md)。

## 常用命令

在项目根目录执行：

```sh
python3 -m unittest discover -s tests -q
./tools/device_round.sh <新的轮次名>
```

只构建诊断工程：

```sh
export DEVELOPER_DIR=/Applications/Xcode-26.6.0.app/Contents/Developer
python3 tools/build_loader_probe.py --stage main --graphics-backend metal2 \
  --xcodegen "$PWD/work/tools/xcodegen/xcodegen/bin/xcodegen"
```

生成的 Xcode 工程位于 `work/build/loader-probe/project/WonderLoaderProbe.xcodeproj`。真机轮次默认使用 LLDB 启动；不等同于独立冷启动验收。

本地开发者 Team ID、设备 ID 与 App ID 当前延续已验证配置。游戏文件和密钥由使用者自行在本地提供；本仓库不包含这些内容。原始 Downloads 文件不因项目迁移而移动。
