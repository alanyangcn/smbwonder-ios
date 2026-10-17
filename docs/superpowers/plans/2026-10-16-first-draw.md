# 首个 draw（B3）实施计划

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** round52 停在第 2 次提交第 272 条命令（第一个真实 DrawElements）。目标是让这条 draw 在 FlyMe 上用游戏原版着色器的翻译结果、游戏真实的顶点/常量/贴图数据执行，并把渲染目标读回成图片留证，然后推进到下一个真实停点。

**Architecture:** 设备端先做一次“draw 普查”：记录这条 draw 的全部状态，同时导出所用程序的控制段和代码。离线流程：沿用 D2R 的 Ryujinx 翻译器，复制一份 Wonder 专用版并加上 NEAR_IS_MINUS_W 深度重映射；把普查得到的事实作为翻译查询的答案，经 spirv-cross 生成 MSL 2.1，使用离散绑定。产出按控制段键（`sha256(control[0:0x880])[:16]`）索引的 MSL 与绑定反射，随 App 携带。设备端用 `newLibraryWithSource` 编译并缓存。回放层负责解析状态和资源，组装宿主 draw 描述；Metal 层负责建 PSO、上传贴图并编码 draw。无法证明正确的环节一律按名停止。

**Tech Stack:** C11 / Objective-C、Metal 2 + MSL 2.1（离散槽，无 argument buffer、无 gpuAddress）、Python 3、.NET（work/tools/dotnet）、Ryujinx.Graphics.Shader、SPIRV-Cross（work/tools/spirv-cross）、XcodeGen/Xcode。

**Spec:** `RENDER-PLAN.md`（§3–§6、实施顺序 B3/B4）与 `STATUS.md`。D2R 参考实现：`vendor/d2r-mobile/dm/gfx/README.md`、`tools/dmsh_build.py`、`tools/maxwell_refs.py`、`tools/dmsh_xlate/Program.cs`、`nvn/nvn_mtl.m`（support_buffer 布局约 2870 行）。

## Global Constraints

- 设备 FlyMe：iPhone 8 Plus / A11 / iOS 16.7.16。只用 Metal 2 / MSL 2.1（`MTLLanguageVersion2_1`），不依赖 argument buffer tier 2、`gpuAddress`、Metal 3。A11 每阶段上限：31 个 buffer 槽、96 个 texture 槽、16 个 sampler 槽。
- 游戏模式（round52 日志实测）：`nvnDeviceSetWindowOriginMode UPPER_LEFT`、`nvnDeviceSetDepthMode NEAR_IS_MINUS_W`。
- 控制段 0x880 字节、魔数 0x98761234、阶段字段 +0x714；键 = `sha256(control[0:0x880]).hexdigest()[:16]`（与 D2R `dm_ctl_sha16` 同法）。
- Maxwell 绑定约定（D2R `maxwell_refs.py`，Wonder 须实测确认）：uniform 槽 r ↔ `c[3+r]`；贴图句柄 ↔ `c[2][0x168+8r]`；c1 = 控制段 `+0x700`/`+0x704` 给出的代码区区间。
- Mac 无离线 Metal 工具链（`xcrun metal` 报 missing Metal Toolchain）：只出 MSL 源码，设备编译。
- 不跳过、不伪造：缺着色器、未知格式、绑定不全、编译失败都按名停止，日志写明键/槽/格式；不画假帧。
- 游戏数据与派生着色器只放 `work/`（不进 Git）；新运行时文件要同时加进 `tools/build_loader_probe.py` 和 `ios-probe/project.yml`。
- 每个任务结束提交一次 commit，不 push；每次真机轮次都更新 `STATUS.md` 和 ledger（`git add -f`）。

## Review Focus

