#define _DEFAULT_SOURCE   /* MAP_ANON on glibc; Darwin has it by default */
// Host test of the strict NVN bootstrap: Wonder's actual call order (main+0x5bb8b4..0x5bb970).
#include "nvn_device.h"
#include "nvn_names.h"
#include "nvn_texture.h"
#include "nvn_commands.h"
#include "host_draw.h"
#include <assert.h>
#include <setjmp.h>
#include <stdarg.h>
#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include <CommonCrypto/CommonDigest.h>
static jmp_buf stopped; static char last_stop[512]; static int host_ready, expecting_stop;
void wonder_nvn_report(const char *format, ...) { va_list a; va_start(a, format); vfprintf(stdout, format, a); va_end(a); fputc('\n', stdout); }
_Noreturn void wonder_nvn_stop(const char *format, ...) { va_list a; va_start(a, format); vsnprintf(last_stop, sizeof last_stop, format, a); va_end(a); if (!expecting_stop) { fprintf(stderr, "unexpected stop: %s\n", last_stop); abort(); } longjmp(stopped, 1); }
static void *last_labeled_resource; static const char *last_resource_label;
// Draw census sink (Documents/DrawCensus on device): files kept in memory for assertions.
static struct { char name[64]; unsigned char *data; size_t size; } census_files[64]; static unsigned census_count;
int wonder_host_census_write(const char *name, const void *data, size_t size) {
    assert(census_count < 64); snprintf(census_files[census_count].name, 64, "%s", name);
    census_files[census_count].data = malloc(size ? size : 1); memcpy(census_files[census_count].data, data, size);
    census_files[census_count++].size = size; return 0;
}
static const unsigned char *census_file(const char *name, size_t *size) {
    for (unsigned i = 0; i < census_count; i++) if (!strcmp(census_files[i].name, name)) { *size = census_files[i].size; return census_files[i].data; }
    return NULL;
}
void wonder_host_resource_label(void *resource, const char *label) {last_labeled_resource=resource;last_resource_label=label;}
int wonder_host_driver_ready(void) { return host_ready; }
static int buffers, host_textures; static const void *native_layer = (const void *)0x281b8a400;
void *wonder_host_display_texture(unsigned w, unsigned h, int f) { assert(w == 1280 && h == 720 && f == 0x25); return (void *)(uintptr_t)(0xc0000 + ++host_textures); }
int wonder_host_native_window_valid(const void *n) { return n == native_layer; }
#include <sys/mman.h>
#include "nvn_deswizzle.h"
void *wonder_host_reserve(size_t size) { void *p = mmap(NULL, size, PROT_NONE, MAP_PRIVATE | MAP_ANON, -1, 0); return p == MAP_FAILED ? NULL : p; }
void wonder_nvnTextureBuilderSetDevice(void *, const void *); void wonder_nvnTextureBuilderSetDefaults(void *);
uint32_t wonder_nvnTextureInitialize(void *, const void *); int wonder_nvnWindowAcquireTexture(void *, void *, int *);
void wonder_nvnWindowBuilderSetNativeWindow(void *, const void *); void *wonder_nvnBufferMap(const void *);
uint32_t wonder_nvnBufferInitialize(void *, const void *); void wonder_nvnBufferBuilderSetStorage(void *, const void *, ptrdiff_t, size_t);
void wonder_nvnCommandBufferSetMemoryCallback(void *, void *);
int wonder_nvnTextureGetWidth(const void *); void wonder_nvnBlendStateSetBlendFunc(void *, int, int, int, int); void wonder_nvnDepthStencilStateSetDepthFunc(void *, int); void wonder_nvnWindowBuilderSetTextures(void *, int, const void *const *);
// Fake pool buffers remember their memory so GPU-ordered writes can land there when "executed" (flush / finish).
static void *buffer_memory[64]; static struct { unsigned id; size_t offset, size; unsigned char data[64]; } pending_writes[64]; static unsigned pending_write_count;
void *wonder_host_pool_buffer(void *memory, size_t size, size_t *offset) { assert(memory && size && offset); *offset = 0; buffers++; if (buffers < 64) buffer_memory[buffers] = memory; return (void *)(uintptr_t)(0xb0000 + buffers); }
static void apply_pending_writes(void) {
    for (unsigned i = 0; i < pending_write_count; i++)
        if (pending_writes[i].id < 64 && buffer_memory[pending_writes[i].id]) memcpy((unsigned char *)buffer_memory[pending_writes[i].id] + pending_writes[i].offset, pending_writes[i].data, pending_writes[i].size);
    pending_write_count = 0;
}
#define EXPECTED_IMPLEMENTED 267
static unsigned released_resources, captured_programs;
void wonder_host_release(void *resource) { if (resource) released_resources++; }
static uint64_t forgotten_serial; static unsigned forgotten_textures;
void wonder_host_texture_forget(uint64_t serial) { forgotten_serial = serial; forgotten_textures++; }
void wonder_host_unreserve(void *memory, size_t size) { (void)memory; (void)size; }
void wonder_nvn_capture_shaders(const void *program, const struct wonder_nvn_program_shaders *shaders) { (void)program; (void)shaders; captured_programs++; }
// Fake translated-shader library and draw backend: the vertex key gets fake_refl[0], every other key fake_refl[1].
static struct wonder_shader_refl fake_refl[2]; static char fake_vs_key[17]; static int shaders_available; static unsigned host_draws; static struct wonder_host_draw last_draw;
int wonder_host_shader_available(const char key[17]) { (void)key; return shaders_available; }
void *wonder_host_shader_function(const char key[17], int stage) { (void)key; return (void *)(uintptr_t)(0xf000 + stage); }
const struct wonder_shader_refl *wonder_host_shader_refl(const char key[17]) { return !strcmp(key, fake_vs_key) ? &fake_refl[0] : &fake_refl[1]; }
int wonder_host_draw(const struct wonder_host_draw *d) { last_draw = *d; host_draws++; return 0; }
static unsigned host_depth_clear_rects;
int wonder_host_clear_depth_stencil_rect(void *texture, int level, int slice, int clear_depth, float depth, int clear_stencil, uint32_t stencil, uint32_t stencil_mask, const int rect[4]) {
    (void)texture; (void)level; (void)slice; (void)clear_depth; (void)depth; (void)clear_stencil; (void)stencil; (void)stencil_mask; (void)rect; host_depth_clear_rects++; return 0; }
static unsigned host_clear_rects; static int last_clear_rect[4]; static unsigned last_clear_mask;
int wonder_host_clear_rect(void *texture, int level, int slice, const float rgba[4], unsigned mask, const int rect[4]) {
    (void)texture; (void)level; (void)slice; (void)rgba; memcpy(last_clear_rect, rect, 16); last_clear_mask = mask; host_clear_rects++; return 0; }
