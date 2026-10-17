> 最新进度见 [STATUS.md](../../STATUS.md)：A11 已通过 Metal 2 GPU 绘制/回读，以及六个目标着色器阶段的设备编译检查；游戏首帧仍待接通。

> 最新持续推进记录见 [ADVANCE-STATUS.md](ADVANCE-STATUS.md)：已越过核心掩码阻塞并在真机读取真实 RomFS；下文保留此前冷启动验证记录。

# 独立启动、构造函数和 nnMain

本轮范围：完成独立主线程与 SDK 冷启动验证，执行模块 DT_INIT，再验证 nnMain 的实际执行。这里的“进入 nnMain”不包含游戏主菜单、完整文件/系统服务或图形验收。

## 实现

- 显式 mmap 1 MiB 主线程栈和独立不可访问保护页，以 pthread_attr_setstack 交给独立 pthread。构建前校验原始 NPDM 哈希及栈字段；真机断言实际线程栈为 0x100000。
- 每次进程启动写唯一 UUID、PID、debugger 状态和 stage，并轮转上一份日志。只有 debugger=0 的新进程日志可用于无调试器验证。
- 校验 DT_INIT 执行区间和 init_array 边界，调用目标模块原生 DT_INIT；由它自行执行数组，不重复手动调用。
- 原生执行 SDK 初始化及构造后设置目标 argv，调用 main 的 nnMain（main+0x5b7e10）。以其后的实际 SVC/宿主接口调用证明入口已执行，不仅凭调用前标记。
- 补齐目标首先触发的 OE 通知订阅、消息队列、运行模式/焦点查询，UIKit 生命周期驱动焦点与恢复消息。主线程优先级支持合法设置/回读和参数错误，并映射到宿主 QoS。
- 未知 SVC 终止客体线程并向诊断 UI 报 BOOT_BLOCKED，不返回成功、不恢复客体执行；SDK 自身 fatal Break 和内存异常仍按崩溃处理。

## 已保留的故障证据

1. `cold-attempt1`：debugger=0，但自动分配的 pthread 栈被系统报告为 0x103000，旧的精确断言失败。改为显式栈分配，未放宽断言。
2. `boot3-main1`：1 MiB 线程、SDK、两个 DT_INIT 均返回；真实 nnMain 首次调用 OE 恢复通知时，未建立的 Horizon 会话导致 sdk+0x3989a4 空指针。已用有状态的宿主通知实现接管。
3. `cold-attempt2`：debugger=0，主线程、SDK、main DT_INIT（6610 个数组条目）和 sdk DT_INIT（97 个条目）均返回；nnMain 实际执行内存查询与三项通知订阅，停在未实现的 SetThreadPriority / SVC 0x0d。优先级实现已增加并通过宿主测试。

注意：服务初始化不等于所有后续 SDK 服务都可原样执行。未接管的原生服务客户端也可能先访问空会话指针；必须按故障证据接管具体边界，不能把“未知 SVC 会报错”当作覆盖了全部服务客户端。

最终安装版已独立启动验证：run ID `52205C0C-475E-4C84-9BFB-87307A5DE739`，PID `21112`，`debugger=0`；主线程/SDK/两个 DT_INIT 全部通过，nnMain 发起真实接口调用，优先级设置成功。下一项阻塞为 `SetThreadCoreMask`（SVC 0x0f，sdk+0x15f188，handle=0x1001、ideal_core=-1、mask=1）。客体线程已终止，宿主 UI 保留 BOOT_BLOCKED，未假装成功继续执行。日志与构建哈希见 boot-device-result.json。宿主回归测试目前 25 项通过；预期的非法输入/未实现导入中止负对照会在测试输出中出现 FATAL。

## 仍未完成

完整的后续系统接口、通用子线程与同步语义、RomFS 实际文件访问、游戏完整启动/图形、输入、音频、游戏存档与后台恢复。当前仍是诊断 App，不能玩游戏。A11 仅用于 CPU/宿主调试；参考 Metal 3 图形路径仍需较新设备。
