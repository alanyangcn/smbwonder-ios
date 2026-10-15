> 最新进度见 [METAL2-STATUS.md](METAL2-STATUS.md)：A11 已通过 Metal 2 GPU 绘制/回读，以及六个目标着色器阶段的设备编译检查；游戏首帧仍待接通。

> 最新持续推进记录见 [ADVANCE-STATUS.md](ADVANCE-STATUS.md)：已越过核心掩码阻塞并在真机读取真实 RomFS；下文保留此前冷启动验证记录。

# Super Mario Bros. Wonder → iOS

**最新进度：无调试器冷启动、独立 1 MiB 主线程、SDK 和两个模块 DT_INIT 均已通过，并在 nnMain 中执行真实接口调用。** 当前停在未实现的 SetThreadCoreMask（SVC 0x0f），界面显示 BOOT_BLOCKED；游戏完整启动及 RomFS/图形等后续接口未完成。详见 [BOOT-STATUS.md](BOOT-STATUS.md) 和 [boot-device-result.json](boot-device-result.json)。25 项宿主回归测试通过。

**上一阶段记录：SDK 初始化链已在 FlyMe 上通过（使用宿主服务初始化后端）。** 最终版真机日志到达 `SDK_INIT_COMPLETE`，20 项宿主回归测试通过；全局构造函数与 nnMain 尚未执行。详细范围见 [SDK-STATUS.md](SDK-STATUS.md)，证据和构建哈希见 [sdk-device-result.json](sdk-device-result.json)。下文保留较早阶段记录，不代表最新状态仍停在单条 RET。

目标：以 D2R Mobile 的离线改写、Horizon 宿主、NVN→Metal 和签名映射方法，实现用户本地游戏在 iOS 上原生运行。最终目标由用户报告为 iPhone 18 Pro / iOS 27，尚未连接验证；当前使用已连接的 FlyMe（iPhone 8 Plus，实测 iOS 16.7.16）调试 CPU 与装载。参考图形后端需要 A14/M1，不能在 A11 上验收完整渲染。

本目录是正在进行的移植工作，不是完成的 iOS 移植或可玩安装包。

## 已核验的输入与环境

- 输入：用户提供的本体与更新 NSZ。原始包的 SHA-256、PFS0 条目及更新内置 CNMT XML 元数据见 `input-inventory.json`。
- Application ID：`010015100B514000`；Patch ID：`010015100B514800`；更新 CNMT 版本整数 `262144`。Control/NACP 实际显示版本已确认是 **1.2.0**。
- 工作区：`/Users/yanglipeng/funny/smbwonder-ios/work`。游戏派生文件、工具、缓存和私有密钥配置仅放此目录，不提交到参考仓库。
- 原始游戏文件和用户提供的密钥文件不修改；不输出密钥值。
- 本机是 x86_64 / macOS 26.7.1，不能原生执行客体 ARM64；可用 Xcode 26.6 和 iPhoneOS 26.5 SDK 交叉编译，后续执行验证需要目标 iOS 设备或 Apple Silicon Mac。
- 已交叉编译 iOS 16 ARM64 装载探针，并用 Xcode 现有开发者账户完成诊断工程构建；已安装到 FlyMe，真机装载、重定位和单条 SDK RET 执行通过；尚未启动游戏。

## 分阶段实施与通过条件

1. **输入与静态分析**：解压 NSZ；验证 NCA；将更新的 ExeFS 和与本体合并后的 RomFS 放入独立目录。记录真实 NSO module ID、哈希、段、动态符号、重定位、NPDM 架构标志及目标 shader 格式。
2. **CPU 改写**：按实际模块执行 native 改写，确认没有误用 D2R 业务表。对支持路径运行结构和指令等价检查；新指令形态必须先有对应测试。Unicorn 结果不替代真实并发、宿主 SVC 或 GPU 验证。
3. **独立宿主引导**：从参考实现抽出装载、线程环境和必要 HLE；按目标 SDK 的符号及初始化路径实现启动。按游戏身份选择功能，目标不使用 D2R 固定地址、私有门、控件或存档代码。
4. **图形闭环**：从实际资源分析容器、Maxwell 代码与反射；建立目标自己的 NVN 调用表和 shader 表。先绘制目标画面，再验证多个场景；D2R 的 shader_facts 和手写内嵌 shader 不可沿用。
5. **iOS App**：生成独立包名/工程；构建时嵌入并签名客体代码，恢复原相对布局，重定位只写数据。接标准手柄输入、音频、沙箱资源/存档和生命周期。
6. **验收**：真机冷启动、实际关卡操作、复杂画面、声音、存档重启、多次后台恢复。逐项列明通过范围；没有目标原版对照数据时不宣称完整行为等价。

