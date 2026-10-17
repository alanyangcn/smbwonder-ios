# SDK 初始化状态

后续更新：无调试器 SDK 冷启动、独立 1 MiB 主线程、模块 DT_INIT 与 nnMain 入口已在 FlyMe 验证；最新范围与阻塞见 BOOT-STATUS.md。下文为 SDK 阶段历史记录。

验收范围：Super Mario Bros. Wonder 1.2.0（content version 262144），FlyMe / iPhone 8 Plus / iOS 16.7.16。只验收 SDK 初始化链，不执行模块全局构造函数或 nnMain。

## 本轮实现

- 使用目标 SDK 实际的 TLR 布局：0x1f0 为 TLS 指针，0x1f8 为由 SDK 自行构建的线程对象；不制造 D2R ThreadType。
- 为此诊断提供独立 TLS backing、计时器门、主线程标识和启动所需的内存/查询 SVC。资源预算明确属于宿主诊断配置；地址预留依次尝试 1 GiB / 512 MiB / 256 MiB，不等同于 Switch 的内存配置。
- 对目标构建开启 SMBW_STRICT_IMPORTS：未解析弱符号置零，调用未实现强符号即中止；此编译选项不改变 D2R 默认构建。
- 通过 8 个明确的 HLE_FORCE 服务边界接管初始化和 CPU boost 请求，保留 SDK 的 os、libc、虚拟地址管理和游戏堆初始化本体原生执行。
- 提供持久日志、线程错误上下文、独立 save/temp 目录、宿主进程环境、空 DLC 挂载目录状态、默认禁止在线操作的宿主策略和保持原终止行为的中止观察器。CPU boost 请求映射到宿主线程 QoS，不承诺改变硬件频率。

这些服务上下文用于替代 Horizon 会话初始化；**后续文件 API、OE 通知、Nintendo 家长控制等完整服务接口仍未实现**。不得把初始化函数返回等同于这些服务的全部语义已适配；未接管的后续请求可能遇到未知 IPC/SVC，也可能先访问空会话指针；具体边界必须按实际故障定位，不能认为全部已被拦截。RomFS 尚未装入此诊断 App。

## 实际检查

宿主测试覆盖真实 C 实现的堆分配/回收、清零、查询、非法大小、物理映射、弱符号、未实现强符号中止、服务日志落盘、目录创建和信号终止保留。与既有 CPU/BNSH/输入校验测试合计 20 项 unittest 通过。测试日志中的一次 SDK_UNIMPLEMENTED_IMPORT FATAL 和信号中止是预期负对照。

真机阶段标记按顺序核验：

1. GUEST_RET_OK（_init_libc0）
2. SDK_TIMER_OK
3. SDK_STAGE_OK nnosInitialize
4. SDK_THREAD_OK
5. SDK_STAGE_OK _init_libc1
6. SDK_STAGE_OK nninitInitializeSdkModule（服务初始化使用上述宿主后端）
7. SDK_STAGE_OK nninitStartup（目标 main 中真实的游戏堆/TLS 分配器初始化）
8. SDK_STAGE_OK _init_libc2
9. SDK_INIT_COMPLETE

最终复验的日志路径、代码哈希与结果见 evidence/sdk-device-result.json。早期原始失败日志保留在外部工作区：第一次为未实现的 ConnectToNamedPort（SVC 0x1f）；另一次直接打开旧版时出现 1 GiB 地址预留 ENOMEM，已加入资源回退，具体回退档位是否在真机命中以最终日志为准。

## 尚未验收

- 独立于调试器的 SDK 新进程冷启动。
- 任意游戏 ELF TLS 的完整静态/动态布局；当前 8 KiB backing 只覆盖本诊断实测初始化路径。
- 主线程后续按 NPDM 的 1 MiB 栈设置与工作线程生命周期；当前诊断在宿主工作线程上运行。
- 原 rtld 的完整生命周期回调；宿主替代 rtld，模块构造函数尚未执行。
- 全部 Horizon 服务语义、游戏入口、图形、音频、输入和存档。

下一阶段：专用游戏主线程和栈 → 模块 DT_INIT / init_array → 严格模式下进入 nnMain 并记录首个缺口。下一阶段不能沿用此文档作为“已可玩”或“完整原生引导已通”的证明。
