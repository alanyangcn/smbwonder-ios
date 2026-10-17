# 当前进度（Metal 3）

目标：Wonder 1.2.0 / update 262144；iPhone 18 Pro（iPhone19,2 / iOS 27）。2026-10-16 起换机：渲染只走 Metal 3 / MSL 3.2，不再为 A11 / Metal 2 做让步（见下方“换机”一节）。round108 之前的记录是在 FlyMe（iPhone 8 Plus / A11 / iOS 16.7.16）上用 Metal 2 跑出来的，作为历史保留。游戏在 FlyMe 上已稳定显示启动加载画面和标题主视觉图（round108），尚未进入可玩状态（3D 场景、输入、音频输出等仍在推进）。

## 换机：iPhone 18 Pro / Metal 3（2026-10-16）

- 去掉的让步：MSL 2.1 → 3.2（运行时编译、GPU 探针、spirv-cross `--msl-version 30200`；57 个阶段重建，源码与 2.1 版逐字节相同），最低 iOS 18.0；BC1–7 原生采样（删去 CPU 解码）；CLAMP_TO_BORDER 的不透明黑/白直接用 ClampToBorderColor（删去运行时边框探测）；swizzle 视图视为必备。图形边界只剩 Metal 3 一条路径，GPU 缺 Metal 3 家族或 BC 采样即按名停止（`GRAPHICS_DEVICE_UNSUPPORTED`）。源码 `metal2_*` 改名 `metal3_*`。
- 共享字体：iOS/macOS 27 的 PingFang SC/TC 是 `hvgl` 可变字形格式（无 glyf/CFF 轮廓），游戏字体引擎无法使用；改为按候选列表取第一个带 glyf/CFF 轮廓、PostScript 名匹配的系统字体（简体/繁体依次试 PingFang → 华文黑体 STHeiti → 冬青黑体 GB）。功能差异：中文字形来自替代字体。
- 工具：新 Mac（Apple M6，无 Rosetta，Xcode 27.1）。ios-deploy 弃用，`device_round.sh` 改用 `xcrun devicectl`（安装、App 容器传文件、`launch --console` 记录 App 输出到 `console-*.log`、系统崩溃日志）；`setup_env.sh` 按本机架构重装工具（arm64 工具链重建的着色器与原产物逐字节一致）。
- round109（2026-10-16，首轮新机）：测试、构建、签名通过，**安装被拒**：付费 Team `5H8VE3NN7D` 新生成的描述文件（13:18）里没有这台 iPhone，而开发者服务器又回复该设备“已在本 Team 存在”——设备在该 Team 里多半处于停用状态（个人 Team MFXJ9AKFM5 的描述文件里有它）。需要在开发者网站把设备启用后重跑。
- round110–133（个人 Team `MFXJ9AKFM5`、Bundle `local.smbwonder.loaderprobe`、无 extended-virtual-addressing：12 GB 机器上未发现地址空间不足）：
  - iOS 27 强制 UIScene 生命周期（round111 SIGTRAP）→ `ProbeSceneDelegate`；只横屏、全屏 16:9、隐藏状态栏与 Home 条。
  - 输入：悬浮半透明触控按键 + GameController 手柄（按位置映射），写入手持手柄 npad 0x20；round115 起游戏读到 A 并离开标题。`WONDER_AUTO_INPUT`（如 `A/2.5@25-300`）脚本按键，round132 起无人操作走到关卡。
  - 日志：stderr 同时写文件和非阻塞控制台（`devicectl --console` 实时看）；NVN 逐调用日志限流；`WONDER_INTERACTIVE=1` 交互模式；`tools/census_round.sh` 一步完成“取普查→重建→启动”。`WONDER_CENSUS_SWEEP=1`（诊断）缺着色器的 draw 只写普查不执行，一次运行收集沿途全部程序。着色器 102 个阶段（`a0dd652304bcc80a` 因普查时纹理槽 3 未绑定暂缺）。
  - 修复：存档首次 Commit 在客体小栈上 64 KiB 栈缓冲溢出（round121）；`nn::account::ShowUserCreator` 离线无新建用户界面 → ResultCancelledByUser 0x27c（round122，选人界面的“+”会回到原界面）；`nvnBufferBuilderSetStorage` 按 sdk+0x442d00 只记录、由 Initialize 校验（round123）；`AddCommandMemory` 允许普通池的授予越过池尾（sdk+0x442f70 不检查，round126）。
  - 关键根因（round125，LLDB 真机附加 + `tools/sdk_nvn_emu.py` 用 unicorn 跑 sdk 自带 NVN 驱动作真值）：gsys::ModelResource 用预录命令的 `GetCommandMemoryUsed/ControlMemoryUsed` 给每个形状留 GPU 空间，宿主内联记录（16 字节头 + 每参数 8 字节）让 `Model/PlayerYoshi.bfres` 在按 RSTB 精确定大小（0x66e620）的堆里差几 KB，48 KiB 块分配静默失败，随后以空池建缓冲。改为命令记录放在宿主（`command_store.c`），客体命令内存每条只占 8 字节引用（不大于任何 NVN 命令）。纹理大小/对齐与 sdk 驱动对 304 种描述逐一相同；`nvnDeviceGetInteger` 游戏用到的值与 sdk 跳转表一致。
  - 文字：字体是矢量字体（bfttf/bfotf）运行时光栅化到 2048×8192 R8 线性字形缓存（swizzle 1,1,1,R），文字着色器以 2D 数组指令采样 2D 纹理（Maxwell 读第 0 层）→ 反射记录着色器声明类型、绑定 1 层 2D 数组视图。线性单层纹理改为内存池 MTLBuffer 上的零拷贝纹理（原先每次文字 draw 对 16 MiB 算哈希，选存档起明显卡顿，已改善）。**文字仍未显示**：UV 指向的行（≈7250）确有字形，常量/混合/顶点色正常，round133 起抓取文字目标纹理 `Documents/Text/` 继续查。
  - 新实现：多边形偏移（depth bias）、采样器 LOD bias 与 reduction（iOS 26+ `lodBias`/`reductionMode`）、GPU 纹理间 `CopyTextureToTexture`（blit）。
  - 诊断：`KERNEL_WAIT`（每 ~30 s 列出阻塞 ≥5 s 的客体线程及等待起点调用栈）、`NVN_NULL_POOL_DIAG`、`HOST_TEXT_DRAW`、`HOST_GLYPH_CACHE`。
