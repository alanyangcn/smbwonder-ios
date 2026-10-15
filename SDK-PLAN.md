# SDK 初始化执行计划

范围：在本地 1.2.0 main/sdk 输入上完成 `_init_libc0 → nnosInitialize → _init_libc1 → nninitInitializeSdkModule → nninitStartup → _init_libc2`，先停止在构造函数与 nnMain 之前。沿用已验证的签名映射，不使用 JIT、不链接 D2R 玩法代码。

1. 以目标反汇编核对 TLR[0x1f0]（TLS）和 [0x1f8]（线程对象）；线程与内核内存使用独立实现/配置，未实现 SVC 立即报出号、参数与客体偏移。
2. 将未解析弱符号按 ELF 语义置零；未实现强符号保持可识别的失败，不返回成功。
3. 实现并在宿主测试内核信息、内存范围、堆与查询的可观察行为；保留非法参数与不支持查询的错误。
4. 真机先执行计时器门和 nnosInitialize，留存首次失败；按真实需要接齐内核调用和 SDK 服务。
5. 按顺序推进其余初始化。只有全部阶段返回且后置状态检查通过，才记录 SDK_INIT_COMPLETE；每次构建保留版本、代码哈希和日志。

验收边界：SDK 初始化不含全局构造函数、游戏入口、GPU 或可玩性。设备断开/锁定导致无法执行时保留具体原因，不以编译通过替代真机结果。

执行结果：本轮在 FlyMe 上按顺序完成初始化链，使用 7 个宿主服务初始化后端与 1 个 CPU 请求接管。完整服务接口未接齐；未执行构造函数/nnMain。详见 SDK-STATUS.md 与 sdk-device-result.json。
