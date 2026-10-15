> 最新进度见 [METAL2-STATUS.md](METAL2-STATUS.md)：A11 已通过 Metal 2 GPU 绘制/回读，以及六个目标着色器阶段的设备编译检查；游戏首帧仍待接通。

# 持续移植进度

目标：按用户要求持续推进代码和 FlyMe 真机验证，直到出现确切的新设备需求。已达到所选 D2R NVN→Metal 路径的设备能力停点。

## 本轮已验证

- `SetThreadCoreMask` 实现单虚拟 CPU 的合法设置/回读，正确处理 -1 不指定首选核、-2 进程默认、-3 保持首选核及错误参数。iOS 物理调度仍由系统负责。
- 真机越过原 SVC 0x0f，调用 `QueryMountRomCacheSize` 时触及 Horizon sm 服务。新增首个缺失 SVC 的端口名和调用链日志，定位到文件挂载边界。
- 新增只读资源归档工具和 C 后端：索引校验、大小写敏感路径、文件句柄代数、pread、短读/精确读、文件大小与条目查询；写入被拒绝，未实现的存档接口仍未接管。
- 本地完整合并 RomFS：19034 文件、3730968244 字节；归档 3732315409 字节，SHA-256 `adbbd1b5147480ad078e7eee80955929e56b065c3ef41670e7e15a79aa7cface`。已传到 FlyMe 的 App Documents/wonder-romfs.wrom；不会随普通代码更新重复传输。
- 真机 run `FBC4EA1D-1534-4A76-9B90-6B653692823C`，PID 21252，debugger=1：成功挂载 `content:`，实际读取 `System/HeapSize/Product.Nin_NX_NVN.HeapDef.byml.zs` 1319 字节及 `System/RegionLangMask.txt` 144 字节。
- 本轮上述进度是调试器辅助运行；之前 SDK/构造函数/nnMain 的独立冷启动证据仍保存在 BOOT-STATUS.md，不能把本轮调试运行写成新的冷启动验证。

## 当前停点：图形初始化需要较新设备

最终真机 run `3C496E05-4CF8-41F7-8050-89B9AB6472B6`，PID 21319，debugger=1（达到停点后已正常分离调试器）：

1. SDK、main/sdk DT_INIT、nnMain 继续通过。
2. iOS 实际首选语言转为 `zh-Hans`；越过原来的空会话崩溃。
3. 实际读取四个资源：堆定义 1319 字节、地区语言表 144 字节、1.2.0 地址表 2393 字节、运行配置 148 字节。
4. 到达 `nv::InitializeGraphics`，请求显卡驱动资源区大小 0x1000000。
5. Metal 实际查询：`Apple A11 GPU Metal3=0 Apple7=0 argumentBuffersTier=0 BC=0 RAM=3118219264 OS=16.7.16`。
6. 按用户指示停止客体，显示 `DEVICE_UPGRADE_REQUIRED`。没有把 InitializeGraphics 返回成功，没有建立虚假的 NVN 设备或首帧。

此前 `advance-run4` 在原生 NVIDIA 驱动路径遇到 SVC 0x15 CreateTransferMemory；它属于这里应整体替换的显卡驱动胶水，并非已验证的子线程需求。新代码在驱动边界检查宿主能力，避免继续执行 Horizon nvdrv IPC。

参考后端的 Metal 3 分支使用直接 GPU 地址和资源 ID，参考 iOS 外壳同样检查 `MTLGPUFamilyMetal3`。A11 实测不满足。此结论针对当前所选图形架构，不意味着重新设计旧 GPU 后端在理论上不可能。

**换机仍不等于移植完成。** 较新设备上会明确显示 `GRAPHICS_BACKEND_PENDING`，直到目标 NVN/VI、着色器、贴图和首帧路径真正接通；不会因 GPU 支持就假装成功。通用子线程/同步、输入、音频、存档及完整游戏流程也未验收。

下一阶段使用支持 Metal 3 的设备接入参考图形路径（参考 Metal 3 构建分支目标 iOS 18+），首先验证 NVN 取址、VI/CAMetalLayer 和最小实际提交；再完成 Wonder 着色器翻译、反射和首帧对照。较新设备的 GPU/BC 能力及内存仍按实测记录，不能沿用 D2R 的性能/可玩结论。

## 验证

- 28 项宿主测试通过，包含核心掩码错误码、真实资源读取/短读/非法输入、iOS 语言映射，以及所有 GPU 检查结果均不允许虚假返回图形初始化成功的负对照。
- iOS 编译与签名校验通过；最终包已安装 FlyMe。
- 本轮进展为调试器辅助运行。没有把此前 SDK 冷启动证明扩写成当前版本已独立冷启动或已可玩。

## 证据

日志目录：`/Users/yanglipeng/funny/smbwonder-ios/work/analysis`。

- `advance-run1/Documents/loader-probe.log`：越过 core mask。
- `advance-run2b/Documents/loader-probe.log`：文件挂载调用链。
- `advance-run3.log/Documents/loader-probe.log`：完整索引挂载和两次真实资源读取。
- `advance-device3.log`：语言查询空会话崩溃栈。
- `advance-run4/Documents/loader-probe.log`：语言查询通过，实际读取四个资源，原始 NVIDIA 驱动的传输内存请求。
- `advance-run5/Documents/loader-probe.log`、`advance-final/Documents/loader-probe.log`：重复验证 GPU 实测和 DEVICE_UPGRADE_REQUIRED；最终一次启动后已正常分离调试器。
- `advance-tests5.log`、`advance-xcode5.log`：28 项测试及签名构建。
- `romfs-pack.log`、`romfs-upload.log`：本地归档与传输。
- `advance-device-result.json`（源码目录）：对应输入、源码、App、日志的 SHA-256。

游戏尚无图形、菜单或可玩流程，线程/同步等后续依赖将按实际运行继续补齐。

接口与能力依据：[Atmosphère 线程 SVC](https://github.com/Atmosphere-NX/Atmosphere/blob/master/libraries/libmesosphere/source/svc/kern_svc_thread.cpp)、[核心负值常量](https://github.com/Atmosphere-NX/Atmosphere/blob/master/libraries/libvapours/include/vapours/svc/svc_types_common.hpp)、[Apple Metal 支持设备](https://support.apple.com/en-us/102894)。本地参考代码：`dm/ios/Sources/main.m` 的 Metal 3 检查、`dm/gfx/nvn/nvn_mtl.m` 的 GPU 地址/资源 ID 使用。

复现宿主测试（在 `/Users/yanglipeng/funny/d2r-mobile`）：

```sh
python3 -m unittest discover -s tests -q
```

重新生成诊断包和 Xcode 工程：

```sh
DEVELOPER_DIR=/Applications/Xcode-26.6.0.app/Contents/Developer python3 tools/build_loader_probe.py --stage main --xcodegen /Users/yanglipeng/funny/smbwonder-ios/work/tools/xcodegen/xcodegen/bin/xcodegen
```

生成工程位于 `/Users/yanglipeng/funny/smbwonder-ios/work/build/loader-probe/project/WonderLoaderProbe.xcodeproj`；scheme `WonderLoaderProbe`，签名 Team `MFXJ9AKFM5`。用 Xcode 选择已信任的真机运行即可。资源归档通过 `pack_romfs.py` 从本地合并 RomFS 流式生成，再上传到 App Documents/wonder-romfs.wrom；不能用空文件替代。完整归档 SHA-256 已在 Mac 计算，手机上验证了索引边界并执行了四次真实读取，尚未重新计算手机端完整归档 SHA-256。
