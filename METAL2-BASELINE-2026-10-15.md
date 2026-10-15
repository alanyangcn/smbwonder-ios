# Metal 2 当前进度

已按用户确认的路线切换：先用 FlyMe（iPhone 8 Plus / A11 / iOS 16.7.16）推进 Metal 2，未来再接 Metal 3。当前没有新的硬件停点；以前的 Metal 3 能力不足记录是历史结果，不再作为 Metal 2 的启动条件。

## 真机已验证

最终 run `38BEF81F-D9F5-48B7-BB35-3B3406961772`、PID `21516`、启动时 `debugger=1`，之后正常分离调试器。不是独立冷启动验收。

- 原生 SDK、构造函数、nnMain、四个真实 RomFS 资源读取继续通过。
- 在 `nv::InitializeGraphics` 边界选择 `metal2`；不依赖 Metal3 GPU 家族、直接 GPU 地址或资源 ID。
- A11 使用 MSL 2.1、普通顶点/片元缓冲槽、纹理和采样器槽完成实际绘制，等待 GPU 完成，再 blit 到共享缓冲回读。
- 64×64 共 4096 个像素全部正确；改用错误常量的第二次真实 GPU 提交得到 4096 个错误像素，负对照被拒绝。导出的四色 PNG 是诊断图，不是游戏首帧。
- 从 Wonder 资源选取三组体积较小的顶点/片元对，共六个唯一阶段；验证 NVN 前缀和 Maxwell SPH 阶段后，离线转成 SPIR-V，再转成使用独立资源槽的 MSL 2.1。这些样本按体积选取，尚未证明是实际启动绘制使用的变体。
- 六份 MSL 在 **Apple A11 GPU** 上全部成功编译并找到对应类型的 `main0`：`compiled=6 total=6 runtime_eligible=0`。

## 代码与证据

- `runtime/graphics_gate.c`：Metal 2/3 路径选择；诊断成功仍停在 `GAME_RENDERER_PENDING`，不假装 NVN/游戏渲染已经初始化。
- `runtime/metal2_probe.m`、`metal2_pixels.c`：真实 GPU 绘制、回读、像素正/负对照及 PNG/JSON 输出。
- `runtime/metal2_shader_check.m`：核对 MSL SHA-256、在设备编译、校验入口阶段，写独立结果。
- `tools/prepare_shader_jobs.py`：从有边界校验的 BNSH 提取阶段和控制数据；所有反射事实明确标记未验证。
- `tools/translate_shader_jobs.py`：在全新临时目录翻译，记录输入、工具 DLL、SPIR-V、反射哈希。
- `tools/compile_metal2_shaders.py`：校验完整翻译证据后生成独立槽位 MSL；拒绝空/缺失/重复/错阶段 jobs 和被替换的 SPIR-V。
- `metal2-device-result.json`：本次 App、源码、工具与日志哈希。

日志在 `/Users/yanglipeng/funny/smbwonder-ios/work/analysis/metal2-final/Documents/loader-probe.log`；GPU 结果在同目录 `metal2-probe.json`；目标编译结果在 `Metal2ShaderSources/device-compile-result.json`。最初 GPU 诊断图在 `analysis/metal2-run1/Documents/metal2-probe.png`。

宿主测试 **35 项通过**，包含 Intel Iris Pro 上的实际 GPU 测试；iOS 构建、签名校验、安装通过。审查发现的 SPIR-V 来源绑定和空清单假阳性已修复并通过负对照复验。

## 工具与编译边界

本地工作区安装了 .NET SDK 10.0.402，编译仓库自带的 Ryujinx 着色器翻译器；SPIRV-Cross 源码来自 KhronosGroup，提交 `300681bbcb8365a8d1eacdacdbdd0249bbf1d6b1`。工具和派生游戏着色器均留在本地 `smbwonder-work`，不作为源码附件公开。

Xcode 的离线 Metal Toolchain 尚未安装：普通 `xcrun metal` 尝试失败，不能算 metallib 已构建。本轮用 `--source-only` 明确记录只生成 MSL，再由设备的 Metal 接口实际编译 GPU 着色器。这不涉及运行时生成可执行 ARM64 客体代码。

## 尚未接通：游戏的实际绘制

当前 `GAME_RENDERER_PENDING` 是实现边界，不是换机要求。游戏尚无画面或可玩流程。

1. 接通 Wonder 的 NVN 取址、设备/资源对象和 VI/CAMetalLayer 显示路径，记录实际调用及状态；未实现调用必须明确停止。
2. 用 BNSH/BFSHA 反射及实际 NVN 状态核实顶点输入格式、常量缓冲、纹理/采样器槽与坐标约定。当前自动生成反射含翻译器默认假设；六个阶段“能编译”不能替代正确性验证，故 `runtime_eligible=false`。
3. 将真实顶点、常量、贴图与目标着色器组成第一条实际 draw，核对图像。现在的四色 GPU 探针不包含这一步。
4. 按实际路径补充线程/同步、输入、音频、存档及内存预算，再验证菜单与可玩流程。

未来接 Metal 3 时，共用游戏接口、资源解析与反射；设备能力和绑定方式分别实现。目前 Metal 3 选项保留原能力检查及未接入提示，未宣称已支持游戏渲染。

## 重建

在 `/Users/yanglipeng/funny/d2r-mobile` 执行：

```sh
python3 -m unittest discover -s tests -q
DEVELOPER_DIR=/Applications/Xcode-26.6.0.app/Contents/Developer python3 tools/build_loader_probe.py --stage main --graphics-backend metal2 --xcodegen /Users/yanglipeng/funny/smbwonder-ios/work/tools/xcodegen/xcodegen/bin/xcodegen
```

工程：`/Users/yanglipeng/funny/smbwonder-ios/work/build/loader-probe/project/WonderLoaderProbe.xcodeproj`，scheme `WonderLoaderProbe`。指定 `--graphics-backend metal3` 可选择未来路径，目前会在未实现处明确停止。

目标着色器链依次运行 `prepare_shader_jobs.py` → `translate_shader_jobs.py` → `compile_metal2_shaders.py --source-only`，详细参数见各工具 `--help`。将生成目录上传到 App `Documents/Metal2ShaderSources`；App 在真实游戏图形初始化边界执行来源校验和编译检查，不会将这批探索产物直接用于游戏绘制。