- round134–136（2026-10-16）：选人后停在加载页 / 闪退。
  - round134（脚本按键）：选完角色后读完开场关卡 Course900 资源（present≈2300）就再无文件读取，加载画面持续 1 万多帧，`KERNEL_WAIT` 无阻塞线程。用户手动那次（round135-user）是**宿主代码**段错误（`SDK_HOST_ABORT_OBSERVER: fatal signal`，无位置）。`crash_report.c` 现在对宿主代码的故障也记 `HOST_FAULT`（符号 + 镜像偏移 + 帧链）后再交回原处理。
  - round135 定位真正的停点：加载页第一次 **DispatchCompute**（submit 1113），回放当时不支持计算着色器。本轮实现计算派发：
    - `BindProgram` 按 sdk+0x4672f0 语义：`stageBits & 程序阶段` 含计算位（bit 5，阶段位 = `1 << NVNshaderStage`）只换计算阶段，否则换整个图形部分；旧的 `0x11` 检查是错的（游戏一直传 0x1f 才没暴露）。
    - `BindImage` 记录；派发按反射解析 uniform（`cbuf`，以及经 c[0][0x210+16r] 地址读的 `uptr`）、存储缓冲（c[0][0x310+16r]）、纹理、图像（c[2][0x120+8r]，sdk+0x46a280）；Metal 计算编码器派发，图像为带 ShaderWrite 的 GPU 纹理（`NVN_TEXTURE_FLAGS_IMAGE`）。CPU 内容尚未上传的图像被读、渲染关闭时的派发、线程组尺寸与控制段不符等都按名停止。
    - 控制段（Lib/agl、Lib/gsys、Effect 共 58 个计算着色器普查）：线程组 x/y/z 在 +0x748，共享内存字节数在 +0x710；计算代码区没有 0x30 头 / SPH，翻译器从第 0 字节解码（与 D2R 一致）。普查目录 `DrawCensus/cs-<key>/`。
    - 着色器积攒问题顺带修：片元/计算阶段 buffer 号可用 0..30（15、16..30 只对顶点阶段保留）；未启用的顶点属性按 float 声明（运行时给 (0,0,0,1)）。构建 135/138（原 130），剩 3 个片元着色器普查时纹理槽未绑定。
  - round136（普查扫描）：**第一次进入关卡**（开场关卡 Course900：马里奥、云、城堡、金币、UI 图标），但只有约 6 fps，缺着色器的 draw 被跳过（黑块、蓝色剪影），数字等文字仍不显示。第一个计算着色器普查 `cs-1fb9d970e6bbceb8`：计算代码区是 0x100 字节头（magic `79563412`，其余为零）+ 指令（c1 = 0x100 + 指令长度对齐 0x100，58 个离线样本全符），线程组 32×1×1；它经 c[0] 读存储/uniform 地址表做地址运算 → 构建验证 c[0] 读取只落在地址表内（`drv 0 <buffer>`），宿主按 NVN 布局（u64 地址 + u32 大小）填表。截图改按 present 的裁剪矩形保存（关卡里窗口 1920×1080、裁剪 1280×720，屏幕上本来就是放大显示的）。
  - round138：加载页又卡住（60 fps、无文件读取、所有工作线程在 main+0x66bf24 空闲），与 round134 相同；round136 慢速时反而通过——与时序有关。新增 `KERNEL_RUNNING`：每 ~30 s 采样未阻塞客体线程的 pc 与调用链（挂起读寄存器后立即恢复），定位它轮询的条件。
  - **文字根因（round138）**：文字顶点着色器 982e87b1faa0b87f 用两个间接跳转（BRX，c1 跳转表各 3 个目标）按顶点角选 UV 字段；Ryujinx 的 AST 优化器把每个 BRX 只保留一个目标，UV 取错 → 采样为 0 → 片元全部丢弃。DebugMode（不跑该优化器）三路分支都在。构建改为所有阶段 DebugMode（D2R 原本的默认）；196 个阶段在 Mac GPU 上全部编译通过。带 BRX 的其它着色器（粒子、材质）同样受益。
  - 性能：每个 draw 单独一个 render pass（附件整张 load/store）+ 每个 uniform 一块 64 KiB 拷贝分配，关卡每帧 141 个 draw 只有约 6 fps。改为同附件的连续 draw 合并为一个 render pass，拷贝统一进前置 blit、从 4 MiB 暂存块切窗口；其它 GPU 操作取命令缓冲时先收尾当前批（保持命令顺序），写存储缓冲或采样本批附件的 draw 结束本批。`HOST_MEMORY` 行报告 draws/passes 与 fps。
  - round139–140：**文字正常显示**（选角色界面中文/英文、按钮文字都在）；按键图标显示为"?"——Switch 共享字体第 5 类（任天堂扩展字体，私用区按键符号）在 iOS 上没有对应字体（功能差异，待自制图标字体）。标题界面合并 pass 后 59–60 fps。3D 采样纹理已支持；计算着色器 `1fb9d970e6bbceb8` 在 Metal 上实际执行（`NVN_DISPATCH_EXECUTED`）。进入关卡后 5–22 fps（普查扫描轮：缺着色器的 draw 被跳过、新着色器首次使用时在设备上编译），换场景时客体空指针 `main+0x8f68a8`（读 `[[x19+0x40]+0x38]`，调用方 main+0x1b97c8c 虚函数 +0x48），待查。
  - round141–143：
    - **换场景闪退**（main+0x8f68a8）：崩溃点在声音联动（SLink）的委托代码里，旁边就是声音区域的名字；同期日志显示**所有流式音频都打不开**——音频引擎用 `content://Sound/Resource/Stream/*.bwav`（双斜杠），宿主 RomFS 没有像 nn::fs 那样合并连续分隔符，一律 0x202。修复后 round143 越过原崩溃点（present 3300）继续运行。另修 SignalToAddress 类型 2（count ≤ 0 且有等待者时按 7.0.0+ 写 value−2）。
    - **声音**：sdk 进程内 DSP 把 DeviceSink（0x13）当空操作（真机由 ADSP 固件送到输出设备）。宿主在每个命令列表处理后读出 DeviceSink 的输入（游戏的 MainAudioOut 读上采样器的 48 kHz 输出，2 声道），经 RemoteIO（48 kHz 立体声 float，SoloAmbient：静音键生效）输出。round143：输出回调 932 次仅 3 次欠载、环形缓冲约 19 ms。
    - **性能**：采样纹理每次 draw 都对整块存储做 FNV 哈希（关卡里每帧上百 MB）→ 改为写保护脏页跟踪（`page_guard.c`：上传后把纹理页设为只读，CPU 写入触发异常→标脏→放开页、重试写入；romfs/存档读文件前先放开目标页）。关卡帧率 5 fps → **约 41 fps**（标题 60 fps）。
    - **实际游玩**：脚本支持按住区间（`RIGHT@150-480`），`A/2.5@25-150,RIGHT@150-480,Y@150-480,A/1.5@151-480` 让角色在开场关卡里向右冲刺跳跃，开场标志动画正常出现。
    - 仍缺：部分背景（远景天空、地面）仍是黑块/纯色；按键图标“?”（任天堂扩展字体）。
  - round144–145（继续追换场景闪退）：
    - 闪退仍在（round145 在开场标志后的场景切换，present ~6300；round144 同处变成停在加载转圈）。绑定对象由 main+0x5775dc 初始化：按 `Model/<名>.bfres`（main+0x41d16c，格式串 main+0x2a7e4b2）在 `"DistanceView"` 类别里找已加载模型，找不到就把 +0x40 留空，之后 main+0x8f68a8 不判空直接取。`"DistanceView"` 是远景（DV）模块（main+0x657b44 创建的单例，vtable 0x35f0510，与崩溃对象同一类族）。
    - **关键事实**：开场关卡的远景主模型 `Model/DVHajimariSougen.bfres.zs`（15 MB）、`DVCommonEnkeiCastleFlower`（8.6 MB）、`DVHajimariSougenWonderDokan` 在 RomFS 里都有，游戏**从未打开**（同组小文件 `…Anm` 却读了；20 MB 的其它文件正常读取）——截图里的大片黑块正是远景层。所以根因是远景模块没有去加载它的模型（不是读文件失败）；崩溃与黑背景同源。下一步：找远景模块启用/加载模型的条件（它读的设置、内存块或 GPU 能力查询）。
    - 加载转圈时的“轮询”线程 0x1029/0x1020 是消息循环（main+0x66bf10 → 处理函数 vtable+0x88）里的 SleepThread，属正常空转；`KERNEL_RUNNING` 现已带宿主符号与 SleepThread 的客体调用链。崩溃时转储 x0–x28 与 x19 对象（`GUEST_FAULT_REGS` / `GUEST_FAULT_X19`）。
  - 脚本按键后 1.5 s 自动截图（`Documents/Present/present-<帧>.png`，日志 `HOST_PRESENT_CAPTURE ... requested`）。
  - 文字：round135 `HOST_TEXT_SAMPLER` 采样器正常（无比较、LOD -1000..1000、min3 mag1），问题不在采样器。
