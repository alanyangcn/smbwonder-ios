# Metal 2 当前进度

目标：Wonder 1.2.0 / update 262144；FlyMe（iPhone 8 Plus / A11 / iOS 16.7.16）。继续以 Metal 2 / MSL 2.1 推进，目前没有已证实必须换新设备的停点。游戏尚未出首帧或达到可玩状态。

## 首帧推进（2026-10-15，持续进行）

项目已独立为 `~/funny/smbwonder-ios`，Git 分支 `first-game-frame`。执行计划在 `docs/superpowers/plans/2026-10-15-first-game-frame.md`。

- round25–26：SDK55 Begin/End、DrawElements 模板录制、原生未压缩控制句柄通过真机。尚未执行绘制。
- round27–28：命令内存统计、回调查询、对象与句柄释放通过；修复内存池 Builder 先 SetDevice 后 SetDefaults 的调用顺序，新增 GPU 地址到真实物理 span 的解析。
- round29：run `0EB5CF03-0579-443C-8942-D17C3A3297DA`（debugger=1），154 入口，shader-code 池地址/大小和空 Program 初始化已接入；实际停在 `MemoryPoolSetDebugLabel`。45 项测试、Xcode 构建、签名、安装通过。
- round30–32：特效系统调试标签已通过；165 入口，推进至系统共享字体服务。round32 run `3546842C-B95E-4A87-AD4D-FDFC23621EFF`。
- 字体路径使用 CoreText 系统字体替代，重建真实 SFNT 并验证校验和与可解析性；六个本机字体及稳定地址测试通过。Nintendo 扩展符号的覆盖并不等价。round33 被未被游戏引用的强制 RequestSharedFontLoad hook 拦住，已移除。round34 run `2F7358F0-4F70-4EF0-9125-AA653D31EC3E`（debugger=1）通过系统字体替代（含 type=5 AppleSymbols），停在 `nvnTextureBuilderSetPackagedTextureLayout`（main+0x5293c）。
- 补了合法 NULL 命令内存回调与 EndRecording 授予范围校验；ProgramSetShaders 严格停点前会捕获控制数据/代码前缀。缺失绘制语义仍明确停止。完整宿主测试目前50项。
- round35（待真机）：`nvnTextureBuilderSetPackagedTextureLayout` 按 nn::gfx TextureInfo+0x24 的 8 字节布局实现。全 RomFS 3063 个 BRTI 统计：u32 块高 log2（0..4）+ u32 0x00010007；第 0 字节决定第 0 级 GOB 块高（与自动算法可不同），其他值严格停止。`nvnDeviceGetTextureHandle` 按 TX1 句柄 texture | sampler<<20 实现（main+0x5de9c 直接传给 BindTexture）。167 入口。
- round35 run `2CF81B76-04DB-4B54-9CDB-2CABD4B42564`（debugger=1）：packaged 贴图布局通过，停在 `nvnProgramSetShadersExt`（main+0x10857a0）。
- 引用扫描修正：原 nvn_inventory 只统计 `adrp+ldr` 直接取槽，漏掉 `adrp+add` 与尾调用（br）。完整扫描（排除 0x1080ff0..0x1084a70 的加载器）得出游戏实际引用 241 个 NVN 名字，其中当时缺 100 个。
- SDK 真值：sdk 模块自带 NVN 名表（名字串 sdk+0xb3de9f，偏移表 +0xb41808，入口 +0xd19a08 → 派发槽 → 实现表 +0xd36fd8），可逐函数核对语义。
- round36（待真机）一次补齐 99 个：
  - 对象类：ProgramSetShaders/Ext（控制段魔数 0x98761234、阶段字段 +0x714，来自 16600 份 RomFS 控制段统计）、各 Finalize、Sampler/TextureView 取值、Buffer 刷新、Device 调试回调（release 驱动为空操作）、ImageHandle、时间戳；GetTextureHandle 按 sdk+0x44e150 修正为符号扩展且置 bit 32。
  - 录制类：67 个 CommandBuffer 调用录为带类型、参数快照的命令；按 sdk+0x443584 实现命令内存回调与分段（兼容游戏内联的 BeginRecording），EndRecording 按段写 SDK55 控制组。
  - 提交类：QueueSubmitCommands 回放到 Metal（清屏、缓冲填充、uniform 更新、时间戳/数值报告、状态跟踪），FenceSync/SyncWait/Flush/Finish，PresentTexture 按宽高比把显示贴图画到 CAMetalLayer（sRGB 经 UNORM 视图原样输出），Window 裁剪/间隔/活动数。
  - 回放遇到 draw、dispatch、贴图复制/降采样/清除、非时间戳计数器、条件渲染、部分掩码或带裁剪的清屏时按名停止，不跳过、不伪造。
  - 唯一保留的停止跳板是 `nvnTextureBuilderGetZCullStorageSize`：它依赖 TX1 硬件 ZCull 参数，唯一调用点在 main+0x1e1f918 的调试工具路径，游戏不使用 ZCull 保存/恢复。
  - 265 个入口；NVN 宿主测试（Linux gcc -Werror）通过，新增录制、分段、回放、栅栏、显示测试。
