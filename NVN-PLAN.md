# NVN / VI 实际启动接通

继续用户已授权的 Metal 2 移植：共用 CPU/SDK/RomFS，先补实际调用，不以诊断画面冒充游戏首帧。

当前首帧推进：round32 已走过录制 Begin/End、DrawElements 模板、shader-code 池创建/取地址及特效系统标签，停在系统共享字体服务；CoreText 字体替代在 round34 验证中。已接入165入口；仍没有 GPU 游戏绘制或首帧。更新见 METAL2-STATUS.md；私有逐轮证据在 work/analysis/first-frame-round*/，执行日志在 work/analysis/first-frame/ledger.md。

- [x] （真机 nvn-round1：svc 0x45 → 0x2001/0x2002，VSync SystemEvent 建立）实现内核事件：写/读端、关闭、signal/clear/reset、真实等待与超时、并发信号及句柄生命周期测试。
- [x] （真机 nvn-round1：HOST_GRAPHICS_DRIVER_READY，InitializeGraphics 返回）建立持久 Metal2 设备/队列和 UIKit 的 CAMetalLayer；驱动初始化只在真实宿主上下文建立后返回，不代表 NVN device 或游戏渲染已经完成。
- [x] （真机 nvn-round1：VI 初始化/打开/建层/VSync 全部 result=0；CADisplayLink 实际触发尚待后续轮次观察）用有状态宿主显示/图层替换 VI，VSync 使用 CADisplayLink 驱动实际事件；使用原生 SDK 构造 SystemEvent，不猜结构偏移。
- [x] （真机 nvn-round1：两遍取址 5 实现/529 停止跳板，DEVICE_READY 55.15 过版本门槛；0x02–0x5F 设备属性取 D2R SMO 实测值，未在 API 55 上核对）严格 NVN bootstrap：按实际加载名登记类型正确的入口，未知加载/调用报告首个缺口；不能使用返回0的通用成功桩。
- [~] 多线程内核（kernel_threads.c，宿主测试含 TSan 通过；待真机）
- [ ] FlyMe 真机逐轮验证，保留新 run 日志；记录首个尚未实现的 NVN 行为，继续对应实现。

验证：宿主事件语义和失败对照、Metal2既有测试、签名构建、真机调用链。未测的渲染/着色器资源契约保持未验证状态。

真机一轮：`tools/device_round.sh <轮名>`（宿主测试→构建→签名→安装→启动（默认附加 LLDB）→取日志，结果在 smbwonder-work/analysis/<轮名>/）。

## 接续审查（2026-10-15）

代码审查及修复见 [REVIEW-2026-10-15.md](REVIEW-2026-10-15.md)；最新验收状态见 [METAL2-STATUS.md](METAL2-STATUS.md)。修复包括 16 字节 TextureView ABI、刷新检查、区域溢出、物理映射解析、内核并发映射、线程退出/关闭和 TLS 所有权。采样器新增 7 个状态设置入口，加上两项命令内存绑定，当前 137 个宿主入口 / 397 个停止跳板；入口数不是实现完成率。

round21b 的旧日志（run 531713E2-2369-440A-81F4-3900317FC801）仍报告 128 个入口，不能作为新版真机证据。连接恢复后使用 round22 重新构建、签名、安装及启动。

## 逐轮记录