## 参考源码中已确认的改造点

| 来源 | 已确认问题 | 实施约束 |
|---|---|---|
| `dm/tools/extract_game.sh` | 只认 XCI/NSP；写死 D2R Title ID、版本和三模块 SHA | 不直接用于惊奇；用独立解包流程，按 NCA 类型和实际 ID 识别 |
| `dm/tools/product.sh` | 固定 main/sdk/subsdk0 与 touch-run-v4 | 按实际 ExeFS 模块出产物，不调用 D2R 全流水线 |
| `dm/host/main.c:guest_main` | 存档节流、种子、padmode 及装载后回调 | SDK 引导与游戏钩子分离，未知版本不读写固定地址 |
| `dm/host/hle_oe.c:scale3d_tick` | 获取通知会访问 D2R renderer 和函数地址 | 新游戏绕过整个专用路径，不仅设置缩放环境变量 |
| `dm/host/loader.c:relocate` | 未完整拒绝未知重定位类型 | 静态盘点目标类型，再以白名单处理和测试 |
| `dm/host/thread.c` | 固定 TLS/TLR 尺寸及 D2R 宽高比槽 | 依据目标 SDK 检查布局；通用线程环境与显示配置分离 |
| `dm/host/Makefile` | 自动生成 D2R 数据表；同时编入全部玩法和触屏 | 新宿主独立列源文件，避免用空表掩盖缺失依赖 |
| `dm/gfx/nvn/hle_nvn.c:g_nops` | 多项取址成功的 NVN 函数为空实现 | 按惊奇实际需求补齐，不能只以无 UNIMPL 判通过 |
| `dm/gfx/tools/dmsh_build.py` | D2R 包格式、反射表和 shader 规则 | 新游戏重新采集与翻译，不以表替换伪造覆盖 |
| `dm/ios/Sources/main.m` | D2R 资源清单和 UI 桥接 | 重做惊奇的资源定位、验证与输入，不复用 D2R 壳出假包 |

## 工具来源

- NSZ CLI：<https://github.com/nicoboss/nsz>，本地 Python venv 安装版本 5.0.0。
- hactool：<https://github.com/SciresM/hactool>，本地源码提交 `1d64a83450e025622f3468c28fc4164dad2c5ef6`，本机编译。
- NSZ 解压、NCA 提取只在本机执行。工具日志位于外部工作区；私有日志不作为公开附件。

状态以工作区实际日志和后续产物为准；此文件中的后续阶段均是实施目标，不构成通过声明。

## 当前验证结果与入口

- 更新合并完成：`game/update-262144-nstool/0` 为 ExeFS，`1` 为合并 RomFS（19,034 个文件）。游戏文件保存在外部工作区。
- `main` / `sdk` 离线改写均未匹配 D2R CUTS、OBS 或 PARAMS。两模块独立 LLVM 结构检查通过；Unicorn 等价检查合计 34,546 个站点，600 个变异样本均被检出。宿主门只做结构检查，这些结果不证明 SVC 或并发语义正确。
- 修复了验证器把整个 TSD 映射内的客体写入误删的问题，现在只排除改写器的确切 8 字节内部槽写入；保留了复现回归用例。Xcode 缺少 llvm-objcopy 时使用 clang 包装 Mach-O 后独立反汇编，未放松结构规则。
- `verified-inputs.json` 固定原 NSO、改写映像、布局/门元数据及四份通过日志的 SHA-256；`tools/build_loader_probe.py` 在构建前逐项验证，不接受换版产物。
- `ios-probe/LoaderProbe.m` 只链接 loader、hle、entry，未链接 D2R main/thread/玩法文件。装载后只允许调用事先核验为单条 RET 的 `_init_libc0`，明确停止于 SDK 初始化、构造函数和 nnMain 之前。弱符号解析等装载器后续完善项仍未处理，不能用探针通过证明游戏启动正确。
- shader 已识别为 Zstandard 包装的 BFSHA/SARC，包含 BNSH；尚未转换为 MSL，也没有目标游戏画面。