1. **c[2] 句柄表偏移在 Wonder 的 SDK 版本里可能和 D2R 不同**：贴图槽会全部错位，画面错但不会报错。Task 3 用机器码扫描结果对照普查到的 BindTexture 下标，不一致就停止（测试：`test_texture_slots_match_census`）。
2. **NEAR_IS_MINUS_W 漏了重映射**：深度测试全错。Task 2 测试顶点 MSL 里必须有 `(z + w) * 0.5` 一类的变换；片元阶段不能有。
3. **buffer 槽冲突**：顶点流与 UBO/support/c1 抢同一个 `[[buffer(n)]]`。Task 3 分配器测试；超过 31 个槽就停止。
4. **设备 MSL 编译失败或入口阶段不符**：必须带键和编译器报错停止，不能静默跳过这条 draw（Task 4 测试 `stops_on_compile_failure`）。
5. **顶点/索引/UBO 跨越虚拟映射边界或落在 PROT_NONE 区**：沿用 `host_span` 的停止语义（Task 5 负例测试）。

---

## 文件结构

| 文件 | 职责 |
|---|---|
| `runtime/nvn_draw_census.c/.h`（新） | 纯 C：把回放状态 + 一条 draw 序列化成普查 JSON 与阶段二进制（写盘函数注入，可单测） |
| `runtime/nvn_queue.c`（改） | 回放状态扩展（程序、属性/流、顶点缓冲、UBO、贴图句柄、各状态对象）；draw 时普查 / 组装宿主 draw |
| `tools/wonder_xlate/Program.cs`、`wonder_xlate.csproj`（新，源自 D2R） | 翻译器：作业里的 `depth_minus_one` 打开 Ryujinx 深度重映射 |
| `tools/wonder_shader_build.py`（新） | 普查目录 → 作业（事实）→ SPIR-V → MSL 2.1 → 反射文本 + manifest |
| `runtime/shader_library.c/.h`（新） | 纯 C：键计算、反射文本解析、槽位分配、manifest 校验 |
| `runtime/metal2_draw.m/.h`（新） | Metal：MSL 编译缓存、PSO/深度模板缓存、贴图上传、采样器、draw 编码、渲染目标读回 |
| `runtime/host_graphics.h/.m`（改） | 声明 `struct wonder_host_draw` 与 `wonder_host_draw()`、读回接口 |
| `tests/…`（新） | 每个纯 C 文件一套 C 测试 + Python 包装；Mac GPU 测试扩展 `tests/test_metal2_gpu.py` |

## Tasks

### Task 1：首个 draw 普查（真机 round53）

**Files:** Create `runtime/nvn_draw_census.c/.h`, `tests/nvn_draw_census_test.c`, `tests/test_nvn_draw_census.py`；Modify `runtime/nvn_queue.c`、`tools/device_round.sh`（fetch 段多取 `Documents/DrawCensus/*`）、`tools/build_loader_probe.py`、`ios-probe/project.yml`。

**Interfaces:**
- Produces: `struct wonder_nvn_draw_state`（回放状态的可序列化子集，nvn_queue.c 维护）；`int wonder_nvn_draw_census(const struct wonder_nvn_draw_state *s, unsigned opcode, const struct wonder_nvn_cmd *draw, int (*write)(const char *name, const void *data, size_t size, void *ctx), void *ctx)`，返回 0 成功。产出文件：`draw.json`；每个阶段 `stage-<n>.control.bin`（0x880 字节）和 `stage-<n>.code.bin`（从 code GPU 地址读取控制段 `+0x704` 给出的总长，即代码区加 c1；离线用现有 `tools/match_shader_capture.py` 对照 RomFS 原版 BNSH 核实长度和起点，对不上 Task 3 就失败）。
- `draw.json` 字段：`opcode`、`primitive`、`index_type`、`count`、`base_vertex`、`instances`；`program.stages[]`（`stage`、`key`、`code_size`）；`attribs[]`（`index`、`format`、`offset`、`stream`）；`streams[]`（`stride`、`divisor`、`buffer`：{`address`,`size`}）；`uniform_buffers[]`（`stage`、`index`、`address`、`size`）；`textures[]`（`stage`、`index`、`handle`，并解析出纹理描述 {`target`,`format`,`width`,`height`,`depth`,`levels`,`layout`} 和采样器描述）；`targets`（各颜色及深度的格式/尺寸/视图）；`blend`/`color`/`channel_mask`/`depth_stencil`/`polygon`/`multisample` 原始字节的十六进制；`viewport`、`scissor`、`depth_mode`、`window_origin`。