| 轮 | 结果 | 首个缺口 |
|---|---|---|
| nvn-round1 | bootstrap、版本门槛、VI、VSync 事件通过 | `nvnDeviceSetWindowOriginMode`（main+0x984e98 ← 0x5bba00） |
| nvn-round2 | 原点 UPPER_LEFT、深度 NEAR_IS_MINUS_W（GL 式，宿主需换算）；0x733xxx 查询 23 项设备属性 | `nvnMemoryPoolBuilderSetDefaults`（main+0x733904 ← 0x985824 ← 0x5bba14）；Queue 尚未到达 |
| nvn-round3 | 无效：脚本取回了上一轮遗留在设备上的日志（同一 run id），已改为必须看到新 run id | — |
| nvn-round3b | 描述符池（采样器 4096、贴图 33068）、采样器 #256 注册、Queue 建立（内存 0x100000）；整个游戏线程在 app 变为 active 之前已跑完，1.6 s 后进入后台（设备疑似锁屏） | `nvnTextureBuilderSetDevice`（main+0x984a58 ← 0x5bbc64） |
| nvn-round4 | 交换链：3×1920×1080 RGBA8 显示贴图（TX1 0x870000/张）、Window 绑 CAMetalLayer、Acquire→0；app 全程前台 | `nvnCommandBufferInitialize`（main+0x5bbcfc） |
| nvn-round5 | 无效：--justlaunch 丢失启动（rc=1「Application has not been launched」），无新 run id | — |
| nvn-round5b | 命令缓冲对象、3 个池、32 字节 Buffer 映射全部通过（55 个已实现） | 非 NVN：`svc 0x4 MapMemory`（sdk 建「Presentation Thread」搬栈，main+0x5bc0ac） |
| nvn-round6 | 多线程内核真机通过：MapMemory 搬栈、CreateThread→0x1004、StartThread，Presentation Thread 运行并进入 WaitForAddress；app 保持前台（LLDB 挂后台） | 非 NVN：`svc 0x1f ConnectToNamedPort("sm:")`，主线程 nn::account::Initialize（main+0x5b86b0） |
| nvn-round7 | 账户通过：uid 生成并持久化、昵称 Player | 非 NVN：`sm:`，nn::nfp::Initialize（amiibo，main+0x5ba880） |
| nvn-round8 | amiibo 通过（0 台设备，可用性事件建立）；游戏起了 6 个作业线程（核掩码 0x4） | 非 NVN：`sm:`，nn::hid::InitializeNpad（main+0x63e4d0） |
| nvn-round9 | hid 通过（支持 9 个 NpadId、样式 0x1f，掌机已连接）；随后主线程崩溃：nn::os::AllocateAddressRegion → VammManager 空指针（sdk+0x134894）。原因：GetInfo(16) 返回 0，SDK 未启用 VAMM；NPDM 实际声明 system_resource_size=16 MiB | 主机侧缺陷（非游戏缺口） |
| nvn-round10 | VAMM 启用；SDK 以 4 KiB 调 MapPhysicalMemory，旧内核按 16 KiB 页校验返回 0xd401，SDK Break | 主机侧缺陷 |
| nvn-round11 | 4 KiB 别名映射通过，SDK 初始化完成；但 nninitStartup（main+0x6122b0）在 VAMM 启用时为虚拟地址堆预留 0x6d600000（1750 MiB），诊断池仅 512 MiB → SetHeapSize(-0x52200000) 失败，随后 nnMain 查找失败 | 主机侧内存模型 |
| nvn-round12 | iOS 拒绝 8/6/5/4 GiB 保留（errno 12；无扩展虚拟地址 entitlement） | 主机地址空间上限 |
| nvn-round13 | 实测最大可保留 1280 MiB（免费个人团队，无扩展虚拟地址），不足 2816 MiB，明确停止 | 主机地址空间上限 |
| nvn-round14 | 付费团队 5H8VE3NN7D、包名 two.local.smbwonder.loaderprobe；两项 entitlement 生效，可保留 8 GiB，堆 1444 MiB；nnMain 走过账户/amiibo/hid/VAMM，读 Bootup pack（17 MB）与 RSDB，进入图形引擎初始化 | `nvnDeviceRegisterFastClearColor`（main+0x423684） |
| nvn-round15 | 快速清除色登记通过 | `nvnTextureBuilderSetWidth`（main+0x7778c，图形封装库贴图助手）|
| nvn-round16b | 贴图/状态对象通过（脚本 RomFS 标记修正后补传一次）| `nvnTextureBuilderGetStorageClass`（main+0x42efc0，按格式建存储类别表，供稀疏池）|
| nvn-review-round24 | run E17788BD-F2E3-43EA-8DCD-487D0EE015EF；137个入口，512字节控制内存与4 MiB命令内存初始绑定通过，45项测试/正常构建/签名/安装通过 | BeginRecording（main+0x5c388） |
| nvn-review-round22 | 新运行 AE288825-7B5C-45C3-BCA9-3E16707AE478，两遍135入口确认；采样器设置通过 | AddControlMemory |
| nvn-review-round23 | 新运行43B805CA-CFFE-4DEA-A2A4-794982D63490，修正命令缓冲SDK55 ABI，512字节控制内存绑定成功 | AddCommandMemory，请求4 MiB |
| nvn-round17 | 真机已取证；共 128 个入口，存储类别、虚拟池和贴图写入代码已接入，但尚未全部实际执行 | CPU_NO_ACCESS 物理池上的 MemoryPoolFlushMappedRange 被宿主误拒绝 |
| （历史）nvn-round16 设计 | 贴图拆为 nvn_texture.c（Builder 全套、通用贴图、Get*、视图、贴图池注册按序号快照）+ nvn_state.c（8 类状态对象 33 个）；复用 dm/gfx/tex 的格式映射与 BC 解码；共 123 个 | 预计命令缓冲 AddCommandMemory/BeginRecording（B1） |
| （历史）nvn-round15 设计 | 快速清除色登记（4 个）、Buffer 地址、内存池 Map/GetFlags/Flush/Invalidate（共 64 个）；B 阶段设计见 METAL2-RENDER-PLAN.md | 预计命令缓冲 AddCommandMemory/BeginRecording |
| （历史）nvn-round14 设计 | 加 extended-virtual-addressing + increased-memory-limit entitlement 试签名（用户选择） | 签名是否接受 |
| （历史）nvn-round13 设计 | 二分实测最大可保留空间，留 512 MiB 余量；别名区 1792 MiB（容纳游戏 1750 MiB 虚拟堆）、栈区 256 MiB、其余为堆；不足 2.5 GiB 则明确报需 entitlement | — |
| （历史）nvn-round12 设计 | 地址空间按主机布局重排（尝试 8/6/5/4 GiB 保留；别名区 2 GiB、栈区 512 MiB、堆区 3 GiB 起），应用池报主机值 3285 MiB（放不下时按堆区收缩），GetInfo 21 扣除系统资源、7/22 计入 VAMM 已映射页；全部惰性提交 | 预计回到 AllocateAddressRegion 之后 |
| （历史）nvn-round11 设计 | 别名区按 4 KiB 位图记账（16 KiB 宿主页引用计数、新映射页清零、QueryMemory 按位图出区段）；SDK Break 改为带调用链的干净停止；以及上一轮的 GetInfo(16)=16 MiB（NPDM META+0x14，构建时校验）；客体故障报告 crash_report.c（SIGSEGV/BUS/ILL/TRAP → 模块偏移 + 调用链，冻结线程，不再闪退） | 预计 VAMM 经 MapPhysicalMemory 分配地址区后继续 |
| （历史）nvn-round9 设计 | nn::hid 服务侧 26 个入口——iPhone 作为掌机手柄 0x20（游戏支持掌机样式时连接；输入由宿主发布，目前中性）、5 种 GetNpadStates（16 条历史、5 ms 采样号）、鼠标/键盘未连接、震动不许可（值丢弃）、六轴有句柄但未连接 | 下一个缺口 |
| （历史）nvn-round8 设计 | nn::nfp 无 NFC 读卡器——Initialize、可用性事件（SDK SystemEvent，永不触发）、ListDevices→0 台（2115-0064） | 下一个服务缺口 |
| （历史）nvn-round7 设计 | nn::account 单一离线本地用户（uid 首次随机生成并持久化于 WonderSDK/account；昵称 nickname.txt 或 Player）；网络服务账号仅「不可用」查询，其余联网接口不接管 | 下一个服务缺口 |
| （历史）nvn-round6 设计 | 多线程内核 kernel_threads.c（移植 D2R hle_thread.c：线程/事件/锁/条件变量/地址仲裁，取代 kernel_events.c）；MapMemory/UnmapMemory 搬移语义（iOS 16 KiB 页无法做 4 KiB 别名）；进程核掩码 1→7；首个停点后其余客体线程在下一次 SVC 处冻结；默认挂 LLDB 后台启动（justlaunch 断开会把 app 送回主屏） | 预计 Presentation Thread 内的首个 NVN/服务缺口 |

交换链（main+0x9849bc）静态调用：TextureBuilder（SetDevice/Defaults/Size2D/Target/Format/Flags/GetStorageSize/Alignment/SetStorage）、MemoryPool、TextureInitialize、SyncInitialize、WindowBuilder（Defaults/Device/NativeWindow/Textures/NumActive/PresentInterval）、WindowInitialize、WindowAcquireTexture。需先定贴图存储大小/对齐契约（TX1 块线性 vs 宿主线性），再实现。