- round36 run `059CAB3C-DBDF-4D16-9DEB-841822C8B6FA`（debugger=1，51 项测试、xcodebuild、签名、安装通过）：265 个 NVN 入口全部生效，Metal 回放代码在设备上编译运行；游戏完成图形初始化，3570 次 ProgramSetShadersExt（首个程序 2 阶段已完整捕获到 Documents/ShaderCapture），VFX 内存池、两个工作线程、声音配置读取均通过。尚未发生 QueueSubmit/Present。
- 新停点不在 NVN：`nn::audio::GetAudioRendererWorkBufferSize`（sdk+0x1e0a54）经 `CreateAudioRendererManagerByHipc` 走真实 `sm:` 端口（SVC 0x1F ConnectToNamedPort）。Wonder 导入 197 个 nn::audio 函数，使用完整 AudioRenderer（不是 D2R 的 AudioOut），需要新的音频子系统。
- 音频方案（用户选择：先静音实时消耗）：sdk 静态链接了完整 AudioRenderer 栈：客户端、进程内服务端（AudioRendererManagerImpl/AudioRenderSystem/InfoUpdater/CommandGenerator）、DSP 命令处理器（CommandListProcessor）。本构建只有平台层是空实现。round37（待真机）：`CreateAudioRendererManagerByHipc` 转到 sdk 自己的 `ByDfc`（sdk+0x1e0a38 在 executionMode==1 时本就走它）；宿主 DSP：`dsp::SendCommandBuffer` 按 sdk+0x20ccd4 的方式运行原生 CommandListProcessor，`dsp::Wait` 按 5 ms DSP 帧绝对时间节拍；音频设备按 iPhone 扬声器（内置扬声器、立体声、主音量 1.0）作答，设备事件为真实内核事件。最终混音暂留内存，下一步接 iOS 出声。
- round37 加载器拒绝未被引用的强制接管（GetAudioSystemMasterVolumeSetting 为 sdk 内部直接调用，已移除）。round37b run `E380293E-AE28-4558-B272-396870E242A4`：进程内音频服务端生效（GetAudioRendererWorkBufferSize 通过），OpenAudioRenderer 停在 SVC 0x15 CreateTransferMemory。
- round38（待真机）：内核实现传输内存 0x15/0x51/0x52：仅支持所有者权限 None，映射时按 16 KiB 宿主页搬移内容，解除时搬回；按 Horizon 规则拒绝堆已用部分和 alias 区（返回 0xd401 让 SDK 换随机地址），另外接受堆区未用尾部，SetHeapSize 不允许扩到传输映射上。句柄表新增通用宿主对象类型。
- 仍需：实际着色器对应及完整身份验证、Metal 资源绑定与 PSO、命令提交/同步/展示和真正的游戏首帧。六份旧样本仍不具备运行资格。

以下审查和轮次保留为历史基线，不是当前停点。

## 本轮接续审查与修复

详见 [审查记录](REVIEW-2026-10-15.md)。保留其他会话的工作，修复了目标 ABI、边界校验、并发映射和线程退出问题：

- CPU_NO_ACCESS 物理池的刷新误拒绝：按目标 SDK 的 CPU_CACHED 分支修正。
- TextureView 从错误的 40 字节改为目标 SDK 的 16 字节真实布局，避免覆盖相邻 copyRegion。
- WriteTexels 坐标回绕、MapVirtual 范围回绕均补负例并修复。
- 虚拟池增加物理存储 span 解析，验证同物理页别名、解除中间页及残留偏移；Metal 贴图缓存的别名同步仍未实现。
- 内核映射元数据与 VM 操作串行化；子线程恢复宿主栈后才发布退出；关闭未启动线程会回收宿主线程。
- TLS/TLR 用独立所有权键保留原始分配，客体改写 TLS 指针后仍可正确释放。
- 采样器允许先 SetDevice 后 SetDefaults，并补齐环绕、LOD、比较、边框色、各向异性和 reduction 参数保存。137 个名字已有宿主入口，剩余 397 个为按名停止跳板；数量不代表功能完成率。
- 命令缓冲对象改为目标 SDK 的 0xa0 字节 ABI；设备/回调位于 +0x60/+0x68/+0x70，避免与游戏直接写入的 +0x10 标志冲突。支持首次控制内存和命令内存绑定；录制中补段/链式连接仍明确停止。
- 真机脚本删除安装失败后自动卸载 App 的行为；默认启动仍附加 LLDB。

