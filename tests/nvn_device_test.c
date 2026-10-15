#define _DEFAULT_SOURCE   /* MAP_ANON on glibc; Darwin has it by default */
// Host test of the strict NVN bootstrap: Wonder's actual call order (main+0x5bb8b4..0x5bb970).
#include "nvn_device.h"
#include "nvn_names.h"
#include "nvn_texture.h"
#include "nvn_commands.h"
#include <assert.h>
#include <setjmp.h>
#include <stdarg.h>
#include <stdio.h>
#include <string.h>
#include <stdlib.h>
static jmp_buf stopped; static char last_stop[512]; static int host_ready, expecting_stop;
void wonder_nvn_report(const char *format, ...) { va_list a; va_start(a, format); vfprintf(stdout, format, a); va_end(a); fputc('\n', stdout); }
_Noreturn void wonder_nvn_stop(const char *format, ...) { va_list a; va_start(a, format); vsnprintf(last_stop, sizeof last_stop, format, a); va_end(a); if (!expecting_stop) { fprintf(stderr, "unexpected stop: %s\n", last_stop); abort(); } longjmp(stopped, 1); }
static void *last_labeled_resource; static const char *last_resource_label;
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
void *wonder_host_pool_buffer(void *memory, size_t size, size_t *offset) { assert(memory && size && offset); *offset = 0; buffers++; return (void *)(uintptr_t)(0xb0000 + buffers); }
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
int main(void) {
    assert(WONDER_NVN_API_MAJOR == 55 && WONDER_NVN_API_MINOR_MIN == 15 && WONDER_NVN_NAME_COUNT == 534);
    // Bootstrap gives exactly the two names Wonder asks for; unknown names are NULL.
    assert(wonder_nvn_resolve("nvnDeviceInitialize", "nvnBootstrapLoader") == (void *)wonder_nvnDeviceInitialize);
    assert(wonder_nvn_resolve("nvnDeviceGetProcAddress", "nvnBootstrapLoader") == (void *)wonder_nvnDeviceGetProcAddress);
    assert(!wonder_nvn_resolve("nvnBootstrapLoader", "test") && !wonder_nvn_resolve("nvnNotAFunction", "test") && !wonder_nvn_resolve(NULL, "test"));
    loader_pass(NULL);
    int thunks = 0;
    for (int i = 0; i < WONDER_NVN_NAME_COUNT; i++) thunks += (uintptr_t)slots[i] == 0x100000 + 8 * (uintptr_t)i;
    assert(wonder_nvn_implemented_count() == 165 && thunks == WONDER_NVN_NAME_COUNT - 165);
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
    EXPECT_STOP(wonder_nvnWindowAcquireTexture(window, sync, &index), "NVN_UNSUPPORTED");   // all held; present not implemented
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
    EXPECT_STOP(add_control(cmdbuf, control, 128), "NVN_INVALID_ARGUMENT");
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
    cmdbuf[0x11] = 1;
    EXPECT_STOP(add_command(cmdbuf, memory_pool, 0, 512), "NVN_UNSUPPORTED");
    cmdbuf[0x11] = 0;



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
    ((void (*)(void *))slot("nvnTextureFinalize"))(gtex);
    EXPECT_STOP(wonder_nvnTextureGetWidth(gtex), "NVN_INVALID_OBJECT");
    assert(wonder_nvn_texture_lookup(texture_pool, 300)->texture.desc.width == 128);  // registration keeps its snapshot
    // State objects: defaults + setters, validated ranges.
    uint64_t blend = 0, ds = 0, vstream = 0; uint32_t color = 0, mask = 0, poly = 0, vattr = 0; unsigned char ms[0x18];
    ((void (*)(void *))slot("nvnBlendStateSetDefaults"))(&blend); ((void (*)(void *, int, int, int, int))slot("nvnBlendStateSetBlendFunc"))(&blend, 5, 6, 2, 1);
    EXPECT_STOP(wonder_nvnBlendStateSetBlendFunc(&blend, 0, 1, 1, 1), "NVN_INVALID_ARGUMENT");
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
    puts("NVN_STRICT_BOOTSTRAP_OK");
}