- [ ] **Step 1：写失败测试** `tests/nvn_draw_census_test.c`：构造一个含 2 阶段程序、1 个属性、1 个流、1 个 UBO、1 个贴图句柄的状态，调用普查，断言：写出 `draw.json` 和 4 个阶段文件；`"key":"<16 位十六进制>"` 等于用同一 0x880 字节算出的 sha256 前缀；JSON 里 `"primitive":4`、`"index_type":0`、`"count":6`；未绑定程序 → 返回非 0。Python 包装同 `tests/test_ngc_service.py` 的写法（clang `-Werror`，Mac 上用 CommonCrypto 算 sha256）。
- [ ] **Step 2：跑测试，确认因缺文件失败** `python3 -m unittest tests.test_nvn_draw_census`
- [ ] **Step 3：实现** `nvn_draw_census.c`。nvn_queue.c 在现有“纯状态”分支里把 BIND_PROGRAM/ATTRIB/STREAM/VERTEX_BUFFER/UNIFORM_BUFFER/TEXTURE(S)/各状态对象写进 `struct wonder_nvn_draw_state`。在 `WONDER_NVN_DRAW_ELEMENTS` 和 `CMD_DRAW_*` 按名停止之前调用普查（每次运行只做第一次）；写到 `Documents/DrawCensus/`，写入函数在 `host_graphics.m` 里实现。停止信息追加 `census=DrawCensus vs=<key> fs=<key>`。
- [ ] **Step 4：全套测试** `python3 -m unittest discover -s tests -q` → OK。
- [ ] **Step 5：提交**；然后 `tools/device_round.sh first-frame-round53`，取回 `DrawCensus`，在 ledger 记下两个键、属性格式、贴图格式/目标、渲染目标格式。若普查暴露出新停点（例如贴图句柄落在未注册的池槽），按名停止并另起任务处理。

### Task 2：Wonder 翻译器（深度重映射）

**Files:** Create `tools/wonder_xlate/Program.cs`、`tools/wonder_xlate/wonder_xlate.csproj`（从 `vendor/d2r-mobile/dm/gfx/tools/dmsh_xlate/` 复制；文件头注明来源和改动）、`tests/test_wonder_xlate.py`。

**Interfaces:**
- Produces: 可执行文件 `work/tools/wonder_xlate/wonder_xlate <jobs.json> <outdir>`，作业字段在 D2R 基础上增加 `"depth_minus_one": true|false`；输出 `<key>.spv`、`<key>.res.json`（格式同 D2R）。

- [ ] **Step 1：写失败测试** `test_vertex_remaps_minus_one_depth`：用 `work/build/metal2-shaders/exploratory` 里现有的顶点阶段 `78cc4a70148d800cfb0a6c69.bin`（不存在就 `skipUnless`）。`depth_minus_one=true` 翻译后再过 spirv-cross（参数同 Task 3），断言 MSL 里有对 `gl_Position.z` 的 `(z + w) * 0.5` 变换；`false` 时没有。`test_fragment_unchanged`：片元阶段 `a241834ba9fed232042bcf38.bin` 两种设置产出的 SPIR-V 字节相同。
- [ ] **Step 2：跑测试，确认失败**（工具不存在）。
- [ ] **Step 3：实现**：`QueryGraphicsState()` 里深度模式参数取作业的 `depth_minus_one`。若 Ryujinx 版本另有 `QueryHostSupportsDepthClipControl()` 之类的查询，答 `false`，让翻译器插入重映射；在 Ryujinx.Graphics.Shader 的 IGpuAccessor 里找到真实名字，写进注释。用 `work/tools/dotnet` 构建到 `work/tools/wonder_xlate/`。
- [ ] **Step 4：跑测试通过。**
- [ ] **Step 5：提交。**