可重复构建（游戏派生文件只写入外部工作区）：

```bash
export DEVELOPER_DIR=/Applications/Xcode-26.6.0.app/Contents/Developer
python3 tools/build_loader_probe.py --work-root /Users/yanglipeng/funny/smbwonder-ios/work --xcodegen /Users/yanglipeng/funny/smbwonder-ios/work/tools/xcodegen/xcodegen/bin/xcodegen
python3 -m unittest discover -s tests -v
```

Xcode 工程生成于 `work/build/loader-probe/project/WonderLoaderProbe.xcodeproj`。真实构建命令及签名结果在 `work/analysis/probe-xcode-build.log`，安装/启动日志在 `probe-device-launch.log`。第一次安装暴露可执行文件名与清单不一致，已将独立 Xcode 清单绑定到 `$(EXECUTABLE_NAME)`；保留诊断日志。

## FlyMe 真机结果（2026-10-15）

Xcode 工程 `BUILD SUCCEEDED`，`codesign --verify --strict` 通过，ios-deploy 返回 `InstallComplete`。从 App Documents 拉取的真实日志包含：

```text
MAPPED_OK: 2 modules; signed text + writable relocations
GUEST_RET_OK: executed the signed, remapped ARM64 SDK ret instruction
PROBE_COMPLETE: stopped before SDK initialization, constructors and nnMain
```

main 重定位 relative=368204、abs/glob=2782、jump=1056；sdk 为 10988、15207、8935；force 表为 0。该日志证明映射、重定位和一次 RET 返回完成，不证明整个模块代码都执行正确。自动调试启动过程出现连接/退出异常，App 日志未标记具体启动途径，用户随后确认从主屏幕打开并看到同一 `PROBE_COMPLETE`，拉回日志也一致；未明确控制强制结束后的新进程启动，因此**强制结束后的冷启动验收仍需另行记录**。证据路径与哈希见 `device-result.json`。

新增 `tools/inspect_bnsh.py` 对实际更新资源建立逐变体、逐阶段清单，检查所有读取范围并记录控制段/代码段 SHA-256。两份资源共 8,801 个变体、17,602 个阶段引用、4,882 个不同代码载荷，当前只有 vertex/fragment。完整清单位于外部工作区 `analysis/shader-inventory-262144.json`；这不是已翻译 shader 缓存。17 项回归测试通过。

下一阶段：独立冷启动复核 → 目标 SDK 初始化路径与线程布局 → 严格 HLE 接管配置（未实现调用立即中止，保留首个失败）→ BNSH 反射与 Maxwell→MSL。A11 只用于 CPU/装载调试；参考 Metal 3 后端的完整画面验证需要后续连接 A14 或更新设备。

## 手动打开与下一段 SDK 启动分析

用户已直接确认：从 FlyMe 主屏幕打开后，最后一行为 `PROBE_COMPLETE: stopped before SDK initialization, constructors and nnMain`。重新取得 Documents 日志，与成功标记一致；详细证据更新于 `device-result.json`。未把前台打开等同于经过强制结束的新进程冷启动，也未扩大为 SDK 初始化通过。

`startup-analysis.json` 固定目标模块身份并记录实际 `nn::init::Start` 调用顺序。SDK 的 `_init_libc1` 是跳转，并非 RET；必须继续分析其目标，不能因 `_init_libc0/2` 为空而跳过整个 libc 初始化。游戏的 `nninitStartup` 会查询内存、设置堆、分配 32 MiB 内存块并注册线程本地分配器；SDK 初始化涉及虚拟地址空间、oe/aoc/fs/pctl 等服务。下一次执行前先核实目标线程对象/TLR/TLS 布局和严格失败策略，不使用 D2R 的固定线程结构或返回成功的服务空桩推进。
