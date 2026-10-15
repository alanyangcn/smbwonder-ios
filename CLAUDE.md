# smbwonder-ios：工作约定

《超级马里奥 惊奇》1.2.0（update 262144）原生移植到 iOS：客体 AArch64 代码在进程内原生执行，宿主接管内核 SVC、SDK 服务和 NVN→Metal。用中文回复。

## 先读
- `METAL2-STATUS.md`：当前停点、每轮结果、导入服务接管统计。**开工前读“首帧推进”一节的最后几条。**
- `work/analysis/first-frame/ledger.md`：逐轮执行记录（英文，含 run id、sdk/main 偏移证据和裁定）。
- 计划：`docs/superpowers/plans/2026-10-15-first-game-frame.md`；渲染设计：`METAL2-RENDER-PLAN.md`。

## 设备与环境
- 测试机 FlyMe：iPhone 8 Plus / A11 / iOS 16.7.16，USB 连接。先做 Metal 2 / MSL 2.1，暂不换新设备。
- Xcode：`DEVELOPER_DIR=/Applications/Xcode-26.6.0.app/Contents/Developer`（脚本已默认）。付费 Team `5H8VE3NN7D`，Bundle `two.local.smbwonder.loaderprobe`。
- `work/` 整体不进 Git（游戏文件、工具链、构建、设备日志）；唯一例外 `work/analysis/first-frame/ledger.md` 需 `git add -f`。

## 一轮真机验证
```sh
tools/device_round.sh first-frame-round<N>          # 宿主测试 → 构建 → 签名 → 安装 → LLDB 启动 → 取日志
tools/device_round.sh first-frame-round<N> --fetch-only   # 只重新取回日志
```
结果在 `work/analysis/<轮次>/`：`steps.log`、`Documents/loader-probe.log`（客体日志，看结尾的 `BOOT_BLOCKED` / `BLOCKED_CALLER` / die 信息）、`launch-*.log`（LLDB）、`crashes/`。
停点调用栈用 `python3 tools/symbolize.py sdk+0x... main+0x...` 换成符号；`--dis N` 反汇编（能解析 PLT 桩），`--find 文本 sdk` 查符号。
轮次名递增，不覆盖旧轮次；RomFS 档案（3.7 GB）按 sha256 标记只传一次。

## 每轮循环
1. 跑轮次 → 读日志定位停点 → 符号化调用栈，用 sdk/main 反汇编确认真实语义（偏移写进注释和 ledger）。
2. 实现 → 加/改测试 → `python3 -m unittest discover -s tests -q` 全部通过。
3. 更新 `METAL2-STATUS.md`（本轮 run id + 结果、下一轮“待真机”内容、导入统计）和 ledger。
4. **每个大步骤提交一次 commit，不要 push。** 然后跑下一轮。

## 实现原则（项目一贯的裁定）
- **不跳过、不伪造**：缺语义或遇到不支持的参数时按名停止（`die(...)`，日志里能看到函数名），不返回假成功、不画假帧。
- 能用 sdk 自带原生代码的就用原生（例：音频走 sdk 的进程内 DFC 服务端，ngc 只替换服务对象）；宿主只替换会连 `sm:` 的服务层或平台层。
- 宿主接管用 `HLE_FORCE(改编名, 理由, ...)`：每条必须写理由；**没有任何模块引用的条目会让装载器报错中止**，只接管确实被引用（main 导入或 sdk JUMP_SLOT）的名字。C++ 经 x8 返回的 `SharedPointer` 用 naked 跳板（参考 `audio_platform.c`、`ngc_hooks.c`）。
- 苹果 AAPCS64 与 Switch 的差异：<32 位参数（bool/char/short）声明成 int/unsigned 自己截断；可变参数函数不能直接用 C 的 `...`。
- 离线主机：单个本地用户、无网络、无好友、无 DLC、不上传报告；功能差异（如无 NG 词库、系统字体替代）要在 STATUS 里明说。
- 可单测的逻辑放进不依赖 `hle.h` 的独立文件（如 `savedata.c`、`ngc_service.c`），在 `tests/` 里用 C 测试 + Python 包装；新运行时文件要同时加进 `tools/build_loader_probe.py` 和 `ios-probe/project.yml`。
- 删除安装失败后的 App 会丢 Documents（含存档），脚本已禁止自动卸载，不要改回去。