### Task 3：离线着色器构建（普查 → MSL 2.1 + 反射）

**Files:** Create `tools/wonder_shader_build.py`、`tests/test_wonder_shader_build.py`；复用 `vendor/d2r-mobile/dm/gfx/tools/maxwell_refs.py`（`sys.path` 导入，不复制）。

**Interfaces:**
- Consumes: Task 1 的普查目录，Task 2 的翻译器。
- Produces: `work/build/wonder-shaders/<key>.metal`、`<key>.refl`（文本格式，供 Task 4 解析）、`manifest.json`（`{"keys": {key: {"stage", "msl_sha256", "refl_sha256", "census": 普查 draw.json 的 sha256}}}`）。命令：`python3 tools/wonder_shader_build.py <census-dir>... --out work/build/wonder-shaders`。
- `.refl` 每行一条：`stage vs|ps`；`cbuf <slot r> <metal buffer>`；`support <metal buffer>`；`c1 <metal buffer> <hex>`；`tex <slot r> <metal texture> <metal sampler> <type>`；`attr <metal location> <nvn attribute> float|sint|uint`；`fout <color index> float|sint|uint`；`drv <bank> <hex offsets>`（D2R `drv_reads`；c0/c2 运行时读取，Task 5 遇到就停止）。

- [ ] **Step 1：写失败测试**（用 round53 普查目录，路径由环境变量 `WONDER_CENSUS` 给出，没有就 skip）：`test_keys_match_census`（产出键 = 普查键）；`test_texture_slots_match_census`（`maxwell_refs` 扫描代码得到的贴图槽集合 == 普查里该阶段 BindTexture 的下标集合，否则构建失败并点名）；`test_buffer_slots_disjoint`（同一阶段 cbuf/support/c1 的 metal buffer 号互不相同，并且都 < 16；顶点流保留 16..30）；`test_vertex_attr_types_from_census`（作业的 `attr` 取自普查属性格式的分量类型）；`test_msl_21_flags`（spirv-cross 命令含 `--msl-ios --msl-version 20100 --msl-decoration-binding`）。
- [ ] **Step 2：跑测试确认失败。**
- [ ] **Step 3：实现**：作业事实全部来自普查。`tex` 取槽 r 的描述 target（NVN target → `2d|2darray|cube|3d|…`）；`attr` 取属性格式的分量类型；`fout` 取颜色目标格式的分量类型；`c1` 按 `+0x700/+0x704` 截取；`depth_minus_one = (depth_mode == NEAR_IS_MINUS_W)`。翻译 → spirv-cross → 解析 `.res.json`、`--reflect` JSON 与 MSL 参数表生成 `.refl`。spirv-cross 的 buffer 号超出 0..15 时用 `--msl-...` 重映射参数压到 0..15，压不下就失败并点名。
- [ ] **Step 4：用 round53 普查跑构建与测试通过**；在 ledger 记下两个键的 `.refl` 摘要。
- [ ] **Step 5：提交。**

### Task 4：宿主着色器库（键、反射、设备编译）

**Files:** Create `runtime/shader_library.c/.h`、`tests/shader_library_test.c`、`tests/test_shader_library.py`；Modify `runtime/metal2_draw.m`（新建，本任务只写编译缓存部分）、`tools/build_loader_probe.py`（把 `work/build/wonder-shaders/` 复制进 App 的 `shaders/`，并校验 manifest 的 sha256）、`ios-probe/project.yml`。

**Interfaces:**
- Produces:
  - `void wonder_shader_key(const void *control /*0x880 bytes*/, char out[17])`
  - `struct wonder_shader_refl { int stage; int ncbuf; struct { uint8_t slot, buffer; } cbuf[16]; int support; int c1_buffer; uint16_t c1_len; uint8_t c1[4096]; int ntex; struct { uint8_t slot, texture, sampler, type; } tex[32]; int nattr; struct { uint8_t location, nvn, type; } attr[16]; uint8_t fout_mask; uint8_t drv_read; }`
  - `int wonder_shader_refl_parse(const char *text, size_t size, struct wonder_shader_refl *out, char *err, size_t errlen)`（0 = 成功）
  - `void *wonder_host_shader_function(const char key[17], int stage)`：返回 retained `id<MTLFunction>`，按键缓存。缺 MSL 时停止 `NVN_SHADER_MISSING key=… stage=…`；编译失败时停止 `NVN_SHADER_COMPILE_FAILED key=… error=<首行>`；入口阶段不符时停止。