- 测试：`metal3_encode_test` 的渲染目标替身改用真实描述符（原来用 2D 纹理冒充 cube，AMD 上碰巧通过，Apple GPU 验证层拒绝）。宿主测试 98 项通过（2 项跳过）。

  - round146–180（2026-10-16/17，换场景闪退已修，画面问题持续追）：
    - **换场景闪退/远景纯色已修**：alias 区只剩约 40 MiB，VAMM 堆为 DV 模型申请地址区失败（0x1803）；alias 区改为占满剩余地址空间（5376 MiB）。
    - 停名修复：纹理视图换目标类型（2D↔2D 数组等）、深度钳制（MTLDepthClipModeClamp）、0 号纹理句柄=空槽（读 0）、2D 数组被 2D 指令采样读第 0 层、后台禁止提交 GPU（gpu_gate.c：resign 起等待 active）、CPU boost 在实时线程上保留实时策略、轮次环境写入 Documents/launch-env.txt（主屏启动同配置）。
    - **着色器全量离线构建**：tools/extract_shaders.py 从 RomFS 的 BNSH（bfsha/esetb/blarc/sarc）取出 7575 个程序；与普查合并共构建 6463 个阶段，留下 107 个（指令需绑定贴图类型的顶点阶段）。离线阶段的顶点属性/输出按 float，宿主逐次绘制核对格式类别（不符按名停）。超过 16 个采样器的 12 个阶段改走参数缓冲（smpov）。agl/gsys 的 sharcb 尚未解析（仍靠普查）。
    - LINEAR_RENDER_TARGET(0x40) 贴图按线性布局读（天空查找表 64x1 RGBA16F 原来只读到前 64 字节）；顶点位置加 [[invariant]] + preserveInvariance。
    - 诊断工具：WONDER_FRAME_DUMP_EVERY / WONDER_DRAW_DUMP（逐程序画面+深度）/ WONDER_TEX_DUMP_FS（贴图各级+alpha+常量与顶点数据）/ WONDER_DIAG_NODEPTH_FS / WONDER_SKIP_CS / WONDER_TRACE_VS（记录时绑定调用链、回放绑定/绘制）/ NVN_RENDER_ENABLE 计数。
    - **仍未解决：关卡前景灌木、斜坡草、音符块所在区域显示为清屏蓝色**。已确认：深度预通道（如 1451a723-2e8b0a9c，写深度+ID）每帧都画，同一网格的颜色通道（7c598128-571c2316，EQUAL）与低分辨率层通道（bc0e5f06-0f000c1e）**游戏根本没录制**（记录时追踪 0 次；round136 首次进关时曾录制）。排除：采样器上限、D24 精度（EQUAL→LEQUAL 无效）、CPU/GPU 竞争（SUBMIT_SYNC 无效）、条件渲染（0 次跳过绘制）、计算着色器 cs-45bc61e5。所有绘制都经 main+0x16b8fc→main+0x77b8d4（绑定材质程序）；渲染主循环 main+0x1108a0 按层调用 main+0x16b1f0 画各层清单——灌木颜色没进任何层的清单，下一步逆向分类/剔除逻辑。
    - 已知：从桌面切走后有时被系统以 signal 15 结束（另一个项目 zelda-la-mobile 同机安装/启动 App 时也会挤掉本 App）。
  - round181–191（2026-10-17，**前景灌木/斜坡草/音符块变蓝的根因找到：着色器键撞车**）：
    - 逐层真机采样（LLDB 附加运行中的游戏，不重启）：渲染主循环的层名为 Model(ZPrePass)/Model(Opa+AlphaMask)/AfterDepthSilhouette/Xlu/XluAfterDOF/DrawGIMapOpa，**没有整层被跳过**；main+0x77b810 按通道掩码 `[item+8]` 筛模型，预通道画过的每个模型都在 Opa 或 0x3 通道再画；main+0x1e555a0 → 0xe5fe0（选程序）全部返回成功。之前“颜色通道没录制”的结论错误（按 vs 名对不上，颜色通道用另一组 vs 变体）。
    - 逐绘制日志（WONDER_DRAW_LOG 的帧界可由调试器改写；新增清屏颜色、写掩码、混合、深度、剔除、绕向）：场景缓冲 R11G11B10F 清成 (0.2, 0.3, 1.0)——蓝色是游戏自己的清屏色；前景剪影预通道 `*-f183cb0b` 写掩码 0、深度 LESS；**按索引数配对，每个前景预通道网格都有同网格的 EQUAL 颜色绘制**，但灌木处一像素未写。
    - 对颜色着色器关深度+剔除（运行时 setenv WONDER_DIAG_NODEPTH_FS）灌木出现；只关深度（round188 WONDER_DEPTH_EQUAL_AS=always）或只关剔除（WONDER_DIAG_NOCULL_FS）都不出现 → 颜色绘制的几何与预通道不一致。常量转储（运行时改写 tex_dump 名单）：前景大面片预通道 b4c6c67e 与颜色 fbadfa4c-fb314975 的顶点、常量完全相同，但翻译出的 b4c6c67e 用 `-c4[8].y`（-19.5），颜色用 `+c4[8].x`（-746）；**原始 Maxwell 代码三处都是 `FADD …, c[0x4][0x80]`**，翻译结果不是这份代码。
    - **根因**：着色器键只取控制段 sha256(control[0:0x880])；SecredUber 的 6494 个控制段里 **1395 个被代码不同的阶段共用（多出 2552 份代码）**（b4c6c67e 2 份、e50bb0ac 6 份、1451a723 4 份）。离线提取/普查对同一键只留第一份代码，运行时按键取 Metal 函数，于是这些阶段跑的是别的变体的代码。
    - **修复**：键 = sha256(control[0:0x880] ‖ code[0:len])，len 为 control+0x704 的 u32（与设备普查字节核对：设备上的 e50bb0ac 代码与 6 份变体中恰好一份一致）。宿主在 nvnProgramSetShaders 时算一次存入程序快照（读不出代码按名停），回放绑定程序时随阶段拷贝；extract_shaders.py / wonder_shader_build.py 从字节重算键（旧普查目录名里的旧键不再采信）。新增回归测试：同控制段不同代码 → 不同键。宿主测试 113 项通过（2 项跳过）。
    - 全量重建：离线提取 9030 个程序 / 9046 个阶段（原 7575 个程序），构建 9020 个阶段（留 112 个）。round192 启动即停 `NVN_SHADER_MISSING`（agl/gsys 启动着色器的新键只有普查能给，以前是撞车顶替）；round193 采集停在离线阶段属性类型猜测（4ee141e7 第 6 属性 RGBA8UI，离线按 float）→ 采集模式下类型不符也记普查、跳过该次绘制（非采集仍按名停）。
    - **round194（采集版，A68856C9）真机结果：水管关卡前景深绿灌木、彩色音符块、斜坡/平台上的草、砖块地面全部正常，蓝色消失**（截图 work/analysis/first-frame-round194/screenshot-course.png）。采集到的普查（新增 90 个目录）已加入 census-dirs，全量重建 9036 个阶段（留 107 个）。
    - **round195（正式版，A00077D4）**：从标题走到水管关卡全程无停点，关卡前景灌木、音符块、斜坡草、地面正常（截图 work/analysis/first-frame-round195/screenshot-course.png）。蓝色问题关闭。
    - **按键图标“?”已修（round196，AC0E1FF2）**：共享字体第 5 类（任天堂扩展字体，私用区按键符号）在固件里，离线主机没有；统计 Mals 文本用到的 27 个私用区码位（E090–E097 箭头、E0AB–E0AE 方向按钮、E0E0–E0E3 ABXY、E0E4–E0E9 L/R/ZL/ZR/SL/SR、E0EA 十字键、E0EF/E0F0 +/−、E100/E101 摇杆；含义由文本上下文推定），tools/make_icon_font.py 自绘图标字体（字母轮廓取 Mac 的 Arial Bold，产物在 work/，不进 Git），打包为 dm/fonts/wonder-icons.ttf，缺失按名停。**功能差异**：图标是自绘替代，外形与任天堂原版不同。用户确认按钮显示正常。
    - round196 卡在离线阶段属性类型（vs 01b4c315 第 6 属性 R16UI）→ **离线 float 属性绑定整数格式时，宿主按翻译器写整数属性的方式改写 MSL**（uint4/int4 声明 + as_type<floatN> 读取），按类型组合编译缓存并给出对应反射（shader_library.c `wonder_shader_retype_attributes`，有 C 与 GPU 编译测试）；声明为整数却绑定 float 仍按名停。round197 待真机。
    - **性能（round198–203）**：Time Profiler（xctrace 附加运行中游戏）定位并修复：CallCommands 线性扫描句柄表（录制线程 ~70%）→ 哈希；address_span 线性扫描 ~1200 个池（回放 ~20%）→ 排序索引+线程缓存；每次绘制新建 64 KiB 常量缓冲与纹理视图 → c1 按反射、drv0 按内容、视图按帧缓存；程序表 → 哈希。卡的关卡 11–13 → 约 22 fps。剩余瓶颈：游戏/回放/GPU 串行（回放 41%、GPU 25% 忙），主因是条件渲染每次都 finish GPU（每帧约 50 次）。条件渲染“读未落地写入表”快速路径（report_shadow.c，按宿主内存跟踪）校验与抽查 0 差异，但开启后世界地图闪烁（推测时序变化影响依赖 GPU 计时的逻辑），**默认关闭**（WONDER_FAST_CONDITIONAL=1 研究用）。
    - **宽屏（round204–207，进行中）**：主机内存扫描 + 写入断点定位：关卡相机为正交投影（高 15.5×缩放），每帧在 main+0x2e2b30–0x2e2b6c 写上下左右，宽高比读 main+0x2aeedb4（0x3fe38e3b，主程序唯一的 16:9 常数；另一引用 main+0x6a7630 为默认透视投影构造）。运行时改该常数：关卡视野变宽（压在 16:9 目标里）；但渲染尺寸也由它推出并受 1280 上限约束，标题画面缩到左上（round206）。runtime/widescreen.c（宽高比/拉伸/常数核对写入，有测试）、present 按比例横向拉伸已实现，**默认关闭**（WONDER_WIDESCREEN=1）。剩余：放宽渲染缓冲上限、2D 排版适配、离线剪口（patch_nso.py CUTS，Wonder 构建号 ff773e90972d544eb79406eaa65396d5）。
    - 诊断工具：wonder_diag_scan_screen / wonder_diag_scan_u32（LLDB 调用，输出到 Documents/loader-probe.log；该日志 80 MB 封顶）；LLDB 分块读取整个客体堆约 20 MB/s。

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
- round38 run `E77572E4-3904-4DEC-BE04-1B7C275BAA93`：MapTransferMemory 权限须等于所有者权限（None），已修正。round39 run `4E225EA1-E27A-44DD-B7A4-B3434AE625BF`：音频渲染器打开，4.3 MB 工作内存映射成功；游戏设置设备音量（扬声器 2.0，服务端本就钳到 [0,128]，校验已改）；另一线程的 `fs::SetPriorityOnCurrentThread` 经 `IsEnabledAccessLog` 查询 fsp-srv 全局访问日志模式，按零售默认 0 接管。
- round40–42：USB 输出设备音量接受；渲染器事件改为宿主内核事件（进程内服务端的事件不跨进程，句柄为 0）。round42 run `87B525B1-74AB-4454-A283-DF895100094F`：渲染器打开并启动，但没有 DSP 帧：本构建里没人创建 AudioRenderSystemManager 或调用 Add（那是音频系统模块的职责）。round43（待真机）：宿主创建管理器实例，渲染器首次启动时调用原生 Add（接口对象 +0x70 为 AudioRenderSystem，核对 Start 分发桩 sdk+0x20d6c0）；`nn::time` 14 个接口按 iOS 系统时钟与时区实现（用户/网络时钟、历法转换、设备时区名、网络时钟可用性任务立即成功）。
- round43–44：MapMemory 接受模块数据段（音频管理线程栈在 sdk 静态区）。round44 run `21076A91-AC75-467B-8437-E3BA11BDCA15`：宿主 DSP 启动，原生 CommandListProcessor 处理命令列表；nn::time 就绪；主线程停在 `oe::EnterExitRequestHandlingSection`（无 AM 代理对象，空指针）。round45（待真机）：补齐游戏导入的全部 20 个 nn::oe（显示版本取自 control.nacp 的 1.2.0、伪设备 ID 持久化、焦点模式、音量平衡、活动时间、性能配置、退出请求区段计数、闲置检测）。
- round45 run `AC72FD44-D739-48A6-83D6-19C8EA583E98`：OE 退出区段进入，语言 zh-Hans；几个可选 RomFS 文件返回 0x202，游戏继续；停在 `nn::friends::Initialize`（sdk+0x346240 连 sm:）。round46（待真机）：按“离线主机”批量接管启动路径上的联网服务：friends 11 个（空好友列表、异步请求同步完成、游玩记录追加到本地文件、宿主签发的 0x40 字节登记键）、nifm 8 个（网络永远不可用；取 IP/代理配置按名停止）、aoc 计数 0、nsd 环境 lp1、prepo 保存/立即上传（不上传，计数日志）、err ShowError 三个（带结果码停止）。存档：新增 `savedata.c`，每用户一份存档镜像，挂载时复制为工作副本，Commit 原子替换（committed.new/old 中断可恢复），Unmount 丢弃未提交修改；接管 fs 存档相关 16 个接口，OpenFile/ReadFile/GetFileSize/CloseFile/GetEntryType 按挂载名与句柄槽位分派到存档或 RomFS；`tests/savedata_test.c` 覆盖结果码、追加规则、锁定、目录列举、提交/丢弃与中断恢复。
- round46 run `3A8DBB67-9E53-47E3-8156-57F44590AE33`（debugger=1，52 项测试、xcodebuild、签名、安装通过，无崩溃报告）：friends 离线就绪，OE 退出区段通过，又创建了 2 个游戏线程（0x79/0x7a）；主线程停在 `nn::ngc::ProfanityFilter::Initialize`（sdk+0x38648c）→ `ngc::detail::CreateServiceByHipc`（sdk+0x387298）→ `sm:`（SVC 0x1F）。游戏调用点 main+0x68eff0。
- round47（待真机）：ngc 客户端保持原生（参数检查、UTF-8/UTF-16 处理、工作缓冲区堆、共享代理引用计数 sdk+0xdc6438），只把 `CreateServiceByHipc` 背后的 IService 对象换成宿主对象（x8 返回 SharedPointer，用跳板写入）。vtable 按 sdk CmifProxy 对象（sdk+0xd0f650..0xd0f688）：AddReference/Release/GetProxyInfo/GetInterfaceTypeInfo/GetContentVersion/Check/Mask/Reload。宿主没有 NG 词库（任天堂系统数据，未提供），所以 Check 恒报未命中、Mask 原样返回且屏蔽数为 0——这是明确的功能差异，不是伪造结果；GetContentVersion/GetProxyInfo/GetInterfaceTypeInfo 按名停止。每次 Check/Mask 都要经 `ngc::detail::GetLanguageSetting` 调 `settings::GetLanguageCode`（同样连 `sm:`），按 iOS 首选语言作答（与 oe::GetDesiredLanguage 一致）。新增 `runtime/ngc_service.c` 与 `tests/ngc_service_test.c`（经 vtable 偏移调用、引用计数、未终止文本/输出过小/释放后调用/外来对象等停止）。完整宿主测试 53 项。
- round47–52（2026-10-16）：round47 run `EC535B5F` ngc 服务对象通过；`os::SetUserExceptionHandler` 需要 GetInfo(20) 进程本地区（已实现，0x200 字节零页）。round48 发现 `settings::operator==(Language, LanguageCode)` 并非纯函数：经 `MakeLanguageCode` 连 `set:`；新增宿主 ISettingsServer（`runtime/settings_service.c`，18 种语言表、系统语言取 iOS，地区/键位/昵称等按名停止）。注意：新源文件必须同时加入 `tools/build_loader_probe.py` 与 `ios-probe/project.yml`（round49 漏了后者）。round49b `F854DC10` 首次 QueueSubmitCommands，开头是 ReportCounter(TIMESTAMP_TOP)，已按宿主时间戳回放。round50 `os::FlushDataCache` 读 CTR_EL0 被 iOS 拦截（SIGILL）；sdk 只有 3 处读取，`FlushDataCache` 经 PLT 强制接管为 `sys_dcache_flush`，另两处由 SIGILL 处理器精确模拟（LLDB 调试轮次下陷阱仍会结束运行）。
- round52 run `8FD64D64-9FE3-4FD4-94AE-36B110DE46D1`（debugger=1，55 项测试，165 个强制接管）：第 1 次提交回放完成，第 2 次提交第 272 条命令是第一个真实 DrawElements，按名停止“需要翻译后的着色器与 PSO”。启动期服务链已全部打通，下一阶段是着色器翻译与绘制执行（见 METAL2-RENDER-PLAN.md）。
- B3 首个 draw（计划 `docs/superpowers/plans/2026-10-16-first-draw.md`，2026-10-16）：
  - round53 run `CB36CC00`：首个 draw 普查（`Documents/DrawCensus`）——提交 2 第 272 条 DrawElements，6 个 u8 索引，程序 vs `b6ef435899b15f38` / fs `b56073e27e6d9488`，RGBA8 1920×1080 目标、视口 1280×720，深度模式 NEAR_IS_MINUS_W。
  - 离线链路：`tools/wonder_xlate`（D2R 翻译器的 Wonder 副本：顶点阶段做 NEAR_IS_MINUS_W 深度重映射；贴图句柄表按 sdk 实测为 `c[2][0x20+8r]`，不是 D2R 的 0x168；uniform r = c[3+r]）→ `tools/wonder_shader_build.py`（翻译查询全部取自普查）→ MSL 2.1 离散槽 + `.refl` 反射，随 App 进 `dm/shaders`，设备 `newLibraryWithSource` 编译并按键缓存。Mac 上没有离线 Metal 工具链，只出源码。
  - 设备端：回放把 draw 解析成宿主描述（`runtime/host_draw.h`）：着色器、UBO/c1/support、贴图（块线性解排布、BC 解码、内容变化重传）、采样器、顶点流（buffer 16+）、索引（u8 扩成 u16）、混合/写掩码/深度/剔除/视口/裁剪；`runtime/metal2_draw.m` 建 PSO 并编码。缺着色器时先写普查再按键停止（NVN_SHADER_MISSING）；不支持的状态按名停止（NVN_DRAW_UNSUPPORTED）。
  - round54/55：补 `nvnSamplerGetMinMagFilter`（266 个入口）；round55 第一个 draw 处死锁（编码时持有回放锁又去取渲染目标），已修正。
  - round56 run `84249253-BC93-403B-8AB0-B703884ECB37`：**首个 draw 在 FlyMe 真机执行**，此后每次提交都执行（提交 2–14），渲染目标读回 `Documents/FirstDraw/target-0.png`（1920×1080，sha256 8a5dae00…）：视口 1280×720 区域为不透明黑（0,0,0,255），区域外未写入。这是游戏采样 4×4 缺省贴图 258 的整屏四边形，**还不是可辨认的游戏画面**。
  - round57 run（4 个阶段已打包）：第二个程序 vs `0aeff6b7b3b657fd`（含 c1 跳转表）/ fs `ed9066c9e3f5cb63` 在设备编译通过；停在 `NVN_DRAW_UNSUPPORTED texture view on sampled texture serial 688`。整体审查后修正 8 项（流水线缓存键、UBO 越界读取改为 GPU 拷贝到补零的 64 KiB、GPU 错误按名停止、流 15、原点/深度模式校验、光栅状态与贴图类型校验等）；round58 复验：首个 draw 执行 12 次，读回图与 round56 逐字节相同（sha256 8a5dae00…），停点不变。下一步：采样贴图视图；之后每个新程序按“普查 → 离线构建 → 真机”循环，程序多时把普查扩成一次运行记录多条 draw（另写计划）。