## 验证证据

宿主测试 45 项通过，包括真实 GPU 诊断、虚拟映射别名/拆分、并发整页重置、实际切换栈后的线程退出、关闭未启动线程和 TLS 所有权测试。关闭恢复宿主栈的负对照会触发断言。

- `nvn-round17`：原 CPU_NO_ACCESS 刷新停点。
- `nvn-review-round18`：run B2ACBD93-AA4B-4466-9B9D-8A3F61B600B8，发现 TextureView 覆盖参数。
- `nvn-review-round19`：run F277336D-D837-4839-9592-E338267390B1，通过贴图写入，停在 SamplerBuilderSetDevice 的错误生命周期检查。
- `nvn-review-round20b`：run 9CBAC4A1-DEF6-493C-8D53-C91F2AC13044，停在未实现的 SamplerBuilderSetWrapMode。
- `nvn-review-round21b`：增量重放构建/签名/安装成功，但下载到的日志仍报告 128 个入口，已标为无效新版验收。
- `nvn-review-round22`：run AE288825-7B5C-45C3-BCA9-3E16707AE478，135 个入口，采样器设置通过，停在 AddControlMemory。
- `nvn-review-round23`：run 43B805CA-CFFE-4DEA-A2A4-794982D63490，136 个入口；512 字节控制内存绑定通过，停在 AddCommandMemory（请求 4 MiB）。
- `nvn-review-round24`：run E17788BD-F2E3-43EA-8DCD-487D0EE015EF，debugger=1，两遍137入口确认；512字节控制内存、4 MiB命令内存绑定通过，停在 `nvnCommandBufferBeginRecording`（main+0x5c388）。45项测试、正常xcodebuild、签名校验和安装均通过。
- 最新证据：`/Users/yanglipeng/funny/smbwonder-ios/work/analysis/nvn-review-round24/verified-result.json` 绑定本次源码、签名App及日志哈希。

round23 启动时 debugger=1；round22 启动时检测为 debugger=0，但同样由 LLDB 启动流程发起。均不作为独立冷启动验收。

连接恢复前 Xcode 在 Mac 的 IOPM 电源断言调用中阻塞（采样证据在私有工作区 `/tmp/wonder-xcode-sample.txt`，pmset 查询也阻塞）。使用 round20b 成功日志里的 clang/链接参数重编译 27 个 runtime C 文件，复用其余已构建对象，重新签名并严格校验后安装。`replay-build.py/.json/.log` 保存命令、源码/头文件哈希、所用对象哈希和签名后可执行文件哈希。这份 round21b 产物是增量重放构建。连接恢复后的 round22/23/24 已重新完成正常 xcodebuild、签名和安装。

## 仍未完成

1. 从 BeginRecording 补录制、命令格式、段链接/内存回调、提交、同步和游戏清屏。首次内存绑定已验证，录制中补段仍停止；须兼容游戏可能内联访问的 SDK 55 字段和控制记录。
2. Metal 资源消费与缓存一致性：pool_span 跨映射读取、物理别名、CPU 更新上传、GPU 写回和资源生命周期。
3. 实际启动用着色器的捕获、离线翻译、反射绑定与 PSO。先前六个样本编译成功仍是诊断证据，runtime_eligible=0。
4. 真实游戏首帧、多个场景、输入、音频、存档与独立冷启动验收。
5. DeviceGetInteger 的 D2R/Tegra 配置尚未逐项验证，StorageClass 仍是宿主类别；不能当作完整 NVN 55 或宿主硬件能力声明。

先前 Metal 2 GPU 诊断及六份样本着色器编译证据保存在 [早期基线](METAL2-BASELINE-2026-10-15.md)，其中 GAME_RENDERER_PENDING 等停点仅代表当时状态。下一阶段设计见 [渲染计划](METAL2-RENDER-PLAN.md)。