- [ ] **Step 1：写失败测试**：`key` 与 Python `hashlib.sha256(b)[:16]` 对拍（3 组输入）；`.refl` 解析：正常样例逐字段断言、未知行类型返回错误、cbuf 超过 16 返回错误、`c1` 十六进制长度为奇数返回错误；`stops_on_compile_failure` 放到 Mac GPU 测试（`tests/test_metal2_gpu.py` 增加一项：喂一段语法错误的 MSL，断言进程按名停止）。
- [ ] **Step 2：跑测试确认失败。**
- [ ] **Step 3：实现**：`shader_library.c` 只用 CommonCrypto 和 libc；`metal2_draw.m` 编译时用 `MTLCompileOptions.languageVersion = MTLLanguageVersion2_1`，用 `fastMathEnabled = NO` 保留翻译器的精度约定，入口名取 MSL 里唯一的 `vertex`/`fragment` 函数。BindProgram 回放时解析两阶段的键，没有 MSL 就立刻按键停止（在 draw 之前，日志更早）。
- [ ] **Step 4：测试通过。**
- [ ] **Step 5：提交。**

### Task 5：回放组装宿主 draw

**Files:** Modify `runtime/nvn_queue.c`、`runtime/host_graphics.h`、`tests/nvn_device_test.c`（测试替身实现 `wonder_host_draw`，记录参数）。

**Interfaces:**
- Consumes: Task 1 的状态结构、Task 4 的 `wonder_shader_refl`。
- Produces（`host_graphics.h`）：
  - `struct wonder_host_span { void *buffer; size_t offset, size; };`
  - `struct wonder_host_draw { void *vs, *fs; const struct wonder_shader_refl *vr, *fr; struct wonder_nvn_texture_snapshot targets[9]; int color_count; int primitive, index_type, count, base_vertex, instances, base_instance; struct wonder_host_span index; int nstream; struct { struct wonder_host_span span; uint32_t stride, divisor; } stream[16]; int nattr; struct { uint32_t format, offset, stream; } attr[16]; struct wonder_host_span ubo[2][16]; struct { const struct wonder_nvn_texture *texture; const struct wonder_nvn_texture_view *view; const void *sampler; } tex[2][32]; uint8_t blend[8*8], color[4], channel_mask[4], depth_stencil[8], polygon[4], multisample[8]; float viewport[4], depth_range[2]; int scissor[4]; };`
  - `int wonder_host_draw(const struct wonder_host_draw *d);`（0 = 已编码）

- [ ] **Step 1：写失败测试**（在 `nvn_device_test.c` 现有回放用例旁边）：录制 BindProgram + 属性/流/顶点缓冲/UBO/贴图 + DrawElements，提交后断言替身收到的 `index.offset`、`stream[0].stride`、`ubo[0][r]` 与录制的地址一致，贴图描述来自纹理池注册；负例：缺 UBO 槽（反射要求 `cbuf r` 但未绑定）→ 停止 `unbound uniform buffer`；索引范围跨映射 → 停止 `crosses a virtual mapping boundary`；反射有 `drv` 行 → 停止 `driver constant segment read`；属性格式没有 Metal 对应 → 停止并给出格式号。
- [ ] **Step 2：跑测试确认失败。**
- [ ] **Step 3：实现**：替换现有 `draw execution needs translated shaders and PSOs` 的停止，改为组装并调用 `wonder_host_draw`。`render_enable==0` 时仍照旧不执行。
- [ ] **Step 4：测试通过。**
- [ ] **Step 5：提交。**

### Task 6：Metal 编码（贴图、采样器、PSO、draw、读回）

