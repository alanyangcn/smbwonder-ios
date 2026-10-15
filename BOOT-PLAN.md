# 独立启动、构造函数与游戏入口

本轮按用户授权的第 1/2/3 步推进。固定 main/sdk/NPDM 身份；游戏派生文件仍在 funny/smbwonder-work，源码在本目录。

1. 独立主线程使用 NPDM 的 1 MiB 栈和保护页。真机从主屏幕结束旧进程后打开，日志以新 run ID、PID、debugger=0 和 SDK_INIT_COMPLETE 为证据。
2. 在同一线程按装载顺序调用两个模块原生 DT_INIT。DT_INIT 自行执行 init_array，不重复调用；核对入口可执行范围和数组边界，并记录开始与返回。
3. 构造完成后设置目标 argv，通过动态符号进入 nnMain。记录入口与第一次实际客体调用/失败；缺接口时按真实语义补齐。达到 nnMain 不代表能显示画面或游戏可玩。

保留 stage=sdk / constructors / main 三个构建检查点。任何闪退先读取唯一 run ID 对应的日志，不能把旧日志、调试启动或“调用入口前打印”当成冷启动/游戏实际执行的证明。

首个真机故障：系统自动分配的 pthread 栈报告 0x103000 而非 0x100000，触发旧的严格断言。修复为显式 mmap 1 MiB 可写栈加独立 PROT_NONE 保护页，再交给 pthread_attr_setstack；不放宽栈检查。