- round59–93（2026-10-16）：按“普查 → 离线翻译 → 真机”循环推进到 57 个翻译后的着色器阶段、约 216 次提交、多次 present（加载转圈可见）。期间补齐：贴图视图（含立方体/立方体数组切片按 6 的倍数）、存储缓冲（c[0][base+16r] 绑定为设备缓冲）、绑定式与无绑定贴图句柄、跨行线性贴图与 2D 数组、条件渲染（GPU 完成后在 CPU 比较 addr 与 addr+16 的计数）、带裁剪/掩码的清屏、混合常量色、禁用顶点属性读 (0,0,0,1)、越界常量读返回 0（夹取读）、顶点流越界补零、常量缓冲动态下标保护（round62–69 的 GPU 页错误）；回放改在 16 MiB 栈的宿主线程上执行，Metal 编译离开客体小栈（round84/85 栈溢出）；`nn::account::LoadProfileImage` 返回宿主生成的 256×256 JPEG 占位头像（离线主机无用户头像，功能差异）。
- round94–95：`nn::hid::StartLrAssignmentMode/StopLrAssignmentMode` 接管（宿主只有内置手持控制器，无可重新配对的 Joy-Con，模式无效果——功能差异）；`VibrationNode::Update` 经 sdk PLT 调 `hid::detail::SendVibrationValues`，按“振动不允许”丢弃（与已有 IsVibrationPermitted=false 一致）。round95 run `5E55EAA1-8798-466B-AA5B-AB6F21B8EFDC`：**首次画出可辨认的游戏画面——标题主视觉图**，随后被 iOS 内存高水位（2098 MB）停止。
- round96 run `6C31DF64-8CD4-4CA4-9185-681515A15F2C`：`nvnTextureFinalize` 释放宿主 Metal 副本后内存稳定在约 670 MB（每 300 次 present 记 `HOST_MEMORY`）。但画面在主视觉图与黑/蓝/斜切之间闪烁。
- 闪烁排查（round97–107，诊断开关 `WONDER_PRESENT_EVERY`、`WONDER_SUBMIT_SYNC`、`WONDER_DRAW_LOG`，日志 `NVN_FRAME`/`NVN_DRAW_LOG`/`NVN_CALL_RECORD`/`NVN_CALL_REPLAY`）：
  - `nvnWindowAcquireTexture` 改为等该纹理上一次 present 的 GPU 工作完成再返回（原先立即标记完成）。
  - GPU 纹理改按存储位置（内存池 + 偏移 + 布局，`runtime/texture_storage.c`）识别：游戏每次 present 都在同一池偏移重建 9 张临时渲染目标，旧做法每个新对象拿到未初始化的 Metal 纹理；覆盖同段内存的新纹理淘汰旧条目，内存池销毁时一并清除。
  - 采样器 CLAMP_TO_BORDER：A11 实测在边框处恒采到 0（round100 `HOST_BORDER_COLOR_CHECK pixel=0`），透明黑边框映射为 `ClampToZero`（所有 GPU 精确）；不透明黑/白边框需该 GPU 实测支持，否则按名停止；MIRROR_CLAMP_TO_BORDER 停止。
  - 根因（round107 run `4E10C686-2F51-4980-9A26-F27D0CD21A65`）：`CallCommands` 原先只记句柄、回放时再查；游戏工作线程在本帧提交期间就为下一帧重录同一控制内存地址（句柄 0x331d382b0 调用时 2520 字节、回放时 9864 字节），UI 层每帧被执行 0/1/2 次。改为录制时快照被调用句柄的命令段（NVN 本就把控制项拷入调用方）。
