# Metal 2 当前进度

目标：Wonder 1.2.0 / update 262144；FlyMe（iPhone 8 Plus / A11 / iOS 16.7.16）。继续以 Metal 2 / MSL 2.1 推进，目前没有已证实必须换新设备的停点。游戏尚未出首帧或达到可玩状态。

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