static const char *fake_gpu_error;   // set to simulate a failed replay command buffer
void wonder_host_note_submit(unsigned submit) { (void)submit; }
void wonder_host_run_on_replay_stack(void (*fn)(void *), void *arg) { fn(arg); }
int wonder_host_gpu_error(char *out, size_t size) { if (!fake_gpu_error) return 0; snprintf(out, size, "%s", fake_gpu_error); return 1; }
static unsigned first_draw_captures; static struct wonder_nvn_texture_snapshot first_draw_target;
void wonder_host_capture_first_draw(const struct wonder_nvn_texture_snapshot *target) { first_draw_target = *target; first_draw_captures++; }
// Fake replay backend: records operations; CPU callbacks run when pending work is "committed".
static unsigned host_clears, host_depth_clears, host_fills, host_writes, host_presents, host_targets;
static float host_last_clear[4]; static void *pending_fn[64]; static void *pending_arg[64]; static unsigned pending_count;
void *wonder_host_render_target(const struct wonder_nvn_texture *texture, uint32_t format) {
    (void)format; host_targets++; return texture->host ? texture->host : (void *)(uintptr_t)(0xd000 + texture->serial);
}
int wonder_host_clear_color(void *texture, int level, int slice, const float rgba[4]) { (void)texture; (void)level; (void)slice; memcpy(host_last_clear, rgba, 16); host_clears++; return 0; }
int wonder_host_clear_depth_stencil(void *texture, int level, int slice, int cd, float depth, int cs, uint32_t stencil) {
    (void)texture; (void)level; (void)slice; (void)cd; (void)depth; (void)cs; (void)stencil; host_depth_clears++; return 0;
}
int wonder_host_fill_buffer(void *buffer, size_t offset, size_t size, uint8_t value) { (void)buffer; (void)offset; (void)size; (void)value; host_fills++; return 0; }
int wonder_host_write_buffer(void *buffer, size_t offset, const void *data, size_t size) {
    host_writes++;
    if (size <= 64 && pending_write_count < 64) { pending_writes[pending_write_count].id = (unsigned)((uintptr_t)buffer - 0xb0000); pending_writes[pending_write_count].offset = offset;
                                                  pending_writes[pending_write_count].size = size; memcpy(pending_writes[pending_write_count++].data, data, size); }
    return 0;
}
void wonder_host_after_gpu(void (*fn)(void *), void *arg) { assert(pending_count < 64); pending_fn[pending_count] = (void *)fn; pending_arg[pending_count++] = arg; }
void wonder_host_flush(void) { apply_pending_writes(); for (unsigned i = 0; i < pending_count; i++) ((void (*)(void *))pending_fn[i])(pending_arg[i]); pending_count = 0; }
void wonder_host_finish(void) { wonder_host_flush(); }
void wonder_host_fence(void (*fn)(void *), void *arg) { wonder_host_after_gpu(fn, arg); wonder_host_flush(); }
int wonder_host_present(void *texture, const int crop[4], int interval) { (void)texture; (void)crop; (void)interval; host_presents++; return 0; }
void *wonder_nvn_unimplemented_thunk(unsigned index) { assert(index < WONDER_NVN_NAME_COUNT); return (void *)(0x100000 + 8 * (uintptr_t)index); }
#define EXPECT_STOP(expr, text) do { last_stop[0] = 0; expecting_stop = 1; if (!setjmp(stopped)) { expr; assert(!"expected stop: " text); } expecting_stop = 0; assert(strstr(last_stop, text)); } while (0)
static unsigned char device[WONDER_NVN_DEVICE_SIZE], builder[WONDER_NVN_DEVICE_BUILDER_SIZE];
static void *slots[WONDER_NVN_NAME_COUNT];
static void loader_pass(const void *dev) {   // main+0x1080ff0: one GetProcAddress per name, stored per slot
    void *(*gpa)(const void *, const char *) = wonder_nvn_resolve("nvnDeviceGetProcAddress", "nvnBootstrapLoader");
    for (int i = 0; i < WONDER_NVN_NAME_COUNT; i++) { slots[i] = gpa(dev, wonder_nvn_names[i].name); assert(slots[i]); }
}
static void *slot(const char *name) { int i = wonder_nvn_name_index(name); assert(i >= 0); return slots[i]; }
static unsigned recorded_draws;
static void check_draw(unsigned opcode, const void *payload, size_t size, void *context) {
    assert(opcode == WONDER_NVN_DRAW_ELEMENTS && size == sizeof(struct wonder_nvn_draw_elements));
    const struct wonder_nvn_draw_elements *draw = payload;
    assert(draw->primitive == 4 && draw->index_type == 0 && draw->count == 6 && draw->index_address == (uintptr_t)context);
    recorded_draws++;
}
// Guest-style command-memory callback (main+0x1dd6344 equivalent): hands out the next 4 KiB of cb_pool.
static const void *rec_pool; static ptrdiff_t rec_next; static unsigned rec_callbacks;
static void rec_memory_callback(void *cmdbuf, int event, size_t min_size, void *data) {
    assert(event == 0 && min_size >= 0x200 && data == (void *)0x5eed);
    rec_callbacks++;
    ((void (*)(void *, const void *, ptrdiff_t, size_t))wonder_nvn_resolve("nvnCommandBufferAddCommandMemory", "test"))(cmdbuf, rec_pool, rec_next, 0x1000);
    rec_next += 0x1000;
}
struct rec_trace { unsigned count, opcodes[256]; float clear[4]; uint64_t uniform_word; unsigned targets; uint32_t program_mask; };
static void trace_commands(unsigned opcode, const void *payload, size_t size, void *context) {
    struct rec_trace *t = context; (void)size;
    if (t->count < 256) t->opcodes[t->count] = opcode;
    t->count++;
    if (opcode == WONDER_NVN_DRAW_ELEMENTS) return;
    const struct wonder_nvn_cmd *c = payload;
    if (opcode == WONDER_NVN_CMD_CLEAR_COLOR) memcpy(t->clear, wonder_nvn_cmd_blob(c), 16);
    if (opcode == WONDER_NVN_CMD_UPDATE_UNIFORM_BUFFER) memcpy(&t->uniform_word, wonder_nvn_cmd_blob(c), 8);
    if (opcode == WONDER_NVN_CMD_SET_RENDER_TARGETS) t->targets = (unsigned)c->a[0];
    if (opcode == WONDER_NVN_CMD_BIND_PROGRAM) t->program_mask = ((const struct wonder_nvn_program_shaders *)wonder_nvn_cmd_blob(c))->mask;
    if (opcode == WONDER_NVN_CMD_CALL_COMMANDS) assert(c->a[0] == 1);
}
int main(void) {
    // Cube-map arrays: NVN depth counts layer-faces (6 per cube). Round70: the game sized a 0x109000 pool for a
    // 128x128x12 (two cubes) RG11B10F array with 8 levels; one 128x128 cube with 8 levels is 0x84000.
    {
        nvn_texdesc cube = {.target = 8, .format = 0x3f, .width = 128, .height = 128, .depth = 6, .levels = 8}, array = cube;
        array.target = 9; array.depth = 12;
        nvn_texlayout one, two; nvn_tex_layout(&cube, &one); nvn_tex_layout(&array, &two);
        assert(one.size == 0x84000 && one.layers == 6);
        assert(two.layers == 12 && two.size == 2 * one.size && two.size <= 0x109000);
    }
    assert(WONDER_NVN_API_MAJOR == 55 && WONDER_NVN_API_MINOR_MIN == 15 && WONDER_NVN_NAME_COUNT == 534);
    // Bootstrap gives exactly the two names Wonder asks for; unknown names are NULL.
    assert(wonder_nvn_resolve("nvnDeviceInitialize", "nvnBootstrapLoader") == (void *)wonder_nvnDeviceInitialize);
    assert(wonder_nvn_resolve("nvnDeviceGetProcAddress", "nvnBootstrapLoader") == (void *)wonder_nvnDeviceGetProcAddress);
    assert(!wonder_nvn_resolve("nvnBootstrapLoader", "test") && !wonder_nvn_resolve("nvnNotAFunction", "test") && !wonder_nvn_resolve(NULL, "test"));
    loader_pass(NULL);
    int thunks = 0;
    for (int i = 0; i < WONDER_NVN_NAME_COUNT; i++) thunks += (uintptr_t)slots[i] == 0x100000 + 8 * (uintptr_t)i;
    assert(wonder_nvn_implemented_count() == EXPECTED_IMPLEMENTED && thunks == WONDER_NVN_NAME_COUNT - EXPECTED_IMPLEMENTED);
    assert(slot("nvnQueueBuilderSetQueuePriority") == wonder_nvn_unimplemented_thunk((unsigned)wonder_nvn_name_index("nvnQueueBuilderSetQueuePriority")));

    // Objects are validated, never assumed.
    EXPECT_STOP(wonder_nvnDeviceBuilderSetFlags(builder, 0), "NVN_INVALID_OBJECT");
    EXPECT_STOP(wonder_nvnDeviceGetInteger(device, 0, &(int){0}), "NVN_INVALID_OBJECT");
    EXPECT_STOP(wonder_nvnDeviceGetProcAddress(device, "nvnDeviceGetInteger"), "NVN_INVALID_OBJECT");
    ((void (*)(void *))slot("nvnDeviceBuilderSetDefaults"))(builder);
    EXPECT_STOP(wonder_nvnDeviceBuilderSetFlags(builder, 1), "NVN_UNSUPPORTED");
    ((void (*)(void *, int))slot("nvnDeviceBuilderSetFlags"))(builder, 0);
    EXPECT_STOP(wonder_nvnDeviceInitialize(device, builder), "NVN_DEVICE_HOST_UNAVAILABLE");
    host_ready = 1;
    assert(((uint32_t (*)(void *, const void *))slot("nvnDeviceInitialize"))(device, builder) == 1);
    loader_pass(device);
    void (*get)(const void *, int, int *) = slot("nvnDeviceGetInteger");
    int major = 0, minor = 0; get(device, 0, &major); get(device, 1, &minor);
    assert(major == 0x37 && minor >= 0xf);          // main+0x5bb960 cmp w8,#0x37 / 0x5bb96c cmp w8,#0xf
    int v; get(device, 0x55, &v); assert(v == 262144); get(device, 0x52, &v); assert(v == 65536); get(device, 0x5c, &v); assert(v == 16384);
    EXPECT_STOP(get(device, 0x60, &v), "NVN_UNIMPLEMENTED_DEVICE_INFO");
    EXPECT_STOP(get(device, -1, &v), "NVN_UNIMPLEMENTED_DEVICE_INFO");
    // main+0x984e90 / 0x984ea4: Metal-style conventions requested by Wonder.
    EXPECT_STOP(wonder_nvnDeviceSetWindowOriginMode(device, 2), "NVN_INVALID_ARGUMENT");
    ((void (*)(void *, int))slot("nvnDeviceSetWindowOriginMode"))(device, 1);
    ((void (*)(void *, int))slot("nvnDeviceSetDepthMode"))(device, 1);
    // main+0x5bba14..0x5bbb78: queue builder reuses the device-builder slot, then SetDevice.
    void *qb = builder;
    EXPECT_STOP(wonder_nvnQueueBuilderSetFlags(qb, 1), "NVN_INVALID_OBJECT");   // still a device builder
    ((void (*)(void *))slot("nvnQueueBuilderSetDefaults"))(qb);
    ((void (*)(void *, int))slot("nvnQueueBuilderSetFlags"))(qb, 1);
    EXPECT_STOP(wonder_nvnQueueBuilderSetDevice(qb, builder + 0), "NVN_INVALID_OBJECT");
    ((void (*)(void *, const void *))slot("nvnQueueBuilderSetDevice"))(qb, device);
    get(device, 0x55, &v); ((void (*)(void *, size_t))slot("nvnQueueBuilderSetComputeMemorySize"))(qb, (size_t)v);
    get(device, 0x52, &v); ((void (*)(void *, size_t))slot("nvnQueueBuilderSetCommandMemorySize"))(qb, (size_t)v);
    get(device, 0x5c, &v); ((void (*)(void *, size_t))slot("nvnQueueBuilderSetControlMemorySize"))(qb, (size_t)v);
    EXPECT_STOP(wonder_nvnQueueBuilderSetCommandMemorySize(qb, 4096), "NVN_INVALID_ARGUMENT");      // below minimum
    EXPECT_STOP(wonder_nvnQueueBuilderSetControlMemorySize(qb, 4096 + 256), "NVN_INVALID_ARGUMENT"); // not granular
    size_t need = ((size_t (*)(const void *))slot("nvnQueueBuilderGetQueueMemorySize"))(qb);
    assert(need == 65536 + 262144 + 16384);
    static _Alignas(4096) unsigned char queue_memory[65536 + 262144 + 16384 + 4096];
    static _Alignas(16) unsigned char queue[WONDER_NVN_QUEUE_SIZE];
    EXPECT_STOP(wonder_nvnQueueInitialize(queue, qb), "NVN_UNSUPPORTED");                     // no memory yet
    EXPECT_STOP(wonder_nvnQueueBuilderSetQueueMemory(qb, queue_memory + 16, need), "NVN_INVALID_ARGUMENT");
    EXPECT_STOP(wonder_nvnQueueBuilderSetQueueMemory(qb, queue_memory, need - 4096), "NVN_INVALID_ARGUMENT");
    ((void (*)(void *, void *, size_t))slot("nvnQueueBuilderSetQueueMemory"))(qb, queue_memory, need);
    assert(((uint32_t (*)(void *, const void *))slot("nvnQueueInitialize"))(queue, qb) == 1);
    // main+0x7338c8..0x733a48: descriptor memory pool, texture/sampler pools, one registered sampler.
    int tex_desc, smp_desc, tex_reserved, smp_reserved;
    get(device, 0xf, &tex_desc); get(device, 0x10, &smp_desc); get(device, 0x11, &tex_reserved); get(device, 0x12, &smp_reserved);
    int tex_count = 2048, smp_count = 512;                        // game-chosen counts ([x19+0xf0]/[x19+0xf4])
    size_t pool_size = ((size_t)tex_desc * tex_count + (size_t)smp_desc * smp_count + 0xfff) & ~(size_t)0xfff;
    static _Alignas(4096) unsigned char descriptor_memory[0x20000];
    assert(pool_size <= sizeof descriptor_memory);
    struct { unsigned char builder[0x40]; int locals[2]; } frame;    // builder must not touch the locals after it
    frame.locals[0] = 0x1234; frame.locals[1] = 0x5678;
    ((void (*)(void *, const void *))slot("nvnMemoryPoolBuilderSetDevice"))(frame.builder, device); // legal before Defaults
    ((void (*)(void *))slot("nvnMemoryPoolBuilderSetDefaults"))(frame.builder);
    ((void (*)(void *, const void *))slot("nvnMemoryPoolBuilderSetDevice"))(frame.builder, device);
    EXPECT_STOP(wonder_nvnMemoryPoolBuilderSetFlags(frame.builder, 0x22 | 0x200), "NVN_UNSUPPORTED");     // virtual pool
    EXPECT_STOP(wonder_nvnMemoryPoolBuilderSetFlags(frame.builder, 0x6 | 0x20), "NVN_UNSUPPORTED");       // two CPU modes
    ((void (*)(void *, int))slot("nvnMemoryPoolBuilderSetFlags"))(frame.builder, 0x22);
    EXPECT_STOP(wonder_nvnMemoryPoolBuilderSetStorage(frame.builder, descriptor_memory + 0x800, pool_size), "NVN_INVALID_ARGUMENT");
    ((void (*)(void *, void *, size_t))slot("nvnMemoryPoolBuilderSetStorage"))(frame.builder, descriptor_memory, pool_size);
    static _Alignas(16) unsigned char memory_pool[WONDER_NVN_MEMORY_POOL_SIZE];
    assert(((uint32_t (*)(void *, const void *))slot("nvnMemoryPoolInitialize"))(memory_pool, frame.builder) == 1 && buffers == 1);
    assert(slot("nvnMemoryPoolGetBufferAddress") != wonder_nvn_unimplemented_thunk((unsigned)wonder_nvn_name_index("nvnMemoryPoolGetBufferAddress")));
    assert(((uint64_t (*)(const void *))slot("nvnMemoryPoolGetBufferAddress"))(memory_pool) == (uintptr_t)descriptor_memory);
    assert(((size_t (*)(const void *))slot("nvnMemoryPoolGetSize"))(memory_pool) == pool_size);
    const char *label_names[] = {"nvnDeviceSetDebugLabel", "nvnQueueSetDebugLabel", "nvnWindowSetDebugLabel", "nvnSyncSetDebugLabel", "nvnProgramSetDebugLabel", "nvnTexturePoolSetDebugLabel", "nvnSamplerPoolSetDebugLabel", "nvnTextureSetDebugLabel", "nvnSamplerSetDebugLabel", "nvnCommandBufferSetDebugLabel"};
    for (unsigned i=0;i<sizeof label_names/sizeof *label_names;i++) {
        assert(slot(label_names[i]) != wonder_nvn_unimplemented_thunk((unsigned)wonder_nvn_name_index(label_names[i])));
        EXPECT_STOP(((void (*)(void *, const char *))slot(label_names[i]))(NULL,"invalid"), "NVN_INVALID_OBJECT");
    }
    _Alignas(16) unsigned char program_memory[0x90]; memset(program_memory, 0xa5, sizeof program_memory);
    assert(slot("nvnProgramInitialize") != wonder_nvn_unimplemented_thunk((unsigned)wonder_nvn_name_index("nvnProgramInitialize")));
    assert(((uint32_t (*)(void *, const void *))slot("nvnProgramInitialize"))(program_memory, device) == 1);
    ((void (*)(void *, const char *))slot("nvnProgramSetDebugLabel"))(program_memory,"Startup shaders");
    ((void (*)(void *, const char *))slot("nvnDeviceSetDebugLabel"))(device,"Wonder Metal2");
    const void *program_device; memcpy(&program_device, program_memory + 8, sizeof program_device); assert(program_device == device);
    for (unsigned i = 0x80; i < sizeof program_memory; i++) assert(program_memory[i] == 0xa5);
    assert(slot("nvnMemoryPoolSetDebugLabel") != wonder_nvn_unimplemented_thunk((unsigned)wonder_nvn_name_index("nvnMemoryPoolSetDebugLabel")));
    ((void (*)(void *, const char *))slot("nvnMemoryPoolSetDebugLabel"))(memory_pool, "Test shader pool");
    assert(slot("nvnBufferSetDebugLabel") != wonder_nvn_unimplemented_thunk((unsigned)wonder_nvn_name_index("nvnBufferSetDebugLabel")));
    assert(last_labeled_resource == (void *)(uintptr_t)0xb0001 && !strcmp(last_resource_label, "Test shader pool"));
    struct wonder_nvn_pool_view resolved_address;
    wonder_nvn_address_span((uintptr_t)descriptor_memory + 32, 64, &resolved_address);
    assert(resolved_address.memory == descriptor_memory + 32 && resolved_address.size == 64);
    EXPECT_STOP(wonder_nvn_address_span(0x1234, 16, &resolved_address), "NVN_INVALID_ADDRESS");
    EXPECT_STOP(wonder_nvn_address_span((uintptr_t)descriptor_memory + pool_size - 16, 32, &resolved_address), "NVN_INVALID_ARGUMENT");

    static _Alignas(16) unsigned char texture_pool[WONDER_NVN_DESCRIPTOR_POOL_SIZE], sampler_pool[WONDER_NVN_DESCRIPTOR_POOL_SIZE];
    uint32_t (*pool_init)(void *, const void *, ptrdiff_t, int) = slot("nvnSamplerPoolInitialize");
    EXPECT_STOP(pool_init(sampler_pool, memory_pool, 0, smp_reserved - 1), "NVN_INVALID_ARGUMENT");       // below reserved
    EXPECT_STOP(pool_init(sampler_pool, memory_pool, 16, smp_count), "NVN_INVALID_ARGUMENT");             // misaligned
    EXPECT_STOP(pool_init(sampler_pool, memory_pool, 0, 1 << 20), "NVN_INVALID_ARGUMENT");                // beyond pool
    EXPECT_STOP(pool_init(sampler_pool, device, 0, smp_count), "NVN_INVALID_OBJECT");
    assert(pool_init(sampler_pool, memory_pool, 0, smp_count) == 1);
    assert(((uint32_t (*)(void *, const void *, ptrdiff_t, int))slot("nvnTexturePoolInitialize"))(texture_pool, memory_pool, (ptrdiff_t)smp_desc * smp_count, tex_count) == 1);
    ((void (*)(void *, const void *))slot("nvnSamplerBuilderSetDevice"))(frame.builder, device); // main+0x9afdc before Defaults
    ((void (*)(void *))slot("nvnSamplerBuilderSetDefaults"))(frame.builder);
    ((void (*)(void *, int, int))slot("nvnSamplerBuilderSetMinMagFilter"))(frame.builder, 3, 1);
    EXPECT_STOP(wonder_nvnSamplerBuilderSetMinMagFilter(frame.builder, 6, 1), "NVN_INVALID_ARGUMENT");
    static _Alignas(16) unsigned char sampler[WONDER_NVN_SAMPLER_SIZE];
    unsigned char no_device_builder[WONDER_NVN_BUILDER_SLOT] = {0};
    ((void (*)(void *))slot("nvnSamplerBuilderSetDefaults"))(no_device_builder);
    EXPECT_STOP(wonder_nvnSamplerInitialize(sampler, no_device_builder), "NVN_INVALID_ARGUMENT");
    assert(wonder_nvnSamplerInitialize(sampler, frame.builder) == 1); // Defaults preserved device
    ((void (*)(void *, const void *))slot("nvnSamplerBuilderSetDevice"))(frame.builder, device);
    assert(((uint32_t (*)(void *, const void *))slot("nvnSamplerInitialize"))(sampler, frame.builder) == 1);
    assert(frame.locals[0] == 0x1234 && frame.locals[1] == 0x5678);
    EXPECT_STOP(wonder_nvnSamplerPoolRegisterSampler(sampler_pool, smp_count, sampler), "NVN_INVALID_ARGUMENT");
    EXPECT_STOP(wonder_nvnSamplerPoolRegisterSampler(texture_pool, 0, sampler), "NVN_INVALID_OBJECT");
    assert(!wonder_nvn_sampler_lookup(sampler_pool, 0));
    ((void (*)(const void *, int, const void *))slot("nvnSamplerPoolRegisterSampler"))(sampler_pool, 0, sampler);
    const struct wonder_nvn_sampler_state *st = wonder_nvn_sampler_lookup(sampler_pool, 0);
    assert(st && st->min == 3 && st->mag == 1 && st->wrap_s == 1 && st->compare_func == 2 && st->lod_max == 1000.f);
    assert(slot("nvnSamplerBuilderSetWrapMode") != wonder_nvn_unimplemented_thunk((unsigned)wonder_nvn_name_index("nvnSamplerBuilderSetWrapMode")));
    ((void (*)(void *, int, int, int))slot("nvnSamplerBuilderSetWrapMode"))(frame.builder, 7, 5, 1);
    ((void (*)(void *, float, float))slot("nvnSamplerBuilderSetLodClamp"))(frame.builder, 0.f, 12.f);
    ((void (*)(void *, float))slot("nvnSamplerBuilderSetLodBias"))(frame.builder, -0.5f);
    ((void (*)(void *, int, int))slot("nvnSamplerBuilderSetCompare"))(frame.builder, 1, 4);
    float border[4] = {0.25f, 0.5f, 0.75f, 1.f};
    ((void (*)(void *, const float *))slot("nvnSamplerBuilderSetBorderColor"))(frame.builder, border);
    ((void (*)(void *, float))slot("nvnSamplerBuilderSetMaxAnisotropy"))(frame.builder, 4.f);
    ((void (*)(void *, int))slot("nvnSamplerBuilderSetReductionFilter"))(frame.builder, 2);
    assert(wonder_nvnSamplerInitialize(sampler, frame.builder) == 1);
    wonder_nvnSamplerPoolRegisterSampler(sampler_pool, 1, sampler);
    st = wonder_nvn_sampler_lookup(sampler_pool, 1);
    assert(st->wrap_s == 7 && st->wrap_t == 5 && st->lod_min == 0.f && st->lod_max == 12.f && st->lod_bias == -0.5f);
    assert(st->compare_mode == 1 && st->compare_func == 4 && st->border[2] == 0.75f && st->anisotropy == 4.f);
    EXPECT_STOP(((void (*)(void *, int, int, int))slot("nvnSamplerBuilderSetWrapMode"))(frame.builder, 8, 1, 1), "NVN_INVALID_ARGUMENT");
    // main+0x9849bc: 3 display textures (builders at a 0x80 stride, SetDevice BEFORE SetDefaults on stack garbage).
    enum { N = 3 };
    static unsigned char tb[N][0x80]; memset(tb, 0xab, sizeof tb);
    size_t total = 0;
    for (int i = 0; i < N; i++) {
        ((void (*)(void *, const void *))slot("nvnTextureBuilderSetDevice"))(tb[i], device);
        ((void (*)(void *))slot("nvnTextureBuilderSetDefaults"))(tb[i]);
        ((void (*)(void *, int, int))slot("nvnTextureBuilderSetSize2D"))(tb[i], 1280, 720);
        ((void (*)(void *, int))slot("nvnTextureBuilderSetTarget"))(tb[i], 1);
        ((void (*)(void *, int))slot("nvnTextureBuilderSetFormat"))(tb[i], 0x25);
        ((void (*)(void *, int))slot("nvnTextureBuilderSetFlags"))(tb[i], 1);
        size_t size = ((size_t (*)(const void *))slot("nvnTextureBuilderGetStorageSize"))(tb[i]);
        size_t align = ((size_t (*)(const void *))slot("nvnTextureBuilderGetStorageAlignment"))(tb[i]);
        assert(size == 0x3c0000 && align == 512);   // 80 GOBs wide x ceil(90/16) blocks x 16 GOBs x 512 B
        total += (size + align - 1) / align * align;
    }
    // Device survives SetDefaults only when SetDevice ran first; a fresh builder has none.
    static unsigned char fresh[0x80]; memset(fresh, 0, sizeof fresh); wonder_nvnTextureBuilderSetDefaults(fresh);
    static unsigned char sc_pool_memory_dummy[1]; (void)sc_pool_memory_dummy;
    // Display pool: CPU_NO_ACCESS | GPU_CACHED (0x21), storage from the game's 4 KiB allocator.
    size_t sc_size = (total + 0xfff) & ~(size_t)0xfff;
    static _Alignas(4096) unsigned char sc_memory[3 * 0x3c0000];
    assert(sc_size <= sizeof sc_memory);
    unsigned char pb[0x40];
    wonder_nvnMemoryPoolBuilderSetDefaults(pb); wonder_nvnMemoryPoolBuilderSetDevice(pb, device);
    wonder_nvnMemoryPoolBuilderSetFlags(pb, 0x21); wonder_nvnMemoryPoolBuilderSetStorage(pb, sc_memory, sc_size);
    static _Alignas(16) unsigned char sc_pool[WONDER_NVN_MEMORY_POOL_SIZE];
    assert(wonder_nvnMemoryPoolInitialize(sc_pool, pb) == 1);
    static _Alignas(16) unsigned char textures[N][0x80]; const void *list[N];
    size_t offset = 0;
    EXPECT_STOP(wonder_nvnTextureInitialize(textures[0], fresh), "NVN_INVALID_ARGUMENT");   // no device, no storage
    for (int i = 0; i < N; i++) {
        ((void (*)(void *, const void *, ptrdiff_t))slot("nvnTextureBuilderSetStorage"))(tb[i], sc_pool, (ptrdiff_t)offset);
        offset += 0x3c0000;
        assert(((uint32_t (*)(void *, const void *))slot("nvnTextureInitialize"))(textures[i], tb[i]) == 1);
        list[i] = textures[i];
    }
    assert(host_textures == N);
    static _Alignas(16) unsigned char sync[0x40];
    assert(((uint32_t (*)(void *, const void *))slot("nvnSyncInitialize"))(sync, device) == 1);
    unsigned char wb[0x40];
    ((void (*)(void *))slot("nvnWindowBuilderSetDefaults"))(wb);
    ((void (*)(void *, const void *))slot("nvnWindowBuilderSetDevice"))(wb, device);
    EXPECT_STOP(wonder_nvnWindowBuilderSetNativeWindow(wb, (const void *)0x1234), "NVN_INVALID_ARGUMENT");
    ((void (*)(void *, const void *))slot("nvnWindowBuilderSetNativeWindow"))(wb, native_layer);
    const void *bad[2] = {textures[0], sampler};
    EXPECT_STOP(wonder_nvnWindowBuilderSetTextures(wb, 2, bad), "NVN_INVALID_OBJECT");
    ((void (*)(void *, int, const void *const *))slot("nvnWindowBuilderSetTextures"))(wb, N, list);
    ((void (*)(void *, int))slot("nvnWindowBuilderSetNumActiveTextures"))(wb, N);
    ((void (*)(void *, int))slot("nvnWindowBuilderSetPresentInterval"))(wb, 1);
    static _Alignas(16) unsigned char window[0x180];
    assert(((uint32_t (*)(void *, const void *))slot("nvnWindowInitialize"))(window, wb) == 1);
    int (*acquire)(void *, void *, int *) = slot("nvnWindowAcquireTexture");
    int index = -1;
    for (int i = 0; i < N; i++) { assert(acquire(window, sync, &index) == 0 && index == i); }
    EXPECT_STOP(wonder_nvnWindowAcquireTexture(window, sync, &index), "NVN_INVALID_STATE");   // all held until presented
    // main+0x5bbcf4..0x5bbffc: command buffer object, CPU_UNCACHED|GPU_UNCACHED pool, 32-byte mapped buffer, sync.
    static _Alignas(16) unsigned char cmdbuf_storage[0xb0];
    unsigned char *cmdbuf = cmdbuf_storage;
    memset(cmdbuf_storage, 0xa5, sizeof cmdbuf_storage);
    assert(((uint32_t (*)(void *, const void *))slot("nvnCommandBufferInitialize"))(cmdbuf, device) == 1);
    for (unsigned i = 0; i < 0xa0; i++)
        if (!(i >= 0x60 && i < 0x68) && i != 0x14 && i != 0x15) assert(cmdbuf[i] == 0);
    for (unsigned i = 0xa0; i < sizeof cmdbuf_storage; i++) assert(cmdbuf_storage[i] == 0xa5);
    wonder_nvnCommandBufferSetMemoryCallback(cmdbuf, NULL);
    assert(((void *(*)(const void *))slot("nvnCommandBufferGetMemoryCallback"))(cmdbuf) == NULL);
    ((void (*)(void *, void *))slot("nvnCommandBufferSetMemoryCallback"))(cmdbuf, (void *)0x1dd6344);
    ((void (*)(void *, void *))slot("nvnCommandBufferSetMemoryCallbackData"))(cmdbuf, cmdbuf);
    // SDK 55 command objects are read/written inline by main+0x5c2b8: flag 0x10 must not alias callback.
    const void *raw_pointer;
    memcpy(&raw_pointer, cmdbuf + 0x60, sizeof raw_pointer); assert(raw_pointer == device);
    cmdbuf[0x10] = 1;
    memcpy(&raw_pointer, cmdbuf + 0x68, sizeof raw_pointer); assert(raw_pointer == (void *)0x1dd6344);
    memcpy(&raw_pointer, cmdbuf + 0x70, sizeof raw_pointer); assert(raw_pointer == cmdbuf);
    assert(slot("nvnCommandBufferAddControlMemory") != wonder_nvn_unimplemented_thunk((unsigned)wonder_nvn_name_index("nvnCommandBufferAddControlMemory")));
    _Alignas(8) unsigned char control[512]; memset(control, 0xa5, sizeof control);
    ((void (*)(void *, void *, size_t))slot("nvnCommandBufferAddControlMemory"))(cmdbuf, control, sizeof control);
    memcpy(&raw_pointer, cmdbuf + 0x20, sizeof raw_pointer); assert(raw_pointer == control);
    memcpy(&raw_pointer, cmdbuf + 0x28, sizeof raw_pointer); assert(raw_pointer == control + sizeof control - 16);
    for (unsigned i = 0; i < sizeof control; i++) assert(control[i] == 0xa5); // provisioning does not submit commands
    memcpy(&raw_pointer, cmdbuf + 0x38, sizeof raw_pointer); assert(raw_pointer == control);
    memcpy(&raw_pointer, cmdbuf + 0x48, sizeof raw_pointer); assert(raw_pointer == control);
    uint64_t accumulated; memcpy(&accumulated, cmdbuf + 0x50, sizeof accumulated); assert(accumulated == 0);
    void (*add_control)(void *, void *, size_t) = slot("nvnCommandBufferAddControlMemory");
    EXPECT_STOP(add_control(NULL, control, sizeof control), "NVN_INVALID_OBJECT");
    EXPECT_STOP(add_control(cmdbuf, NULL, sizeof control), "NVN_INVALID_ARGUMENT");
    EXPECT_STOP(add_control(cmdbuf, control + 1, 256), "NVN_INVALID_ARGUMENT");
    EXPECT_STOP(add_control(cmdbuf, control, 24), "NVN_INVALID_ARGUMENT");   // below header + link tail (release driver has no 256 minimum)
    EXPECT_STOP(add_control(cmdbuf, (void *)(UINTPTR_MAX - 7), 512), "NVN_INVALID_ARGUMENT");
    cmdbuf[0x11] = 1;
    EXPECT_STOP(add_control(cmdbuf, control, sizeof control), "NVN_UNSUPPORTED");
    cmdbuf[0x11] = 0;
    assert(slot("nvnCommandBufferAddCommandMemory") != wonder_nvn_unimplemented_thunk((unsigned)wonder_nvn_name_index("nvnCommandBufferAddCommandMemory")));
    void (*add_command)(void *, const void *, ptrdiff_t, size_t) = slot("nvnCommandBufferAddCommandMemory");
    add_command(cmdbuf, memory_pool, 0, 0x1000);
    memcpy(&raw_pointer, cmdbuf, sizeof raw_pointer); assert(raw_pointer == descriptor_memory);
    memcpy(&raw_pointer, cmdbuf + 8, sizeof raw_pointer); assert(raw_pointer == descriptor_memory + 0x1000);
    memcpy(&raw_pointer, cmdbuf + 0x18, sizeof raw_pointer); assert(raw_pointer == descriptor_memory + 0x1000);
    memcpy(&raw_pointer, cmdbuf + 0x30, sizeof raw_pointer); assert(raw_pointer == descriptor_memory);
    memcpy(&raw_pointer, cmdbuf + 0x40, sizeof raw_pointer); assert(raw_pointer == descriptor_memory);
    memcpy(&raw_pointer, cmdbuf + 0x58, sizeof raw_pointer); assert(raw_pointer == descriptor_memory);
    EXPECT_STOP(add_command(cmdbuf, memory_pool, 2, 512), "NVN_INVALID_ARGUMENT");
    EXPECT_STOP(add_command(cmdbuf, memory_pool, 0, 128), "NVN_INVALID_ARGUMENT");
    EXPECT_STOP(add_command(cmdbuf, memory_pool, pool_size, 512), "NVN_INVALID_ARGUMENT");
    {   // Outside a recording, bytes between the segment start and the cursor would be lost: refuse.
        uintptr_t saved, moved; memcpy(&saved, cmdbuf, sizeof saved); moved = saved + 8; memcpy(cmdbuf, &moved, sizeof moved);
        EXPECT_STOP(add_command(cmdbuf, memory_pool, 0, 512), "NVN_INVALID_STATE");
        memcpy(cmdbuf, &saved, sizeof saved);
    }



    static _Alignas(4096) unsigned char cb_memory[0x10000];
    wonder_nvnMemoryPoolBuilderSetDefaults(pb); wonder_nvnMemoryPoolBuilderSetDevice(pb, device);
    wonder_nvnMemoryPoolBuilderSetFlags(pb, 0x12); wonder_nvnMemoryPoolBuilderSetStorage(pb, cb_memory, sizeof cb_memory);
    static _Alignas(16) unsigned char cb_pool[WONDER_NVN_MEMORY_POOL_SIZE];
    assert(wonder_nvnMemoryPoolInitialize(cb_pool, pb) == 1);
    unsigned char bb[0x40]; memset(bb, 0xcd, sizeof bb);
    ((void (*)(void *, const void *))slot("nvnBufferBuilderSetDevice"))(bb, device);
    ((void (*)(void *))slot("nvnBufferBuilderSetDefaults"))(bb);
    EXPECT_STOP(wonder_nvnBufferBuilderSetStorage(bb, cb_pool, 0xfff0, 0x20), "NVN_INVALID_ARGUMENT");
    ((void (*)(void *, const void *, ptrdiff_t, size_t))slot("nvnBufferBuilderSetStorage"))(bb, cb_pool, 0x8000, 0x20);
    static _Alignas(16) unsigned char buffer[0x30];
    assert(((uint32_t (*)(void *, const void *))slot("nvnBufferInitialize"))(buffer, bb) == 1);
    assert(((void *(*)(const void *))slot("nvnBufferMap"))(buffer) == cb_memory + 0x8000);
    ((void (*)(void *, const char *))slot("nvnBufferSetDebugLabel"))(buffer, "Vfx buffer");
    assert(last_labeled_resource == (void *)(uintptr_t)(0xb0000 + buffers) && !strcmp(last_resource_label, "Vfx buffer"));
    wonder_nvnBufferBuilderSetStorage(bb, sc_pool, 0, 0x20); static _Alignas(16) unsigned char gpu_only[0x30];
    assert(wonder_nvnBufferInitialize(gpu_only, bb) == 1);
    EXPECT_STOP(wonder_nvnBufferMap(gpu_only), "NVN_INVALID_ARGUMENT");                     // CPU_NO_ACCESS pool
    // main+0x42367c fast-clear registration; pool/buffer queries (0x420904, 0x422498..0x4225f4).
    float red[4] = {1, 0, 0, 1};
    assert(((uint32_t (*)(void *, const float *, int))slot("nvnDeviceRegisterFastClearColor"))(device, red, 0x29) == 1);
    for (int i = 1; i < 16; i++) assert(((uint32_t (*)(void *, float))slot("nvnDeviceRegisterFastClearDepth"))(device, 1.0f) == 1);
    assert(((uint32_t (*)(void *, const float *, int))slot("nvnDeviceRegisterFastClearColor"))(device, red, 0x25) == 0);   // table full
    assert(((uint64_t (*)(const void *))slot("nvnBufferGetAddress"))(buffer) == (uint64_t)(uintptr_t)(cb_memory + 0x8000));
    assert(((int (*)(const void *))slot("nvnMemoryPoolGetFlags"))(cb_pool) == 0x12);
    assert(((void *(*)(const void *))slot("nvnMemoryPoolMap"))(cb_pool) == cb_memory && ((void *(*)(const void *))slot("nvnMemoryPoolMap"))(sc_pool) == NULL);
    ((void (*)(const void *, ptrdiff_t, size_t))slot("nvnMemoryPoolFlushMappedRange"))(cb_pool, 0x1000, 0x2000);
    ((void (*)(const void *, ptrdiff_t, size_t))slot("nvnMemoryPoolInvalidateMappedRange"))(cb_pool, 0, sizeof cb_memory);
    // SDK 55: Flush/Invalidate return immediately for pools without CPU_CACHED (sdk+0x4539a0/cc).
    ((void (*)(const void *, ptrdiff_t, size_t))slot("nvnMemoryPoolFlushMappedRange"))(sc_pool, 0, 0x1000);
    ((void (*)(const void *, ptrdiff_t, size_t))slot("nvnMemoryPoolInvalidateMappedRange"))(sc_pool, 0, 0x1000);

    // Target SDK view is 16 bytes (sdk+0x45bee0); main+0x42cd5c places a copy region immediately after it.
    struct { unsigned char view[16]; unsigned char canary[24]; } compact_view;
    memset(&compact_view, 0xa5, sizeof compact_view);
    ((void (*)(void *))slot("nvnTextureViewSetDefaults"))(compact_view.view);
    ((void (*)(void *, int, int))slot("nvnTextureViewSetLevels"))(compact_view.view, 0, 1);
    for (unsigned i = 0; i < sizeof compact_view.canary; i++) assert(compact_view.canary[i] == 0xa5);
    assert(compact_view.view[4] == 0 && compact_view.view[5] == 1 && compact_view.view[7] == 1);
    assert(slot("nvnCommandBufferBeginRecording") != wonder_nvn_unimplemented_thunk((unsigned)wonder_nvn_name_index("nvnCommandBufferBeginRecording")));
    void (*begin_recording)(void *) = slot("nvnCommandBufferBeginRecording");
    begin_recording(cmdbuf);
    assert(cmdbuf[0x11] == 1 && cmdbuf[0x12] == 0);
    memcpy(&raw_pointer, cmdbuf + 0x20, sizeof raw_pointer); assert(raw_pointer == control + 16);
    memcpy(&raw_pointer, cmdbuf + 0x48, sizeof raw_pointer); assert(raw_pointer == control);
    uint64_t begin_words[2]; memcpy(begin_words, control, sizeof begin_words);
    assert(begin_words[0] == UINT64_C(0xffffffffffffff01) && begin_words[1] == 0);
    for (unsigned i = 16; i < sizeof control; i++) assert(control[i] == 0xa5);
    EXPECT_STOP(begin_recording(cmdbuf), "NVN_INVALID_STATE");
    assert(slot("nvnCommandBufferDrawElements") != wonder_nvn_unimplemented_thunk((unsigned)wonder_nvn_name_index("nvnCommandBufferDrawElements")));
    void (*draw_elements)(void *, int, int, int, uint64_t) = slot("nvnCommandBufferDrawElements");
    uint64_t (*end_recording)(void *) = slot("nvnCommandBufferEndRecording");
    draw_elements(cmdbuf, 4, 0, 6, (uintptr_t)descriptor_memory + 0x8000);
    uintptr_t original_cursor, invalid_cursor, command_end;
    memcpy(&original_cursor,cmdbuf,sizeof original_cursor);memcpy(&command_end,cmdbuf+0x18,sizeof command_end);
    invalid_cursor=command_end+4;memcpy(cmdbuf,&invalid_cursor,sizeof invalid_cursor);
    EXPECT_STOP(end_recording(cmdbuf),"NVN_INVALID_STATE");
    memcpy(cmdbuf,&original_cursor,sizeof original_cursor);
    uint64_t recorded_handle = end_recording(cmdbuf);
    assert(recorded_handle == (uintptr_t)control + 16 && !cmdbuf[0x11]);
    wonder_nvn_command_walk(recorded_handle, check_draw, descriptor_memory + 0x8000);
    assert(recorded_draws == 1);
    EXPECT_STOP(draw_elements(cmdbuf, 4, 0, 6, 0), "NVN_INVALID_STATE");
    EXPECT_STOP(end_recording(cmdbuf), "NVN_INVALID_STATE");
    EXPECT_STOP(wonder_nvn_command_walk(0x1234, check_draw, NULL), "NVN_INVALID_HANDLE");
    struct wonder_nvn_command_header saved_header;
    memcpy(&saved_header, descriptor_memory, sizeof saved_header);
    memset(descriptor_memory, 0, 4); // native/unknown packet cannot be silently skipped
    EXPECT_STOP(wonder_nvn_command_walk(recorded_handle, check_draw, NULL), "NVN_NATIVE_PACKET_UNSUPPORTED");
    memcpy(descriptor_memory, &saved_header, sizeof saved_header);
    assert(slot("nvnCommandBufferGetControlMemoryUsed") != wonder_nvn_unimplemented_thunk((unsigned)wonder_nvn_name_index("nvnCommandBufferGetControlMemoryUsed")));
    assert(((size_t (*)(const void *))slot("nvnCommandBufferGetControlMemorySize"))(cmdbuf) == 512);
    assert(((size_t (*)(const void *))slot("nvnCommandBufferGetControlMemoryUsed"))(cmdbuf) == 56);
    assert(((size_t (*)(const void *))slot("nvnCommandBufferGetControlMemoryFree"))(cmdbuf) == 456);
    assert(((size_t (*)(const void *))slot("nvnCommandBufferGetCommandMemorySize"))(cmdbuf) == 4096);
    assert(((size_t (*)(const void *))slot("nvnCommandBufferGetCommandMemoryUsed"))(cmdbuf) == 40);
    assert(((size_t (*)(const void *))slot("nvnCommandBufferGetCommandMemoryFree"))(cmdbuf) == 4056);
    assert(!((uint32_t (*)(const void *))slot("nvnCommandBufferIsRecording"))(cmdbuf));
    assert(((void *(*)(const void *))slot("nvnCommandBufferGetMemoryCallback"))(cmdbuf) == (void *)0x1dd6344);
    assert(((void *(*)(const void *))slot("nvnCommandBufferGetMemoryCallbackData"))(cmdbuf) == cmdbuf);
    ((void (*)(void *))slot("nvnCommandBufferFinalize"))(cmdbuf);
    wonder_nvn_command_walk(recorded_handle, check_draw, descriptor_memory + 0x8000);
    assert(recorded_draws == 2); // handle's storage outlives recording object
    ((void (*)(void *, uint64_t))slot("nvnDeviceFinalizeCommandHandle"))(device, recorded_handle);
    EXPECT_STOP(wonder_nvn_command_walk(recorded_handle, check_draw, NULL), "NVN_INVALID_HANDLE");


    {   // main+0x5de9c: bindless texture handle = texture | sampler << 20.
        uint64_t (*handle)(const void *, int, int) = (uint64_t (*)(const void *, int, int))slot("nvnDeviceGetTextureHandle");
        assert(handle(device, 5, 3) == (UINT64_C(1) << 32 | UINT64_C(3) << 20 | 5));          // sdk+0x44e150
        assert(handle(device, 0xfffff, 0xfff) == UINT64_C(0xffffffffffffffff));               // sign-extended
        EXPECT_STOP(handle(device, 1 << 20, 0), "NVN_INVALID_ARGUMENT nvnDeviceGetTextureHandle");
        EXPECT_STOP(handle(device, 0, -1), "NVN_INVALID_ARGUMENT nvnDeviceGetTextureHandle");
        EXPECT_STOP(handle(builder, 0, 0), "NVN_INVALID_OBJECT");
    }
    // main+0x1084ef4: nn::gfx TextureInfo+0x24 packaged layout (RomFS BRTI survey: u32 block-height log2, u32 0x00010007).
    {
        typedef void (*set_layout_fn)(void *, const void *); typedef size_t (*size_fn)(const void *);
        set_layout_fn set_layout = (set_layout_fn)slot("nvnTextureBuilderSetPackagedTextureLayout");
        size_fn storage = (size_fn)slot("nvnTextureBuilderGetStorageSize");
        static unsigned char ptb[0x80];
        ((void (*)(void *, const void *))slot("nvnTextureBuilderSetDevice"))(ptb, device);
        ((void (*)(void *))slot("nvnTextureBuilderSetDefaults"))(ptb);
        ((void (*)(void *, int, int))slot("nvnTextureBuilderSetSize2D"))(ptb, 64, 72);
        ((void (*)(void *, int))slot("nvnTextureBuilderSetFormat"))(ptb, 0x25);              // RGBA8: 64 px = 4 GOBs wide
        assert(storage(ptb) == 256 * 128);                                                    // automatic: 8-GOB blocks
        static const unsigned char log2_1[8] = {1, 0, 0, 0, 7, 0, 1, 0};
        set_layout(ptb, log2_1);
        assert(storage(ptb) == 256 * 80);                                                     // packaged: 2-GOB blocks, 72 -> 80 rows
        static const unsigned char bad_word1[8] = {1, 0, 0, 0, 7, 0, 0, 0}, bad_log2[8] = {6, 0, 0, 0, 7, 0, 1, 0};
        EXPECT_STOP(set_layout(ptb, bad_word1), "NVN_UNSUPPORTED nvnTextureBuilderSetPackagedTextureLayout");
        EXPECT_STOP(set_layout(ptb, bad_log2), "NVN_UNSUPPORTED nvnTextureBuilderSetPackagedTextureLayout");
        assert(storage(ptb) == 256 * 80);                                                     // rejected layouts change nothing
        set_layout(ptb, NULL);
        assert(storage(ptb) == 256 * 128);
        set_layout(ptb, log2_1);
        ((void (*)(void *))slot("nvnTextureBuilderSetDefaults"))(ptb);                         // SetDefaults clears it
        ((void (*)(void *, int, int))slot("nvnTextureBuilderSetSize2D"))(ptb, 64, 72);
        ((void (*)(void *, int))slot("nvnTextureBuilderSetFormat"))(ptb, 0x25);
        assert(storage(ptb) == 256 * 128);
    }

    // main+0x77714: general texture via SetWidth/Height/Depth/Levels/Swizzle/Samples/DepthStencilMode, then pool registration.
    static unsigned char gtb[0x80]; memset(gtb, 0xab, sizeof gtb);
    ((void (*)(void *, const void *))slot("nvnTextureBuilderSetDevice"))(gtb, device);
    ((void (*)(void *))slot("nvnTextureBuilderSetDefaults"))(gtb);
    ((void (*)(void *, int))slot("nvnTextureBuilderSetTarget"))(gtb, 1);
    ((void (*)(void *, int))slot("nvnTextureBuilderSetWidth"))(gtb, 128);
    ((void (*)(void *, int))slot("nvnTextureBuilderSetHeight"))(gtb, 64);
    ((void (*)(void *, int))slot("nvnTextureBuilderSetDepth"))(gtb, 1);
    ((void (*)(void *, int))slot("nvnTextureBuilderSetLevels"))(gtb, 3);
    ((void (*)(void *, int))slot("nvnTextureBuilderSetFormat"))(gtb, 0x42);            // BC1: CPU decode on A11
    ((void (*)(void *, int, int, int, int))slot("nvnTextureBuilderSetSwizzle"))(gtb, 2, 3, 4, 1);
    ((void (*)(void *, int))slot("nvnTextureBuilderSetSamples"))(gtb, 0);
    ((void (*)(void *, int))slot("nvnTextureBuilderSetDepthStencilMode"))(gtb, 0);
    ((void (*)(void *, int))slot("nvnTextureBuilderSetFlags"))(gtb, 0);
    EXPECT_STOP(wonder_nvnTextureBuilderSetDefaults(NULL), "NVN_INVALID_OBJECT");
    // ZCull save storage: the host has no saveable ZCull, as the sdk reports for formats without it (sdk+0x45d0d0 -> 0).
    assert(((size_t (*)(const void *))slot("nvnTextureBuilderGetZCullStorageSize"))(gtb) == 0);
    EXPECT_STOP(((size_t (*)(const void *))slot("nvnTextureBuilderGetZCullStorageSize"))(device), "NVN_INVALID_OBJECT");
    size_t gsize = ((size_t (*)(const void *))slot("nvnTextureBuilderGetStorageSize"))(gtb);
    assert(gsize > 0 && gsize <= 0x10000);
    ((void (*)(void *, const void *, ptrdiff_t))slot("nvnTextureBuilderSetStorage"))(gtb, cb_pool, 0);
    static _Alignas(16) unsigned char gtex[0x80];
    assert(((uint32_t (*)(void *, const void *))slot("nvnTextureInitialize"))(gtex, gtb) == 1);
    assert(((int (*)(const void *))slot("nvnTextureGetWidth"))(gtex) == 128 && ((int (*)(const void *))slot("nvnTextureGetFormat"))(gtex) == 0x42);
    unsigned char view[0x28];
    ((void (*)(void *))slot("nvnTextureViewSetDefaults"))(view);
    ((void (*)(void *, int, int))slot("nvnTextureViewSetLevels"))(view, 1, 2);
    unsigned char copy[0x80]; memcpy(copy, gtex, sizeof copy);                        // game copies by value
    ((void (*)(const void *, int, const void *, const void *))slot("nvnTexturePoolRegisterTexture"))(texture_pool, 300, copy, view);
    const struct wonder_nvn_texture_registration *reg = wonder_nvn_texture_lookup(texture_pool, 300);
    assert(reg && reg->serial == ((struct wonder_nvn_texture *)gtex)->serial && reg->has_view && reg->view.base_level == 1 && reg->texture.swizzle[3] == 1);
    assert(!wonder_nvn_texture_lookup(texture_pool, 301));
    uint64_t gserial = ((struct wonder_nvn_texture *)gtex)->serial; unsigned forgets = forgotten_textures;
    ((void (*)(void *))slot("nvnTextureFinalize"))(gtex);
    EXPECT_STOP(wonder_nvnTextureGetWidth(gtex), "NVN_INVALID_OBJECT");
    assert(forgotten_textures == forgets + 1 && forgotten_serial == gserial);   // host GPU copies of the serial are released
    assert(wonder_nvn_texture_lookup(texture_pool, 300)->texture.desc.width == 128);  // registration keeps its snapshot
    // State objects: defaults + setters, validated ranges.
    uint64_t blend = 0, ds = 0, vstream = 0; uint32_t color = 0, mask = 0, poly = 0, vattr = 0; unsigned char ms[0x18];
    ((void (*)(void *))slot("nvnBlendStateSetDefaults"))(&blend); ((void (*)(void *, int, int, int, int))slot("nvnBlendStateSetBlendFunc"))(&blend, 5, 6, 2, 1);
    EXPECT_STOP(wonder_nvnBlendStateSetBlendFunc(&blend, 0, 1, 1, 1), "NVN_INVALID_ARGUMENT");
    // CONSTANT_COLOR .. ONE_MINUS_CONSTANT_ALPHA (0x61..0x64), round74; values between 0x13 and 0x61 stay invalid.
    wonder_nvnBlendStateSetBlendFunc(&blend, 0x61, 0x62, 0x63, 0x64);
    EXPECT_STOP(wonder_nvnBlendStateSetBlendFunc(&blend, 0x20, 1, 1, 1), "NVN_INVALID_ARGUMENT");
    EXPECT_STOP(wonder_nvnBlendStateSetBlendFunc(&blend, 0x65, 1, 1, 1), "NVN_INVALID_ARGUMENT");
    wonder_nvnBlendStateSetBlendFunc(&blend, 5, 6, 2, 1);
    ((void (*)(void *))slot("nvnColorStateSetDefaults"))(&color); ((void (*)(void *, int, unsigned))slot("nvnColorStateSetBlendEnable"))(&color, 2, 0xffffff01u);
    assert((color & 0xff) == 4);
    ((void (*)(void *))slot("nvnChannelMaskStateSetDefaults"))(&mask); ((void (*)(void *, int, unsigned, unsigned, unsigned, unsigned))slot("nvnChannelMaskStateSetChannelMask"))(&mask, 1, 1, 0, 1, 0);
    assert(((mask >> 4) & 0xf) == 5 && (mask & 0xf) == 0xf);
    ((void (*)(void *))slot("nvnDepthStencilStateSetDefaults"))(&ds); ((void (*)(void *, int, int))slot("nvnDepthStencilStateSetStencilFunc"))(&ds, 3, 4);
    EXPECT_STOP(wonder_nvnDepthStencilStateSetDepthFunc(&ds, 9), "NVN_INVALID_ARGUMENT");
    ((void (*)(void *))slot("nvnPolygonStateSetDefaults"))(&poly); ((void (*)(void *, int))slot("nvnPolygonStateSetCullFace"))(&poly, 2);
    ((void (*)(void *))slot("nvnMultisampleStateSetDefaults"))(ms); ((void (*)(void *, int))slot("nvnMultisampleStateSetSamples"))(ms, 4);
    ((void (*)(void *))slot("nvnVertexAttribStateSetDefaults"))(&vattr); ((void (*)(void *, int, ptrdiff_t))slot("nvnVertexAttribStateSetFormat"))(&vattr, 0x25, 12);
    ((void (*)(void *))slot("nvnVertexStreamStateSetDefaults"))(&vstream); ((void (*)(void *, ptrdiff_t))slot("nvnVertexStreamStateSetStride"))(&vstream, 32);
    assert((uint32_t)vstream == 32 && (vattr & 0xff) == 0x25 && ((vattr >> 8) & 0x7ffff) == 12);
    // main+0x42cdc0 WriteTexels: storage stays TX1 block-linear; read back through the D2R deswizzler.
    static _Alignas(4096) unsigned char wt_memory[0x10000];
    wonder_nvnMemoryPoolBuilderSetDefaults(pb); wonder_nvnMemoryPoolBuilderSetDevice(pb, device);
    wonder_nvnMemoryPoolBuilderSetFlags(pb, 0x22); wonder_nvnMemoryPoolBuilderSetStorage(pb, wt_memory, sizeof wt_memory);
    static _Alignas(16) unsigned char wt_pool[WONDER_NVN_MEMORY_POOL_SIZE]; assert(wonder_nvnMemoryPoolInitialize(wt_pool, pb) == 1);
    unsigned char wtb[0x80]; wonder_nvnTextureBuilderSetDefaults(wtb); wonder_nvnTextureBuilderSetDevice(wtb, device);
    ((void (*)(void *, int, int))slot("nvnTextureBuilderSetSize2D"))(wtb, 64, 16); ((void (*)(void *, int))slot("nvnTextureBuilderSetFormat"))(wtb, 0x25);
    ((void (*)(void *, const void *, ptrdiff_t))slot("nvnTextureBuilderSetStorage"))(wtb, wt_pool, 0);
    static _Alignas(16) unsigned char wt_tex[0x80]; assert(wonder_nvnTextureInitialize(wt_tex, wtb) == 1);
    static uint32_t texels[64 * 16], back[64 * 16];
    for (int i = 0; i < 64 * 16; i++) texels[i] = 0x01000000u * (uint32_t)(i & 0xff) + (uint32_t)i;
    struct { int x, y, z, w, h, d; } full = {0, 0, 0, 64, 16, 1}, part = {8, 3, 0, 5, 2, 1};
    void (*write)(const void *, const void *, const void *, const void *) = slot("nvnTextureWriteTexels");
    struct { int x, y, z, w, h, d; } overflow_region = {1 << 30, 0, 0, 1, 1, 1};
    EXPECT_STOP(write(wt_tex, NULL, &overflow_region, texels), "NVN_INVALID_ARGUMENT");
    write(wt_tex, NULL, &full, texels);
    assert(memcmp(wt_memory, texels, 64) != 0);                                       // stored swizzled, not linear
    assert(!nvn_deswizzle(&((struct wonder_nvn_texture *)wt_tex)->desc, 0, 0, wt_memory, back, 64 * 4) && !memcmp(back, texels, sizeof texels));
    uint32_t patch[10]; for (int i = 0; i < 10; i++) patch[i] = 0xdead0000u + (uint32_t)i;
    write(wt_tex, NULL, &part, patch);
    nvn_deswizzle(&((struct wonder_nvn_texture *)wt_tex)->desc, 0, 0, wt_memory, back, 64 * 4);
    assert(back[3 * 64 + 8] == 0xdead0000u && back[4 * 64 + 12] == 0xdead0009u && back[3 * 64 + 7] == texels[3 * 64 + 7] && back[3 * 64 + 13] == texels[3 * 64 + 13]);
    ((void (*)(const void *, const void *, const void *))slot("nvnTextureFlushTexels"))(wt_tex, NULL, &full);
    assert(((int (*)(const void *))slot("nvnTextureBuilderGetStorageClass"))(wtb) == ((int (*)(const void *))slot("nvnTextureGetStorageClass"))(wt_tex));
    // main+0x42dcf4..0x42dda8: virtual pool (no storage) mapped from a PHYSICAL pool in 64 KiB pages.
    static _Alignas(65536) unsigned char phys_memory[0x40000];
    wonder_nvnMemoryPoolBuilderSetDefaults(pb); wonder_nvnMemoryPoolBuilderSetDevice(pb, device);
    wonder_nvnMemoryPoolBuilderSetFlags(pb, 0x121); wonder_nvnMemoryPoolBuilderSetStorage(pb, phys_memory, sizeof phys_memory);
    static _Alignas(16) unsigned char phys_pool[WONDER_NVN_MEMORY_POOL_SIZE]; assert(wonder_nvnMemoryPoolInitialize(phys_pool, pb) == 1);
    wonder_nvnMemoryPoolBuilderSetDefaults(pb); wonder_nvnMemoryPoolBuilderSetDevice(pb, device);
    EXPECT_STOP(wonder_nvnMemoryPoolBuilderSetFlags(pb, 0x2a2), "NVN_UNSUPPORTED");                    // virtual needs CPU_NO_ACCESS
    wonder_nvnMemoryPoolBuilderSetFlags(pb, 0x2a1);
    EXPECT_STOP(wonder_nvnMemoryPoolBuilderSetStorage(pb, phys_memory, 0x40000), "NVN_INVALID_ARGUMENT");    // virtual has no storage
    wonder_nvnMemoryPoolBuilderSetStorage(pb, NULL, 0x80000);
    static _Alignas(16) unsigned char virt_pool[WONDER_NVN_MEMORY_POOL_SIZE]; assert(wonder_nvnMemoryPoolInitialize(virt_pool, pb) == 1);
    struct mreq { const void *phys; int64_t poff, voff; uint64_t size; int32_t sc; } req = {phys_pool, 0, 0x10000, 0x30000, 0x104};
    uint32_t (*map_virtual)(void *, int, const void *) = slot("nvnMemoryPoolMapVirtual");
    assert(map_virtual(virt_pool, 1, &req) == 1);
    // Resolve virtual addresses to the physical bytes; two virtual pages alias the same storage.
    struct wonder_nvn_pool_view span;
    wonder_nvn_pool_span(virt_pool, 0x10000, 0x30000, &span);
    assert(span.memory == phys_memory && span.size == 0x30000);
    req = (struct mreq){phys_pool, 0, 0x50000, 0x10000, 0x104};
    assert(map_virtual(virt_pool, 1, &req) == 1);
    wonder_nvn_pool_span(virt_pool, 0x50005, 1, &span);
    span.memory[0] = 0x7b;
    wonder_nvn_pool_span(virt_pool, 0x10005, 1, &span);
    assert(span.memory[0] == 0x7b && phys_memory[5] == 0x7b);
    req.voff = 0x20000; req.size = 0x10000; req.phys = NULL; assert(map_virtual(virt_pool, 1, &req) == 1);        // unmap the middle page
    wonder_nvn_pool_span(virt_pool, 0x10000, 0x30000, &span);
    assert(span.memory == phys_memory && span.size == 0x10000); // clipped before the unmapped hole
    EXPECT_STOP(wonder_nvn_pool_span(virt_pool, 0x20000, 1, &span), "NVN_UNMAPPED");
    wonder_nvn_pool_span(virt_pool, 0x30000, 0x10000, &span);
    assert(span.memory == phys_memory + 0x20000 && span.size == 0x10000); // right split preserves offset
    req = (struct mreq){wt_pool, 0, 0, 0x10000, 0x104};
    EXPECT_STOP(map_virtual(virt_pool, 1, &req), "NVN_INVALID_ARGUMENT");                                         // not a PHYSICAL pool
    req = (struct mreq){phys_pool, 0x8000, 0, 0x10000, 0x104};
    EXPECT_STOP(map_virtual(virt_pool, 1, &req), "NVN_INVALID_ARGUMENT");                                         // unaligned physical offset
    EXPECT_STOP(map_virtual(phys_pool, 1, &req), "NVN_INVALID_ARGUMENT");                                         // not virtual
    req = (struct mreq){phys_pool, 0x10000, 0x10000, UINT64_C(0xffffffffffff0000), 0x104};
    EXPECT_STOP(map_virtual(virt_pool, 1, &req), "NVN_INVALID_ARGUMENT");
    // ---- Command recording: typed records, snapshots, callback-driven segments, CallCommands ----
    {
        static _Alignas(16) unsigned char rcb[0xa0]; static _Alignas(8) unsigned char rctl[512];
        assert(((uint32_t (*)(void *, const void *))slot("nvnCommandBufferInitialize"))(rcb, device) == 1);
        ((void (*)(void *, void *))slot("nvnCommandBufferSetMemoryCallback"))(rcb, (void *)rec_memory_callback);
        ((void (*)(void *, void *))slot("nvnCommandBufferSetMemoryCallbackData"))(rcb, (void *)0x5eed);
        ((void (*)(void *, void *, size_t))slot("nvnCommandBufferAddControlMemory"))(rcb, rctl, sizeof rctl);
        rec_pool = cb_pool; rec_next = 0xa000;
        ((void (*)(void *, const void *, ptrdiff_t, size_t))slot("nvnCommandBufferAddCommandMemory"))(rcb, cb_pool, 0x9000, 0x200);
        ((void (*)(void *))slot("nvnCommandBufferBeginRecording"))(rcb);
        float color[4] = {0.25f, 0.5f, 0.75f, 1.0f};
        ((void (*)(void *, int, const float *, int))slot("nvnCommandBufferClearColor"))(rcb, 0, color, 0xf);
        color[0] = 9.0f;                                                                  // snapshot: later writes do not leak
        unsigned char blend[8], ds[8]; ((void (*)(void *))slot("nvnBlendStateSetDefaults"))(blend); ((void (*)(void *))slot("nvnDepthStencilStateSetDefaults"))(ds);
        ((void (*)(void *, const void *))slot("nvnCommandBufferBindBlendState"))(rcb, blend);
        ((void (*)(void *, const void *))slot("nvnCommandBufferBindDepthStencilState"))(rcb, ds);
        ((void (*)(void *, int, int, int, int))slot("nvnCommandBufferSetViewport"))(rcb, 0, 0, 1280, 720);
        ((void (*)(void *, int, int, int, int))slot("nvnCommandBufferSetScissor"))(rcb, 0, 0, 1280, 720);
        ((void (*)(void *, float, float))slot("nvnCommandBufferSetDepthRange"))(rcb, 0.0f, 1.0f);
        const void *rt[1] = {wt_tex};
        ((void (*)(void *, int, const void *const *, const void *const *, const void *, const void *))slot("nvnCommandBufferSetRenderTargets"))(rcb, 1, rt, NULL, NULL, NULL);
        uint64_t ubo = (uintptr_t)(cb_memory + 0x8000), words[4] = {0x1122334455667788u, 2, 3, 4};
        ((void (*)(void *, uint64_t, size_t, ptrdiff_t, size_t, const void *))slot("nvnCommandBufferUpdateUniformBuffer"))(rcb, ubo, 0x20, 0, 0x20, words);
        words[0] = 0;
        for (int i = 0; i < 40; i++) ((void (*)(void *, int, int, int))slot("nvnCommandBufferDrawArrays"))(rcb, 4, 0, 3);   // overflow 0x200 -> callback
        ((void (*)(void *, int))slot("nvnCommandBufferBarrier"))(rcb, 0x12);
        ((void (*)(void *, int, uint64_t))slot("nvnCommandBufferReportCounter"))(rcb, 0, (uintptr_t)(cb_memory + 0x8000));
        EXPECT_STOP(((void (*)(void *, int, uint64_t))slot("nvnCommandBufferReportCounter"))(rcb, 0, (uintptr_t)(cb_memory + 0x8004)), "16-byte aligned");
        EXPECT_STOP(((void (*)(void *, int, int, int))slot("nvnCommandBufferDrawArrays"))(rcb, 15, 0, 3), "NVN_INVALID_ARGUMENT");
        EXPECT_STOP(((void (*)(void *, int, int, uint64_t, size_t))slot("nvnCommandBufferBindUniformBuffer"))(rcb, 0, 0, 0x10, 0x10), "NVN_INVALID_ADDRESS");
        uint64_t sub = ((uint64_t (*)(void *))slot("nvnCommandBufferEndRecording"))(rcb);
        assert(rec_callbacks >= 1);
        uint64_t group; memcpy(&group, rctl + 16, 8); assert((group & 0xffffffff) == 3 && (group >> 32) == rec_callbacks + 1);
        struct rec_trace trace = {0};
        wonder_nvn_command_walk(sub, trace_commands, &trace);
        assert(trace.count == 50 && trace.opcodes[0] == WONDER_NVN_CMD_CLEAR_COLOR && trace.opcodes[5] == WONDER_NVN_CMD_SET_DEPTH_RANGES);
        assert(trace.opcodes[6] == WONDER_NVN_CMD_SET_RENDER_TARGETS && trace.targets == 1 && trace.clear[0] == 0.25f);
        assert(trace.uniform_word == 0x1122334455667788u && trace.opcodes[49] == WONDER_NVN_CMD_REPORT_COUNTER);
        // A second recording calls the first: handles are validated at record time.
        ((void (*)(void *))slot("nvnCommandBufferBeginRecording"))(rcb);
        ((void (*)(void *, int, const uint64_t *))slot("nvnCommandBufferCallCommands"))(rcb, 1, &sub);
        uint64_t bogus = 0x777;
        EXPECT_STOP(((void (*)(void *, int, const uint64_t *))slot("nvnCommandBufferCallCommands"))(rcb, 1, &bogus), "NVN_INVALID_HANDLE");
        uint64_t outer = ((uint64_t (*)(void *))slot("nvnCommandBufferEndRecording"))(rcb);
        struct rec_trace outer_trace = {0}; wonder_nvn_command_walk(outer, trace_commands, &outer_trace);
        assert(outer_trace.count == 1 && outer_trace.opcodes[0] == WONDER_NVN_CMD_CALL_COMMANDS);
        // Disabled callback: exhaustion stops by name instead of overrunning.
        ((void (*)(void *, uint32_t))slot("nvnCommandBufferSetCommandMemoryCallbackEnabled"))(rcb, 0);
        ((void (*)(void *))slot("nvnCommandBufferBeginRecording"))(rcb);
        EXPECT_STOP(for (int i = 0; i < 4096; i++) ((void (*)(void *, int, int, int))slot("nvnCommandBufferDrawArrays"))(rcb, 4, 0, 3), "NVN_COMMAND_MEMORY_EXHAUSTED");
        rcb[0x11] = 0;
        ((void (*)(void *))slot("nvnCommandBufferFinalize"))(rcb);
    }
    // ---- Queue replay: clears, uniform updates, reports, fences, present; unsupported work stops by name ----
    {
        static _Alignas(16) unsigned char qcb[0xa0]; static _Alignas(8) unsigned char qctl[512];
        assert(((uint32_t (*)(void *, const void *))slot("nvnCommandBufferInitialize"))(qcb, device) == 1);
        ((void (*)(void *, void *, size_t))slot("nvnCommandBufferAddControlMemory"))(qcb, qctl, sizeof qctl);
        ((void (*)(void *, const void *, ptrdiff_t, size_t))slot("nvnCommandBufferAddCommandMemory"))(qcb, cb_pool, 0xd000, 0x1000);
        void (*begin)(void *) = slot("nvnCommandBufferBeginRecording"); uint64_t (*end)(void *) = slot("nvnCommandBufferEndRecording");
        void (*submit)(void *, int, const uint64_t *) = slot("nvnQueueSubmitCommands");
        void (*set_targets)(void *, int, const void *const *, const void *const *, const void *, const void *) = slot("nvnCommandBufferSetRenderTargets");
        void (*scissor)(void *, int, int, int, int) = slot("nvnCommandBufferSetScissor");
        void (*clear)(void *, int, const float *, int) = slot("nvnCommandBufferClearColor");
        const void *target[1] = {textures[0]};    // display texture: owns its host render target
        float blue[4] = {0, 0, 1, 1};
        uint32_t *report = (uint32_t *)(cb_memory + 0x8010); uint64_t *stamp = (uint64_t *)(cb_memory + 0x8020);
        report[0] = 0; stamp[0] = stamp[1] = 7;
        uint64_t *stamp_top = (uint64_t *)(cb_memory + 0x8030); stamp_top[0] = stamp_top[1] = 7;
        begin(qcb);
        set_targets(qcb, 1, target, NULL, NULL, NULL);
        scissor(qcb, 0, 0, 1920, 1080);
        clear(qcb, 0, blue, 0xf);
        uint32_t ubo[2] = {11, 22};
        ((void (*)(void *, uint64_t, size_t, ptrdiff_t, size_t, const void *))slot("nvnCommandBufferUpdateUniformBuffer"))(qcb, (uintptr_t)(cb_memory + 0x8000), 0x10, 0, 8, ubo);
        ((void (*)(void *, uint32_t, uint64_t))slot("nvnCommandBufferReportValue"))(qcb, 0xabcd, (uintptr_t)report);
        ((void (*)(void *, int, uint64_t))slot("nvnCommandBufferReportCounter"))(qcb, 0, (uintptr_t)stamp);
        ((void (*)(void *, int, uint64_t))slot("nvnCommandBufferReportCounter"))(qcb, 0x10, (uintptr_t)stamp_top);   // TIMESTAMP_TOP
        ((void (*)(void *, int))slot("nvnCommandBufferBarrier"))(qcb, 0x10);
        ((void (*)(void *, int))slot("nvnCommandBufferBarrier"))(qcb, 0x101);   // bit 8 exists in SDK 55 (sdk+0x478fc8); round73 uses it
        EXPECT_STOP(((void (*)(void *, int))slot("nvnCommandBufferBarrier"))(qcb, 0x200), "NVN_INVALID_ARGUMENT");
        ((void (*)(void *, int))slot("nvnCommandBufferSetTiledCacheAction"))(qcb, 1);
        uint64_t frame = end(qcb);
        submit(queue, 1, &frame);
        assert(host_clears == 1 && host_last_clear[2] == 1.0f && host_writes == 1 && report[0] == 0);   // reports wait for the GPU
        int (*sync_wait)(const void *, uint64_t) = slot("nvnSyncWait");
        static _Alignas(16) unsigned char fence_sync[0x40];
        assert(((uint32_t (*)(void *, const void *))slot("nvnSyncInitialize"))(fence_sync, device) == 1);
        EXPECT_STOP(sync_wait(fence_sync, 0), "never fenced");
        ((void (*)(void *, void *, int, int))slot("nvnQueueFenceSync"))(queue, fence_sync, 0, 0);
        assert(sync_wait(fence_sync, UINT64_MAX) == 0 && report[0] == 0xabcd && stamp[0] == 0 && stamp[1] != 7);
        assert(stamp_top[0] == 0 && stamp_top[1] != 7 && stamp_top[1] >= stamp[1]);
        ((void (*)(void *))slot("nvnQueueFlush"))(queue); ((void (*)(void *))slot("nvnQueueFinish"))(queue);
        // Present returns the texture to the window; the next acquire can reuse it.
        void (*present)(void *, void *, int) = slot("nvnQueuePresentTexture");
        ((void (*)(void *, int, int, int, int))slot("nvnWindowSetCrop"))(window, 0, 0, 1280, 720);
        unsigned before_present = pending_count;
        present(queue, window, 0);
        assert(host_presents == 1);
        assert(pending_count == before_present + 1);   // texture release registered on the GPU work that presents it
        EXPECT_STOP(present(queue, window, 0), "not acquired");
        // Acquire hands the texture back only after that GPU work completed (the game reuses its per-frame
        // resources once the texture is available): the fake host runs completion handlers on flush.
        assert(acquire(window, sync, &index) == 0 && index == 0 && pending_count == 0);
        ((void (*)(void *, int))slot("nvnWindowSetPresentInterval"))(window, 2);
        ((void (*)(void *, int))slot("nvnWindowSetNumActiveTextures"))(window, N);
        EXPECT_STOP(((void (*)(void *, int))slot("nvnWindowSetNumActiveTextures"))(window, N + 1), "NVN_INVALID_ARGUMENT");
        // Work the host cannot execute yet stops by name instead of being skipped.
        begin(qcb); set_targets(qcb, 1, target, NULL, NULL, NULL);
        ((void (*)(void *, int, int, int))slot("nvnCommandBufferDrawArrays"))(qcb, 4, 0, 3);
        uint64_t draw = end(qcb);
        EXPECT_STOP(submit(queue, 1, &draw), "NVN_REPLAY_PENDING DrawArrays");
        begin(qcb); set_targets(qcb, 1, target, NULL, NULL, NULL); scissor(qcb, 0, 0, 64, 64); clear(qcb, 0, blue, 0xf);
        uint64_t partial = end(qcb);
        unsigned rects = host_clear_rects; submit(queue, 1, &partial);   // scissored clear: drawn over the scissor rect
        assert(host_clear_rects == rects + 1 && last_clear_rect[0] == 0 && last_clear_rect[2] == 64 && last_clear_rect[3] == 64 && last_clear_mask == 0xf);
        begin(qcb); scissor(qcb, 0, 0, 4096, 4096); clear(qcb, 0, blue, 0x7);
        uint64_t masked = end(qcb);
        rects = host_clear_rects; submit(queue, 1, &masked);              // channel-masked clear: drawn with the write mask
        assert(host_clear_rects == rects + 1 && last_clear_mask == 0x7);
        begin(qcb); ((void (*)(void *, uint32_t))slot("nvnCommandBufferSetRenderEnable"))(qcb, 0);
        ((void (*)(void *, int, int, int))slot("nvnCommandBufferDrawArrays"))(qcb, 4, 0, 3);   // disabled rendering: no-op
        ((void (*)(void *, uint32_t))slot("nvnCommandBufferSetRenderEnable"))(qcb, 1);
        ((void (*)(void *, int, uint64_t))slot("nvnCommandBufferReportCounter"))(qcb, 1, (uintptr_t)stamp);
        uint64_t samples = end(qcb);
        EXPECT_STOP(submit(queue, 1, &samples), "counter type 0x1 (SAMPLES_PASSED)");
        uint64_t bogus = 0x999; EXPECT_STOP(submit(queue, 1, &bogus), "NVN_INVALID_HANDLE");
        submit(queue, 1, &frame);                                                                        // replay state survives stops
        assert(host_clears == 2);
        wonder_host_flush();
    }
    // ---- Object lifetime / getters / programs (SDK sdk+0x44dd00.. semantics) ----
    {
        static _Alignas(8) unsigned char vs_control[0x880], fs_control[0x880], bad_control[0x880];
        uint32_t magic = 0x98761234u, stage_vs = 0, stage_fs = 1, stage_bad = 6;
        memcpy(vs_control, &magic, 4); memcpy(vs_control + 0x714, &stage_vs, 4);
        memcpy(fs_control, &magic, 4); memcpy(fs_control + 0x714, &stage_fs, 4);
        memcpy(bad_control, &magic, 4); memcpy(bad_control + 0x714, &stage_bad, 4);
        uint64_t code = (uintptr_t)(cb_memory + 0x100);
        struct { uint64_t code; const void *control; } stages[2] = {{code, vs_control}, {code + 0x100, fs_control}};
        uint32_t (*set_shaders)(void *, int, const void *) = slot("nvnProgramSetShaders");
        assert(set_shaders(program_memory, 2, stages) == 1 && captured_programs == 1);
        struct wonder_nvn_program_shaders shaders; assert(wonder_nvn_program_snapshot(program_memory, &shaders));
        assert(shaders.mask == 3 && shaders.code[1] == code + 0x100 && shaders.control[0] == vs_control);
        struct { uint64_t code; const void *control; uint64_t extra; } ext[1] = {{code, fs_control, 0x1234}};
        assert(((uint32_t (*)(void *, int, const void *))slot("nvnProgramSetShadersExt"))(program_memory, 1, ext) == 1);
        assert(wonder_nvn_program_snapshot(program_memory, &shaders) && shaders.mask == 2 && shaders.extra[1] == 0x1234);
        stages[1].control = vs_control; EXPECT_STOP(set_shaders(program_memory, 2, stages), "duplicate shader stage");
        stages[1].control = bad_control; EXPECT_STOP(set_shaders(program_memory, 2, stages), "NVN_UNSUPPORTED");
        stages[1].control = fs_control + 4; EXPECT_STOP(set_shaders(program_memory, 2, stages), "NVN_INVALID_ARGUMENT");
        stages[1].code = UINT64_C(0x10); stages[1].control = fs_control; EXPECT_STOP(set_shaders(program_memory, 2, stages), "NVN_INVALID_ADDRESS");
        // ---- First-draw census: everything the offline shader build needs, written before the draw stops ----
        {
            assert(census_count == 0);   // the program-less DrawArrays above did not consume the census
            uint32_t length = 0x100; memcpy(vs_control + 0x704, &length, 4); memcpy(fs_control + 0x704, &length, 4);
            for (unsigned i = 0; i < 0x200; i++) cb_memory[0x100 + i] = (unsigned char)(i * 7 + 1);
            stages[0].code = code; stages[0].control = vs_control; stages[1].code = code + 0x100; stages[1].control = fs_control;
            assert(set_shaders(program_memory, 2, stages) == 1);
            // Control memory below the advertised minimum (256, sdk+0x441460) is legal on the release driver: round71 passes 0x80.
            {
                static _Alignas(16) unsigned char scb[0xa0]; static _Alignas(8) unsigned char sctl[0x80];
                assert(((uint32_t (*)(void *, const void *))slot("nvnCommandBufferInitialize"))(scb, device) == 1);
                ((void (*)(void *, void *, size_t))slot("nvnCommandBufferAddControlMemory"))(scb, sctl, 0x80);
                EXPECT_STOP(((void (*)(void *, void *, size_t))slot("nvnCommandBufferAddControlMemory"))(scb, sctl, 0x18), "NVN_INVALID_ARGUMENT");   // header + link tail need 32
            }
            static _Alignas(16) unsigned char dcb[0xa0]; static _Alignas(8) unsigned char dctl[0x4000];
            assert(((uint32_t (*)(void *, const void *))slot("nvnCommandBufferInitialize"))(dcb, device) == 1);
            ((void (*)(void *, void *, size_t))slot("nvnCommandBufferAddControlMemory"))(dcb, dctl, sizeof dctl);
            ((void (*)(void *, const void *, ptrdiff_t, size_t))slot("nvnCommandBufferAddCommandMemory"))(dcb, cb_pool, 0xe000, 0x1000);
            unsigned char attrib[4], stream[8];
            ((void (*)(void *))slot("nvnVertexAttribStateSetDefaults"))(attrib);
            ((void (*)(void *, int, ptrdiff_t))slot("nvnVertexAttribStateSetFormat"))(attrib, 0x2c, 8);
            ((void (*)(void *))slot("nvnVertexStreamStateSetDefaults"))(stream);
            ((void (*)(void *, ptrdiff_t))slot("nvnVertexStreamStateSetStride"))(stream, 20);
            uint64_t handle = ((uint64_t (*)(const void *, int, int))slot("nvnDeviceGetTextureHandle"))(device, 300, 1);
            ((void (*)(void *))slot("nvnCommandBufferBeginRecording"))(dcb);
            ((void (*)(void *, const void *))slot("nvnCommandBufferSetTexturePool"))(dcb, texture_pool);
            ((void (*)(void *, const void *))slot("nvnCommandBufferSetSamplerPool"))(dcb, sampler_pool);
            ((void (*)(void *, const void *, int))slot("nvnCommandBufferBindProgram"))(dcb, program_memory, 0x11);
            ((void (*)(void *, int, const void *))slot("nvnCommandBufferBindVertexAttribState"))(dcb, 1, attrib);
            ((void (*)(void *, int, const void *))slot("nvnCommandBufferBindVertexStreamState"))(dcb, 1, stream);
            ((void (*)(void *, int, uint64_t, size_t))slot("nvnCommandBufferBindVertexBuffer"))(dcb, 0, (uintptr_t)(cb_memory + 0x9000), 0x100);
            ((void (*)(void *, int, int, uint64_t, size_t))slot("nvnCommandBufferBindUniformBuffer"))(dcb, 1, 3, (uintptr_t)(cb_memory + 0x9100), 0x100);
            ((void (*)(void *, int, int, uint64_t))slot("nvnCommandBufferBindTexture"))(dcb, 1, 0, handle);
            ((void (*)(void *, int, int, int, uint64_t))slot("nvnCommandBufferDrawElements"))(dcb, 4, 1, 6, (uintptr_t)(cb_memory + 0x9200));
            uint64_t draw = ((uint64_t (*)(void *))slot("nvnCommandBufferEndRecording"))(dcb);
            void (*dsubmit)(void *, int, const uint64_t *) = slot("nvnQueueSubmitCommands");
            EXPECT_STOP(dsubmit(queue, 1, &draw), "NVN_SHADER_MISSING 1 programs");
            assert(strstr(last_stop, "census=DrawCensus"));
            unsigned char digest[CC_SHA256_DIGEST_LENGTH]; char key[17];
            CC_SHA256(vs_control, 0x880, digest); for (int i = 0; i < 8; i++) sprintf(key + 2 * i, "%02x", digest[i]);
            assert(strstr(last_stop, key));
            char fs_key[17], dir[40], file[96]; CC_SHA256(fs_control, 0x880, digest); for (int i = 0; i < 8; i++) sprintf(fs_key + 2 * i, "%02x", digest[i]);
            snprintf(dir, sizeof dir, "%s-%s", key, fs_key);   // one census directory per program (vs-fs keys)
            size_t n; snprintf(file, sizeof file, "%s/stage-0.control.bin", dir);
            const unsigned char *bytes = census_file(file, &n); assert(bytes && n == 0x880 && !memcmp(bytes, vs_control, 0x880));
            snprintf(file, sizeof file, "%s/stage-1.code.bin", dir);
            bytes = census_file(file, &n); assert(bytes && n == 0x100 && !memcmp(bytes, cb_memory + 0x200, 0x100));
            snprintf(file, sizeof file, "%s/draw.json", dir); bytes = census_file(file, &n); assert(bytes);
            char *json = calloc(1, n + 1); memcpy(json, bytes, n);
            char want[64]; snprintf(want, sizeof want, "\"key\":\"%s\"", key); assert(strstr(json, want));
            assert(strstr(json, "\"primitive\":4") && strstr(json, "\"index_type\":1") && strstr(json, "\"count\":6"));
            assert(strstr(json, "\"depth_mode\":") && strstr(json, "\"window_origin\":"));
            assert(strstr(json, "{\"location\":0,\"format\":44,\"offset\":8,\"stream\":0}"));
            assert(strstr(json, "\"stride\":20"));
            assert(strstr(json, "{\"stage\":1,\"index\":3,\"size\":256,\"address\":"));
            assert(strstr(json, "\"stage\":1,\"index\":0,\"texture_id\":300,\"sampler_id\":1"));
            assert(strstr(json, "\"width\":128"));
            free(json);
            unsigned files = census_count;
            EXPECT_STOP(dsubmit(queue, 1, &draw), "NVN_SHADER_MISSING");
            assert(census_count == files);   // written once per program

            // ---- Batch census: after the first missing program the rest of the submit is census-only ----
            {
                static _Alignas(16) unsigned char program2[0x90]; static _Alignas(8) unsigned char vs2[0x880], fs2[0x880];
                memcpy(vs2, vs_control, 0x880); memcpy(fs2, fs_control, 0x880); vs2[0x100] = 0x5a; fs2[0x100] = 0xa5;   // different keys
                assert(((uint32_t (*)(void *, const void *))slot("nvnProgramInitialize"))(program2, device) == 1);
                struct { uint64_t code; const void *control; } st2[2] = {{code, vs2}, {code + 0x100, fs2}};
                assert(set_shaders(program2, 2, st2) == 1);
                float red[4] = {1, 0, 0, 1};
                ((void (*)(void *))slot("nvnCommandBufferBeginRecording"))(dcb);
                ((void (*)(void *, int, int, int, uint64_t))slot("nvnCommandBufferDrawElements"))(dcb, 4, 1, 6, (uintptr_t)(cb_memory + 0x9200));   // program 1 (missing)
                ((void (*)(void *, int, const float *, int))slot("nvnCommandBufferClearColor"))(dcb, 0, red, 0xf);                                   // must not run
                ((void (*)(void *, const void *, int))slot("nvnCommandBufferBindProgram"))(dcb, program2, 0x11);
                ((void (*)(void *, int, int, int, uint64_t))slot("nvnCommandBufferDrawElements"))(dcb, 4, 1, 6, (uintptr_t)(cb_memory + 0x9200));   // program 2 (missing)
                ((void (*)(void *, int, int, int, uint64_t))slot("nvnCommandBufferDrawElements"))(dcb, 4, 1, 6, (uintptr_t)(cb_memory + 0x9200));   // program 2 again
                ((void (*)(void *, const void *, int))slot("nvnCommandBufferBindProgram"))(dcb, program_memory, 0x11);
                uint64_t batch = ((uint64_t (*)(void *))slot("nvnCommandBufferEndRecording"))(dcb);
                unsigned clears_before = host_clears, draws_before = host_draws, files_before = census_count;
                EXPECT_STOP(dsubmit(queue, 1, &batch), "NVN_SHADER_MISSING 2 programs");
                assert(host_clears == clears_before && host_draws == draws_before);   // nothing after the first missing draw ran
                char key2[17], fkey2[17], dir2[40]; CC_SHA256(vs2, 0x880, digest); for (int i = 0; i < 8; i++) sprintf(key2 + 2 * i, "%02x", digest[i]);
                CC_SHA256(fs2, 0x880, digest); for (int i = 0; i < 8; i++) sprintf(fkey2 + 2 * i, "%02x", digest[i]);
                snprintf(dir2, sizeof dir2, "%s-%s", key2, fkey2); snprintf(file, sizeof file, "%s/draw.json", dir2);
                assert(census_file(file, &n) && census_count == files_before + 6);   // program 2's 5 files + index.txt; program 1 was written earlier
                const unsigned char *index = NULL; size_t index_size = 0;
                for (unsigned i = 0; i < census_count; i++) if (!strcmp(census_files[i].name, "index.txt")) { index = census_files[i].data; index_size = census_files[i].size; }
                char *index_text = calloc(1, index_size + 1); memcpy(index_text, index, index_size);
                assert(strstr(index_text, dir) && strstr(index_text, dir2)); free(index_text);
                assert(strstr(last_stop, key2) && strstr(last_stop, key));
                // The next submit replays normally again (census-only ends with the submit).
                ((void (*)(void *))slot("nvnProgramFinalize"))(program2);
            }

            // ---- With translated shaders the replay assembles a host draw from the recorded state ----
            wonder_nvnDeviceSetDepthMode(device, 0);   // NEAR_IS_MINUS_W, as Wonder sets it (round52 log)
            char err[128];
            const char *vs_refl = "stage vs\ndepth minus_one\nattr 0 0 float\n", *fs_refl = "stage ps\ncbuf 3 1\ntex 0 1 1 2d\nfout 0 float\n";
            assert(!wonder_shader_refl_parse(vs_refl, strlen(vs_refl), &fake_refl[0], err, sizeof err));
            struct wonder_shader_refl fs_parsed; assert(!wonder_shader_refl_parse(fs_refl, strlen(fs_refl), &fs_parsed, err, sizeof err));
            fake_refl[1] = fs_parsed; shaders_available = 1;
            EXPECT_STOP(dsubmit(queue, 1, &draw), "reflection stage");   // fake_vs_key unset: the vertex key gets a fragment reflection
            memcpy(fake_vs_key, key, sizeof fake_vs_key);
            unsigned draws = host_draws;
            assert(first_draw_captures == 0);
            dsubmit(queue, 1, &draw);
            assert(host_draws == draws + 1);
            assert(first_draw_captures == 1 && first_draw_target.valid);   // read back once, after the submit holding the first draw
            dsubmit(queue, 1, &draw); assert(host_draws == draws + 2 && first_draw_captures == 1);
            const struct wonder_host_draw *d = &last_draw;
            assert(d->vs == (void *)0xf000 && d->fs == (void *)0xf001 && d->vr->stage == 0 && d->fr->stage == 1);
            assert(d->primitive == 4 && d->index_type == 1 && d->count == 6 && d->index.size == 12 && d->index.buffer);
            struct wonder_nvn_pool_view iv; wonder_nvn_address_span((uintptr_t)(cb_memory + 0x9200), 12, &iv);
            assert(d->index.offset == iv.host_offset);
            assert(d->nattr == 1 && d->attr[0].format == 0x2c && d->attr[0].offset == 8 && d->attr[0].stream == 0);
            assert((d->stream_mask & 1) && d->stream[0].stride == 20 && d->stream[0].span.size == 0x100);
            assert(d->ubo[1][3].size == 0x100 && d->ubo[1][3].buffer);
            assert((d->tex_mask[1] & 1) && d->tex[1][0].texture.texture.desc.width == 128 && d->tex[1][0].sampler.valid);
            struct wonder_nvn_pool_view tv; const struct wonder_nvn_texture *rt = &d->tex[1][0].texture.texture;
            wonder_nvn_pool_span(rt->pool, (uint64_t)rt->offset, rt->size, &tv);
            assert(d->tex[1][0].storage == tv.memory && d->tex[1][0].storage_size == rt->size && rt->size);
            assert(d->color_count == 0 || d->targets[0].valid || !d->targets[0].valid);   // targets copied from replay state
            int depth_mode, origin; wonder_nvn_device_modes(&depth_mode, &origin); assert(d->depth_mode == depth_mode);
            // Missing bindings stop by name.
            const char *fs_bad = "stage ps\ncbuf 5 1\nfout 0 float\n";
            assert(!wonder_shader_refl_parse(fs_bad, strlen(fs_bad), &fake_refl[1], err, sizeof err));
            EXPECT_STOP(dsubmit(queue, 1, &draw), "unbound uniform buffer stage 1 slot 5");
            const char *fs_tex = "stage ps\ntex 7 1 1 2d\nfout 0 float\n";
            assert(!wonder_shader_refl_parse(fs_tex, strlen(fs_tex), &fake_refl[1], err, sizeof err));
            EXPECT_STOP(dsubmit(queue, 1, &draw), "unbound texture stage 1 slot 7");
            const char *fs_drv = "stage ps\ndrv 2\nfout 0 float\n";
            assert(!wonder_shader_refl_parse(fs_drv, strlen(fs_drv), &fake_refl[1], err, sizeof err));
            EXPECT_STOP(dsubmit(queue, 1, &draw), "driver constant segment");
            const char *vs_far = "stage vs\ndepth minus_one\nattr 0 3 float\n";
            fake_refl[1] = fs_parsed; assert(!wonder_shader_refl_parse(vs_far, strlen(vs_far), &fake_refl[0], err, sizeof err));
            unsigned before_far = host_draws; dsubmit(queue, 1, &draw); assert(host_draws == before_far + 1);   // disabled attribute: Maxwell reads (0,0,0,1)
            assert(last_draw.nattr == 1 && last_draw.attr[0].stream == WONDER_HOST_DEFAULT_ATTRIBUTE);
            assert(!wonder_shader_refl_parse(vs_refl, strlen(vs_refl), &fake_refl[0], err, sizeof err));
            // ---- Review fixes: state the Metal path cannot honour stops by name (final review Important 3-7) ----
            void (*rbegin)(void *) = slot("nvnCommandBufferBeginRecording"); uint64_t (*rend)(void *) = slot("nvnCommandBufferEndRecording");
            void (*rdraw)(void *, int, int, int, uint64_t) = slot("nvnCommandBufferDrawElements");
            #define REVIEW_RECORD(setup) (rbegin(dcb), setup, rdraw(dcb, 4, 1, 6, (uintptr_t)(cb_memory + 0x9200)), rend(dcb))
            #define REVIEW_STOP(setup, expect) do { uint64_t h_ = REVIEW_RECORD(setup); EXPECT_STOP(dsubmit(queue, 1, &h_), expect); } while (0)
            #define REVIEW_OK(setup) do { uint64_t h_ = REVIEW_RECORD(setup); unsigned n_ = host_draws; dsubmit(queue, 1, &h_); assert(host_draws == n_ + 1); } while (0)
            REVIEW_OK((void)0);   // baseline still executes
            // Depth mode must match the build (NVN default NEAR_IS_MINUS_W when never set).
            const char *vs_zero = "stage vs\ndepth zero\nattr 0 0 float\n";
            assert(!wonder_shader_refl_parse(vs_zero, strlen(vs_zero), &fake_refl[0], err, sizeof err));
            REVIEW_STOP((void)0, "depth mode");
            const char *vs_nodepth = "stage vs\nattr 0 0 float\n";
            assert(!wonder_shader_refl_parse(vs_nodepth, strlen(vs_nodepth), &fake_refl[0], err, sizeof err));
            REVIEW_STOP((void)0, "depth mode");
            assert(!wonder_shader_refl_parse(vs_refl, strlen(vs_refl), &fake_refl[0], err, sizeof err));
            // Window origin other than UPPER_LEFT.
            wonder_nvnDeviceSetWindowOriginMode(device, 0); REVIEW_STOP((void)0, "window origin"); wonder_nvnDeviceSetWindowOriginMode(device, 1);
            // Program bound for a subset of stages.
            REVIEW_STOP(((void (*)(void *, const void *, int))slot("nvnCommandBufferBindProgram"))(dcb, program_memory, 0x1), "program stage bits");
            ((void (*)(void *))slot("nvnCommandBufferBeginRecording"))(dcb);
            ((void (*)(void *, const void *, int))slot("nvnCommandBufferBindProgram"))(dcb, program_memory, 0x11);
            uint64_t rebind = ((uint64_t (*)(void *))slot("nvnCommandBufferEndRecording"))(dcb); dsubmit(queue, 1, &rebind);
            // Rasterizer discard, depth clamp, sample mask, polygon offset: recorded and refused.
            void (*u32cmd)(void *, uint32_t);
            u32cmd = slot("nvnCommandBufferSetRasterizerDiscard"); REVIEW_STOP(u32cmd(dcb, 1), "rasterizer discard"); REVIEW_OK(u32cmd(dcb, 0));
            u32cmd = slot("nvnCommandBufferSetDepthClamp"); REVIEW_STOP(u32cmd(dcb, 1), "depth clamp"); REVIEW_OK(u32cmd(dcb, 0));
            void (*mask)(void *, int) = slot("nvnCommandBufferSetSampleMask"); REVIEW_STOP(mask(dcb, 1), "sample mask"); REVIEW_OK(mask(dcb, -1));
            unsigned char poly[4]; ((void (*)(void *))slot("nvnPolygonStateSetDefaults"))(poly);
            ((void (*)(void *, int))slot("nvnPolygonStateSetPolygonOffsetEnables"))(poly, 4);
            REVIEW_STOP(((void (*)(void *, const void *))slot("nvnCommandBufferBindPolygonState"))(dcb, poly), "polygon offset");
            ((void (*)(void *, int))slot("nvnPolygonStateSetPolygonOffsetEnables"))(poly, 0);
            REVIEW_OK(((void (*)(void *, const void *))slot("nvnCommandBufferBindPolygonState"))(dcb, poly));
            // Primitive restart: an index equal to the restart value (index 5 here) cannot be honoured on a list.
            uint16_t *idx = (uint16_t *)(cb_memory + 0x9200); uint16_t saved[6]; memcpy(saved, idx, sizeof saved);
            for (int i = 0; i < 6; i++) idx[i] = (uint16_t)i;
            void (*restart)(void *, uint32_t, int) = slot("nvnCommandBufferSetPrimitiveRestart");
            REVIEW_STOP(restart(dcb, 1, 5), "primitive restart"); REVIEW_OK(restart(dcb, 1, 0xffff)); REVIEW_OK(restart(dcb, 0, 0));
            memcpy(idx, saved, sizeof saved);
            // Line width other than 1 on line primitives.
            rbegin(dcb); ((void (*)(void *, float))slot("nvnCommandBufferSetLineWidth"))(dcb, 2.0f); rdraw(dcb, 1, 1, 6, (uintptr_t)(cb_memory + 0x9200));
            uint64_t lines = rend(dcb); EXPECT_STOP(dsubmit(queue, 1, &lines), "line width");
            rbegin(dcb); ((void (*)(void *, float))slot("nvnCommandBufferSetLineWidth"))(dcb, 1.0f); uint64_t reset = rend(dcb); dsubmit(queue, 1, &reset);
            // Vertex stream 15 would land on [[buffer(31)]].
            unsigned char far_attrib[4]; ((void (*)(void *))slot("nvnVertexAttribStateSetDefaults"))(far_attrib);
            ((void (*)(void *, int, ptrdiff_t))slot("nvnVertexAttribStateSetFormat"))(far_attrib, 0x2c, 8);
            ((void (*)(void *, int))slot("nvnVertexAttribStateSetStreamIndex"))(far_attrib, 15);
            unsigned char streams16[16 * 8]; for (int i = 0; i < 16; i++) { ((void (*)(void *))slot("nvnVertexStreamStateSetDefaults"))(streams16 + 8 * i); ((void (*)(void *, ptrdiff_t))slot("nvnVertexStreamStateSetStride"))(streams16 + 8 * i, 20); }
            REVIEW_STOP((((void (*)(void *, int, const void *))slot("nvnCommandBufferBindVertexAttribState"))(dcb, 1, far_attrib),
                         ((void (*)(void *, int, const void *))slot("nvnCommandBufferBindVertexStreamState"))(dcb, 16, streams16),
                         ((void (*)(void *, int, uint64_t, size_t))slot("nvnCommandBufferBindVertexBuffer"))(dcb, 15, (uintptr_t)(cb_memory + 0x9000), 0x100)), "vertex stream 15");
            REVIEW_OK((((void (*)(void *, int, const void *))slot("nvnCommandBufferBindVertexAttribState"))(dcb, 1, attrib),
                         ((void (*)(void *, int, const void *))slot("nvnCommandBufferBindVertexStreamState"))(dcb, 1, stream)));
            // Conditional rendering: draw if the counters at address and address+16 are equal (mode 0) / not equal (mode 1),
            // evaluated after the GPU work recorded so far has completed (round80).
            { uint64_t *cnt = (uint64_t *)(cb_memory + 0x9400); cnt[0] = 5; cnt[1] = 0; cnt[2] = 5; cnt[3] = 0;
              void (*cond)(void *, int, uint64_t) = slot("nvnCommandBufferSetRenderEnableConditional");
              void (*enable)(void *, uint32_t) = slot("nvnCommandBufferSetRenderEnable");
              REVIEW_OK(cond(dcb, 0, (uintptr_t)cnt));                                   // equal, render-if-equal: drawn
              uint64_t h_ = REVIEW_RECORD(cond(dcb, 1, (uintptr_t)cnt)); unsigned n_ = host_draws;
              dsubmit(queue, 1, &h_); assert(host_draws == n_);                          // equal, render-if-not-equal: skipped
              cnt[2] = 6; REVIEW_OK(cond(dcb, 1, (uintptr_t)cnt));                       // now different: drawn
              REVIEW_OK(enable(dcb, 1)); }
            // Vertex fetch beyond the bound vertex buffer (stream 0: 0x100 bytes, stride 20, attribute RGBA16UI at offset 8):
            // index 12 reads bytes 248..255 (in range), index 13 reads 268..275 (past the end) -> stop instead of a GPU fault.
            { uint16_t *ix = (uint16_t *)(cb_memory + 0x9200); uint16_t keep[6]; memcpy(keep, ix, sizeof keep);
              for (int i = 0; i < 6; i++) ix[i] = 12; REVIEW_OK((void)0);
              ix[3] = 13; REVIEW_OK((void)0);   // past the bound size: Maxwell reads zeros -> the stream is zero-padded (round82)
              assert(last_draw.stream[0].span.size == 0x100 && last_draw.stream[0].fetch_size == 13 * 20 + 8 + 8);
              memcpy(ix, keep, sizeof keep); }
            // Bindless texture: the handle (texture 300 | sampler 1 << 20) is read from uniform slot 3 at byte 0x10 (round86).
            { uint32_t *ubo = (uint32_t *)(cb_memory + 0x9100); uint32_t keep = ubo[4]; ubo[4] = 300u | 1u << 20;
              const char *fs_bl = "stage ps\ncbuf 3 1\nbtex 3 16 2 2 2d\nfout 0 float\n";
              assert(!wonder_shader_refl_parse(fs_bl, strlen(fs_bl), &fake_refl[1], err, sizeof err));
              REVIEW_OK((void)0);
              assert((last_draw.tex_mask[1] >> 32 & 1) && last_draw.tex[1][32].texture.texture.desc.width == 128 && last_draw.tex[1][32].sampler.valid);
              const char *fs_bl2 = "stage ps\nbtex 5 16 2 2 2d\nfout 0 float\n";   // uniform slot 5 is not bound
              assert(!wonder_shader_refl_parse(fs_bl2, strlen(fs_bl2), &fake_refl[1], err, sizeof err));
              REVIEW_STOP((void)0, "bindless texture");
              // Handle written by UpdateUniformBuffer earlier in the same submit: the replay finishes the GPU work recorded so
              // far (the fake host applies pending writes on flush) and then reads it (round87).
              assert(!wonder_shader_refl_parse(fs_bl, strlen(fs_bl), &fake_refl[1], err, sizeof err));
              ubo[4] = 0xdead; uint32_t handle_now = 300u | 1u << 20;
              REVIEW_OK(((void (*)(void *, uint64_t, size_t, ptrdiff_t, size_t, const void *))slot("nvnCommandBufferUpdateUniformBuffer"))(dcb, (uintptr_t)(cb_memory + 0x9100), 0x100, 16, 4, &handle_now));
              assert(last_draw.tex[1][32].texture.texture.desc.width == 128);
              ubo[4] = keep; fake_refl[1] = fs_parsed; }
            // Storage buffers: refl slot 2 of the fragment stage bound with BindStorageBuffer (round89).
            { const char *fs_sb = "stage ps\nsbuf 2 3 r\nfout 0 float\n";
              assert(!wonder_shader_refl_parse(fs_sb, strlen(fs_sb), &fake_refl[1], err, sizeof err));
              REVIEW_STOP((void)0, "unbound storage buffer stage 1 slot 2");
              REVIEW_OK(((void (*)(void *, int, int, uint64_t, size_t))slot("nvnCommandBufferBindStorageBuffer"))(dcb, 1, 2, (uintptr_t)(cb_memory + 0x9800), 0x40));
              assert(last_draw.sbuf[1][2].size == 0x40 && last_draw.sbuf[1][2].buffer);
              fake_refl[1] = fs_parsed; }
            // Shader texture type must match the bound texture's target (texture 300 is 2D).
            const char *fs_array = "stage ps\ncbuf 3 1\ntex 0 1 1 2darray\nfout 0 float\n";
            assert(!wonder_shader_refl_parse(fs_array, strlen(fs_array), &fake_refl[1], err, sizeof err));
            REVIEW_STOP((void)0, "texture type");
            fake_refl[1] = fs_parsed;
            // A failed GPU command buffer stops the next submission by name.
            fake_gpu_error = "Internal Error (0000000e:Internal Error)"; REVIEW_STOP((void)0, "NVN_GPU_ERROR"); fake_gpu_error = NULL;
            REVIEW_OK((void)0);
            #undef REVIEW_RECORD
            #undef REVIEW_STOP
            #undef REVIEW_OK
            // Index range past the end of the pool.
            ((void (*)(void *))slot("nvnCommandBufferBeginRecording"))(dcb);
            ((void (*)(void *, int, int, int, uint64_t))slot("nvnCommandBufferDrawElements"))(dcb, 4, 1, 6, (uintptr_t)(cb_memory + sizeof cb_memory - 4));
            uint64_t overrun = ((uint64_t (*)(void *))slot("nvnCommandBufferEndRecording"))(dcb);
            EXPECT_STOP(dsubmit(queue, 1, &overrun), "NVN_INVALID_ARGUMENT");
            shaders_available = 0;
        }
        ((void (*)(void *))slot("nvnProgramFinalize"))(program_memory);
        EXPECT_STOP(wonder_nvn_program_snapshot(program_memory, &shaders), "NVN_INVALID_OBJECT");
        // Device helpers: release-driver debug callback no-op, image handle sxtw, ns timestamps.
        ((void (*)(const void *, void *, void *, uint32_t))slot("nvnDeviceInstallDebugCallback"))(device, (void *)1, NULL, 1);
        assert(((uint64_t (*)(const void *, int))slot("nvnDeviceGetImageHandle"))(device, 77) == 77);
        uint64_t counter[2] = {0, 123456789};
        assert(((uint64_t (*)(const void *, const void *))slot("nvnDeviceGetTimestampInNanoseconds"))(device, counter) == 123456789);
        // Sampler getters reflect the initialized state.
        int a, b, c; float lo, hi, border[4];
        ((void (*)(const void *, int *, int *))slot("nvnSamplerGetMinMagFilter"))(sampler, &a, &b);
        assert(a == 3 && b == 1);   // LINEAR_MIPMAP_NEAREST / LINEAR as initialized (round54: main+0x52648 reads it back)
        ((void (*)(const void *, int *, int *, int *))slot("nvnSamplerGetWrapMode"))(sampler, &a, &b, &c);
        ((void (*)(const void *, float *, float *))slot("nvnSamplerGetLodClamp"))(sampler, &lo, &hi);
        ((void (*)(const void *, int *, int *))slot("nvnSamplerGetCompare"))(sampler, &a, &b);
        ((void (*)(const void *, float *))slot("nvnSamplerGetBorderColor"))(sampler, border);
        (void)((float (*)(const void *))slot("nvnSamplerGetLodBias"))(sampler);
        assert(((float (*)(const void *))slot("nvnSamplerGetMaxAnisotropy"))(sampler) >= 1.0f);
        (void)((int (*)(const void *))slot("nvnSamplerGetReductionFilter"))(sampler);
        ((void (*)(void *))slot("nvnSamplerFinalize"))(sampler);
        EXPECT_STOP(((void (*)(void *))slot("nvnSamplerFinalize"))(device), "NVN_INVALID_OBJECT");
        // View getters return the stored fields and whether they were set.
        unsigned char gv[16]; ((void (*)(void *))slot("nvnTextureViewSetDefaults"))(gv);
        assert(((uint32_t (*)(const void *, int *, int *))slot("nvnTextureViewGetLevels"))(gv, &a, &b) == 0);
        ((void (*)(void *, int, int))slot("nvnTextureViewSetLevels"))(gv, 2, 3);
        assert(((uint32_t (*)(const void *, int *, int *))slot("nvnTextureViewGetLevels"))(gv, &a, &b) == 1 && a == 2 && b == 3);
        assert(((uint32_t (*)(const void *, int *, int *))slot("nvnTextureViewGetLayers"))(gv, &a, &b) == 0);
        assert(((uint32_t (*)(const void *, int *))slot("nvnTextureViewGetTarget"))(gv, &a) == 0);
        // Buffer flush/invalidate forward to the pool range check; Finalize keeps pool storage.
        ((void (*)(const void *, ptrdiff_t, size_t))slot("nvnBufferFlushMappedRange"))(buffer, 0, 0x20);
        ((void (*)(const void *, ptrdiff_t, size_t))slot("nvnBufferInvalidateMappedRange"))(buffer, 0x10, 0x10);
        EXPECT_STOP(((void (*)(const void *, ptrdiff_t, size_t))slot("nvnBufferFlushMappedRange"))(buffer, 0x10, 0x20), "NVN_INVALID_ARGUMENT");
        ((void (*)(void *))slot("nvnBufferFinalize"))(gpu_only);
        // Pool finalize: a pool backing a virtual mapping cannot go; an unreferenced pool releases its Metal view.
        EXPECT_STOP(((void (*)(void *))slot("nvnMemoryPoolFinalize"))(phys_pool), "still backs virtual pool");
        unsigned before = released_resources;
        ((void (*)(void *))slot("nvnMemoryPoolFinalize"))(wt_pool);
        assert(released_resources == before + 1);
        EXPECT_STOP(((int (*)(const void *))slot("nvnMemoryPoolGetFlags"))(wt_pool), "NVN_INVALID_OBJECT");
    }
    puts("NVN_STRICT_BOOTSTRAP_OK");
}