**Files:** Modify `runtime/metal2_draw.m`、`runtime/host_graphics.m`（渲染编码器沿用现有清屏/提交路径和 `replay_mutex`）、`tests/test_metal2_gpu.py`。

**Interfaces:**
- Consumes: Task 5 的 `struct wonder_host_draw`。
- Produces: `wonder_host_draw()` 的真实实现；`int wonder_host_readback_png(void *texture, int level, int slice, const char *path)`。

- [ ] **Step 1：写失败的 Mac GPU 测试** `test_draw_textured_triangle`：手写 MSL（结构与 `.refl` 一致：`cbuf 0 → buffer(0)`、`tex 0 → texture(0)/sampler(0)`、顶点流 0 → `buffer(16)`），喂一个 64×64 RGBA8 目标、一个 2×2 块线性 RGBA8 贴图（复用 `nvn_deswizzle` 的测试数据）和 NEAR_IS_MINUS_W 深度。读回后断言：中心像素等于贴图采样值，三角形外的像素等于清屏色，深度缓冲中心值 = 0.5（对应 z=0, w=1）。
- [ ] **Step 2：跑测试确认失败。**
- [ ] **Step 3：实现**：
  - 贴图：首次使用时按 `nvn_deswizzle` 转线性后上传；BC1–7 按 `nvn_bcdec` 解成 RGBA8（A11 不支持 BC）；以纹理序号 + 存储地址缓存，CPU 写入后的重新上传先按“每次提交前比对存储哈希”实现，D2R 的脏页追踪以后再做。
  - 采样器：按采样器池描述（环绕、过滤、LOD、比较、边框色）建 `MTLSamplerState`，按描述字节缓存。
  - PSO：以（vs, fs, 颜色/深度格式, blend 字节, 属性/流布局, 采样数）为键缓存；深度模板单独缓存。
  - 编码：顶点流放到 `[[buffer(16+i)]]`，UBO/support/c1 按 `.refl` 绑定；视口用 NVN viewport 与 depth range；scissor 同清屏逻辑；索引类型 u8 在 Metal 里没有对应，转成 u16 临时缓冲；图元类型没有 Metal 对应（如 quads）时按名停止。
  - support buffer：按 D2R `nvn_mtl.m` 的实测布局（alpha_test 0、is_bgra 16、viewport_inverse 144、viewport_size 160、frag_scale_count 176、render_scale float4[73] 从 192 起取 .x 并全部置 1.0）建一个常量 buffer。
- [ ] **Step 4：Mac GPU 测试通过，全套测试通过。**
- [ ] **Step 5：提交。**

### Task 7：真机验收与下一个停点

**Files:** Modify `runtime/nvn_queue.c`（第一个成功执行的 draw 所在的 submit 完成后读回渲染目标，写 `Documents/FirstDraw/target-<n>.png`，只做一次）、`tools/device_round.sh`（fetch 段多取 `FirstDraw/*`）、`STATUS.md`、ledger。

- [ ] **Step 1**：`python3 tools/wonder_shader_build.py work/analysis/first-frame-round53/Documents/DrawCensus --out work/build/wonder-shaders`，然后跑 `tools/device_round.sh first-frame-round54`。
- [ ] **Step 2**：确认日志里有 `NVN_DRAW_EXECUTED vs=<key> fs=<key> count=…` 和 GPU 完成记录，并且停点前移（下一个停点是新程序缺 MSL 也算进展：它按键点名）。打开读回 PNG 检查画面：应该是游戏自己的 UI/场景片段，不能是纯色或花屏。花屏时回到 Task 3 的 Review Focus 1、2 排查。
- [ ] **Step 3**：更新 STATUS（run id、执行的 draw 数、键、图片哈希、下一个停点）和 ledger；提交。
- [ ] **Step 4**：之后每个新程序的流程是“普查 → 构建 → 真机”。新程序很多时，把 Task 1 的普查扩展为同一次运行里记录前 N 条 draw（不执行它们，只在第一条无法执行的 draw 处停止），批量构建；这一步另写计划。
