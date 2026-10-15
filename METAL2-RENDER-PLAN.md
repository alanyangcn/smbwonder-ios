# Metal 2 渲染层设计（B 阶段）

nvn-round14 起游戏已进入引擎图形初始化（读完 Bootup pack 与 RSDB，停在 `nvnDeviceRegisterFastClearColor`）。
接下来是命令录制、提交、资源与着色器。A11 只有 Metal 2：无 GPU 虚拟地址、无资源 ID、显式绑定槽。

## 当前 B1 实测（nvn-review-round24）

目标 SDK 55 的 command buffer 是 0xa0 字节；游戏会直接访问部分字段，不能覆盖成 D2R 的自定义对象。初次控制内存/命令内存绑定已真机通过，停在 BeginRecording。录制格式必须兼容这些内联路径；如果采用宿主命令格式，需要先明确原生控制记录/命令写入的桥接，不能直接假定所有命令都经过 NVN 取址表。

## 1. 命令缓冲
- `AddCommandMemory/AddControlMemory` 用游戏给的内存；命令以紧凑宿主格式录进游戏的命令内存（同 D2R 思路），不够时按 NVN 语义调用游戏注册的内存回调（OUT_OF_COMMAND/CONTROL_MEMORY）。
- `EndRecording` 返回句柄 = 本段命令流位置；`QueueSubmitCommands` 回放到持久 MTLCommandQueue 的命令缓冲。
- 录制阶段只做参数校验；回放遇到尚未实现的命令 → 按名停止（不跳过）。

## 2. 同步与显示
- `QueueFenceSync/SyncWait/QueueWaitSync`：MTLSharedEvent（iOS 12+，A11 可用）的单调值。
- `QueuePresentTexture`：把显示贴图 blit 到 CAMetalLayer 的 drawable 并 present；归还 AcquireTexture 的索引；节奏由 VSync 事件驱动。
- 目标 B1：游戏自己的清屏首次出现在屏幕上。

## 3. 内存与缓冲
- GPU 地址 = CPU 地址；绑定时按地址在内存池区间表中查找 (MTLBuffer, offset)（已有每池零拷贝 MTLBuffer）。

## 4. 贴图
- 每个 NVN 贴图对应宿主 MTLTexture；存储在游戏内存池里按 TX1 块线性排布（游戏的 BNTX 数据即如此）。
- 首次使用或检测到写入（CopyBufferToTexture / WriteTexels / 初始化前的 CPU 写入）时用 D2R 的解排布代码转线性后上传。
- BC1–7：A11 不支持 → CPU 解码为 RGBA8（之后可考虑离线转 ASTC）；ASTC 原生支持。

## 5. 着色器
- iOS 不能运行翻译器（Ryujinx C#）：离线在 Mac 上把 RomFS 内全部 BNSH/BFSHA 变体翻译为 MSL（已有链路：prepare → translate → compile），按代码哈希索引随 App 携带；设备上 `newLibraryWithSource` 编译并缓存。
- `ProgramSetShaders`：按哈希取 MSL；缺失即按哈希停止。
- 反射（常量缓冲、贴图/采样器槽、顶点输入）逐项核实后才置 runtime_eligible。
- 深度：游戏用 NEAR_IS_MINUS_W（GL 裁剪），顶点着色器输出 z' = (z + w)/2；窗口原点 UPPER_LEFT 与 Metal 一致。

## 6. 渲染状态
- Blend/ColorState/ChannelMask/DepthStencil/Multisample/Polygon/Vertex* 为按值拷贝的小结构，按 NVN 保留大小就地打包。
- MTLRenderPipelineState 以 (程序, 附件格式, 混合, 顶点布局, 采样数) 为键缓存；DepthStencilState 单独缓存。

## 实施顺序
B1 命令缓冲内存/录制骨架 + 提交 + 同步 + 显示（清屏可见）→ B2 贴图与描述符注册 → B3 着色器离线库与 PSO（首个 draw）→ B4 绘制与状态全集。
每轮仍以真机日志停点推进；未实现即停止，不以空操作冒充。