- round108 run `1143D54A-EF0F-4C08-BDEB-4E05595F9DD8`：启动顺序与真机一致（加载转圈 → 圆形转场 → 标题主视觉图），标题出现后的 1571 帧每帧都完整执行 3D 层 + UI 层（63–66 个 draw），不再闪烁；内存约 640 MB；无停点（取日志时仍在运行）。
- 待查：标题主视觉图下方的 3D 场景本身渲染不对（单独显示时为蓝天/黑地/斜切色块），被 UI 完全覆盖所以当前画面正确；进入游戏前需修。宿主 present 截图只取窗口纹理左上 1280×720（屏幕显示按裁剪矩形缩放）。
- 导入服务统计（main 导入，已接管/总数）：audio 4/197（其余为原生客户端库）、hid 21/53（另经 sdk PLT 接管 detail::SendVibrationValues）、fs 26/29（余下 3 个是线程 fs 上下文/优先级的纯客户端函数）、account 11/25、nfp 3/20、time 14/14、oe 20/20、vi 5/5、friends 11/11、nifm 8/8、aoc 1/1、nsd 1/2、prepo 3/27（其余为本地报告缓冲区拼装）、err 3/9（其余为结果值类型）、ngc 5/5（客户端原生，服务对象由宿主提供）、settings 1/1（operator==(Language, LanguageCode) 经 MakeLanguageCode 连 set:，服务对象由宿主提供）；尚未涉及：socket 30、ldn 25、swkbd 12、ssl 8、ec 2——这些若被调用，SDK 原生客户端会连 sm: 并按名停止。
- 仍需：更多程序的普查与离线翻译、贴图视图/模板/逻辑运算等尚未映射的状态、展示路径上的真实游戏画面。六份旧样本仍不具备运行资格。

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
