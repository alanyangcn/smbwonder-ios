# 新机器上从零搭建

仓库里只有源码、测试、文档和第三方源码快照。游戏文件、密钥、工具链、构建产物和真机记录都放在 `work/`，不进 Git。下面三步就能在另一台 Mac 上把 `work/` 重建出来，并且与原机器逐字节一致：每一步都会按 `verified-inputs.json` 核对哈希。

## 需要准备

| 项目 | 说明 |
| --- | --- |
| Mac + Xcode | Xcode 26.6，默认路径 `/Applications/Xcode-26.6.0.app`；放在别处时设置 `DEVELOPER_DIR=<Xcode.app>/Contents/Developer`。还需要命令行工具（`xcode-select --install`），提供 git、make、clang、python3。 |
| 磁盘 | 导入时峰值约 8 GB；完成后游戏相关约 7.3 GB（加 `--no-romfs-dir` 约 3.6 GB），工具链约 1 GB。 |
| 网络 | github.com、dot.net、pypi.org（只有 `setup_env.sh` 用到）。 |
| 游戏包（自备） | 《超级马里奥 惊奇》本体 v0（`010015100B514000`）与 1.2.0 更新 v262144（`010015100B514800`），`.nsz` 或 `.nsp` 均可。 |
| prod.keys（自备） | 从你自己的主机导出。只在本机读取，不会复制进仓库。 |
| 真机 | 测试机 FlyMe（iPhone 8 Plus / iOS 16.7.16），USB 连接并已信任这台 Mac。 |
| Apple 开发者账号 | 付费 Team `5H8VE3NN7D`：两项内核内存 entitlement（扩展虚拟地址、提高内存上限）需要付费账号，免费个人 Team 签不出来。在新 Mac 的 Xcode 里登录同一个账号即可。 |

## 1. 装工具链

```bash
tools/setup_env.sh
```

工具装在 `work/tools`，不改系统环境。可以重复执行，已装好的会跳过。在空目录里从零装一遍约 6 分半（实测）。版本固定为已校验产物所用的版本：

- Python venv：依赖见 `requirements.txt`
- hactool、nstool 1.9.2、XcodeGen 2.46.0
- ios-deploy，打了 `tools/patches/ios-deploy-streaming-upload.patch`：3.7 GB 的 RomFS 归档要分块上传，原版会整个读进内存
- .NET SDK 10.0.402，用来编 `tools/wonder_xlate`（Ryujinx 着色器前端，源码在 `vendor/`）
- SPIRV-Cross

nstool 只有 x86_64 版，Apple 芯片的 Mac 要装 Rosetta（`softwareupdate --install-rosetta`）。

## 2. 导入游戏

```bash
tools/import_game.sh "<本体>.nsz" "<1.2.0 更新>.nsz" /path/to/prod.keys
```

脚本依次完成以下步骤：

1. 解压 NSZ，拆 PFS0。
2. 把更新叠在本体上，取出 ExeFS（main/sdk/rtld/npdm）、合并后的 RomFS 和 control 分区。
3. 按 `verified-inputs.json` 核对程序文件，确认是 1.2.0 版本。
4. 做 CPU 改写（`patch_nso.py`），再跑结构核对和等价核对。改写产物和核对日志也必须和记录的哈希一致。
5. 打包 RomFS 归档 `work/build/wonder-romfs.wrom` 并核对 sha256。

全程约 11 分钟（实测，Intel Mac）。中间文件默认做完即删。加 `--keep-intermediate` 会保留它们；加 `--no-romfs-dir` 会在打包后删掉解开的 RomFS 目录（省 3.7 GB，只有做资源分析时才需要它）。

注意：

- 新版 `prod.keys` 里有 hactool 不认的条目，脚本会给 hactool 准备一份过滤后的副本，原文件不动。
- 更新包的 RomFS 是叠在本体上的补丁，本体和更新各有一张 ticket，两张都必须是 common ticket。

## 3. 着色器普查（可选，但能省几十轮）

着色器靠真机运行时采集的“普查”（`DrawCensus`，含游戏着色器二进制）离线翻译出来。这些数据源自游戏，所以不进 Git。没有它们时，App 会在每个新程序处按 `NVN_SHADER_MISSING` 停下并写出普查，要重新跑几十轮才能补齐。

在旧机器上导出，经私人渠道（U 盘、私人网盘）拷过去，再导入：

```bash
tools/local_state.sh export ~/wonder-local-state.tar.gz   # 旧机器上执行，约 200 KB
tools/local_state.sh import ~/wonder-local-state.tar.gz   # 新机器上执行；会用本机工具链重建并比对
```

## 4. 跑一轮真机

```bash
python3 -m unittest discover -s tests -q
tools/device_round.sh first-frame-round<N>
```

换了设备或账号时，用环境变量覆盖：`WONDER_DEVICE=<UDID>`、`WONDER_TEAM=<Team ID>`。包名 `two.local.smbwonder.loaderprobe` 注册在 Team `5H8VE3NN7D` 下；换成别的账号时，还要同时改 `ios-probe/project.yml` 里的 `DEVELOPMENT_TEAM` 和 `PRODUCT_BUNDLE_IDENTIFIER`。

第一轮会把 3.7 GB 的 RomFS 归档传进 App 的 Documents，之后按 sha256 标记跳过。删除 App 会连同 Documents（含存档和归档）一起删掉，脚本不会自动卸载。

诊断用环境变量（`WONDER_ENVS="A=1 B=2" tools/device_round.sh …`）：

- `WONDER_PRESENT_EVERY=n`：每 n 次 present 截一张图，并记录 `NVN_FRAME` 帧摘要
- `WONDER_DRAW_LOG=a-b`：记录第 a–b 次 present 期间每个 draw 的目标和贴图
- `WONDER_SUBMIT_SYNC=1`：每次提交后都等 GPU 完成
- `WONDER_DRAW_SYNC=1`：每个 draw 后都等 GPU 完成，并对出错的 draw 做二分定位

## 不进 Git 的内容

`.gitignore` 排除了 `work/` 整个目录，以及 `*.keys`、`*.nsp/.nsz/.xci/.nca/.nso` 等游戏与密钥文件和签名材料。唯一例外是 `work/analysis/first-frame/ledger.md`（逐轮记录），需要用 `git add -f` 加入。
