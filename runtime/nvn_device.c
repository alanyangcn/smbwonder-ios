// Strict NVN device bootstrap for Wonder (main loader 0x1080ff0, version gate 0x5bb95c).
// Host objects live inside the guest-owned NVN storage; a magic word guards every use.
#include "nvn_device.h"
#include "nvn_names.h"
#include <stdlib.h>
#include <string.h>
#define BUILDER_MAGIC 0x424456576e6f57ull   /* "WonVDB" */
#define DEVICE_MAGIC  0x564544576e6f57ull   /* "WonDEV" */
struct builder { uint64_t magic; int32_t flags; };
#define QBUILDER_MAGIC 0x42515578u    /* queue builder */
#define QUEUE_MAGIC 0x55455551u       /* "QUEU" */
#define FAST_CLEAR_MAX 16
struct fast_clear { int32_t kind, format; uint32_t value[4]; };   // kind 1 float, 2 int, 3 uint, 4 depth
struct device { uint64_t magic; int32_t flags; int32_t api_major, api_minor; int32_t window_origin, depth_mode;
                int32_t fast_clears; struct fast_clear fast_clear[FAST_CLEAR_MAX]; };
// NVNqueueBuilder is 64 bytes; Wonder reuses the device-builder stack slot (main+0x5bba18).
struct queue_builder { uint32_t magic, flags; const void *device; uint32_t command, compute, control, flush; void *memory; uint64_t memory_size; };
struct queue { uint32_t magic, flags; const void *device; void *memory; uint64_t memory_size; uint32_t command, compute, control, submits; };
_Static_assert(sizeof(struct queue_builder) <= WONDER_NVN_QUEUE_BUILDER_SIZE, "queue builder storage");
_Static_assert(sizeof(struct queue) <= WONDER_NVN_QUEUE_SIZE, "queue storage");
_Static_assert(sizeof(struct builder) <= WONDER_NVN_DEVICE_BUILDER_SIZE, "builder storage");
_Static_assert(sizeof(struct device) <= WONDER_NVN_DEVICE_SIZE, "device storage");
_Static_assert(WONDER_NVN_API_MINOR_REPORTED >= WONDER_NVN_API_MINOR_MIN, "minor below game gate");

static const struct builder *builder_of(const void *p, const char *who) {
    const struct builder *b = p;
    if (!b || b->magic != BUILDER_MAGIC) wonder_nvn_stop("NVN_INVALID_OBJECT %s builder=%p (SetDefaults not called)", who, p);
    return b;
}
const void *wonder_nvn_device_check(const void *p, const char *who);
static const struct device *device_of(const void *p, const char *who) {
    const struct device *d = p;
    if (!d || d->magic != DEVICE_MAGIC) wonder_nvn_stop("NVN_INVALID_OBJECT %s device=%p (not initialized)", who, p);
    return d;
}

void wonder_nvnDeviceBuilderSetDefaults(void *builder) {
    if (!builder) wonder_nvn_stop("NVN_INVALID_OBJECT nvnDeviceBuilderSetDefaults builder=NULL");
    memset(builder, 0, WONDER_NVN_DEVICE_BUILDER_SIZE);
    ((struct builder *)builder)->magic = BUILDER_MAGIC;
    wonder_nvn_report("NVN_CALL nvnDeviceBuilderSetDefaults builder=%p", builder);
}
void wonder_nvnDeviceBuilderSetFlags(void *builder, int flags) {
    builder_of(builder, "nvnDeviceBuilderSetFlags");
    // Debug-layer / GPU-crash-dump flags need a debug layer this host does not have.
    if (flags) wonder_nvn_stop("NVN_UNSUPPORTED nvnDeviceBuilderSetFlags flags=%#x (debug layer not implemented)", (unsigned)flags);
    ((struct builder *)builder)->flags = flags;
    wonder_nvn_report("NVN_CALL nvnDeviceBuilderSetFlags flags=%#x", (unsigned)flags);
}
uint32_t wonder_nvnDeviceInitialize(void *device, const void *builder) {
    const struct builder *b = builder_of(builder, "nvnDeviceInitialize");
    if (!device) wonder_nvn_stop("NVN_INVALID_OBJECT nvnDeviceInitialize device=NULL");
    if (!wonder_host_driver_ready())
        wonder_nvn_stop("NVN_DEVICE_HOST_UNAVAILABLE nvnDeviceInitialize: host Metal device/queue/layer not established");
    struct device *d = device;
    memset(d, 0, sizeof *d);
    d->flags = b->flags; d->api_major = WONDER_NVN_API_MAJOR; d->api_minor = WONDER_NVN_API_MINOR_REPORTED;
    d->magic = DEVICE_MAGIC;
    wonder_nvn_report("NVN_DEVICE_READY device=%p api=%d.%d flags=%#x host=metal2", device, d->api_major, d->api_minor, (unsigned)d->flags);
    return 1;
}

// NVN_DEVICE_INFO values 0x02..0x5F: Tegra X1 values measured on SMO for D2R (dm/gfx/nvn/hle_nvn.c).
// NVN device-info enumerators are append-only; values beyond 0x5F are not known and stop.
// API version 0/1 is this host's own 55.15 declaration, satisfying the main+0x5bb95c gate.
static const int device_info[0x60] = {
    [0x00] = WONDER_NVN_API_MAJOR, [0x01] = WONDER_NVN_API_MINOR_REPORTED,
    [0x02] = 14, [0x03] = 65536, [0x04] = 256, [0x05] = 8, [0x06] = 16, [0x07] = 4, [0x08] = 16, [0x09] = 32,
    [0x0A] = 16, [0x0B] = 4, [0x0C] = 4, [0x0D] = 4, [0x0E] = 16, [0x0F] = 32, [0x10] = 32, [0x11] = 256, [0x12] = 256,
    [0x13] = 4, [0x14] = 8, [0x15] = 512, [0x16] = 256, [0x17] = 2, [0x18] = 256, [0x19] = 4096, [0x1A] = 131072,
    [0x1B] = 16, [0x1C] = 1536, [0x1D] = 1024, [0x1E] = 64, [0x1F] = 1536, [0x20] = 65535, [0x21] = 65535, [0x22] = 65535,
    [0x23] = 8, [0x24] = 1048576, [0x25] = 4096, [0x26] = 16, [0x27] = 4096, [0x28] = 1, [0x29] = 1, [0x2A] = 1,
    [0x2B] = 262144, [0x2C] = 15, [0x2D] = 2048, [0x2E] = 1, [0x2F] = 1, [0x30] = 14, [0x31] = 5, [0x32] = 1, [0x33] = 8,
    [0x34] = 8, [0x35] = 4, [0x36] = 32, [0x37] = 128, [0x38] = 32, [0x39] = 128, [0x3A] = 65536, [0x3B] = 1, [0x3C] = 4,
    [0x3D] = 16384, [0x3E] = 134217728, [0x3F] = 2048, [0x40] = 16384, [0x41] = 16384, [0x42] = 1, [0x43] = 1, [0x44] = 1,
    [0x45] = 1, [0x46] = 5, [0x47] = 1, [0x48] = 1, [0x49] = 1, [0x4A] = 1, [0x4B] = 1, [0x4C] = 1, [0x4D] = 16,
    [0x4E] = 1024, [0x4F] = 32, [0x50] = 4096, [0x51] = 65536, [0x52] = 65536, [0x53] = 256, [0x54] = 16384,
    [0x55] = 262144, [0x56] = 4096, [0x57] = 1, [0x58] = 4, [0x59] = 2, [0x5A] = 0, [0x5B] = 4096, [0x5C] = 16384,
    [0x5D] = 4096, [0x5E] = 128, [0x5F] = 4096,
};
void wonder_nvnDeviceGetInteger(const void *device, int what, int *out) {
    device_of(device, "nvnDeviceGetInteger");
    if (!out) wonder_nvn_stop("NVN_INVALID_ARGUMENT nvnDeviceGetInteger what=%#x out=NULL", (unsigned)what);
    if (what < 0 || what >= (int)(sizeof device_info / sizeof *device_info))
        wonder_nvn_stop("NVN_UNIMPLEMENTED_DEVICE_INFO nvnDeviceGetInteger what=%#x", (unsigned)what);
    *out = device_info[what];
    wonder_nvn_report("NVN_CALL nvnDeviceGetInteger what=%#x -> %d", (unsigned)what, *out);
}

static int info(int what) { return device_info[what]; }
int wonder_nvn_info(int what) { return info(what); }
const void *wonder_nvn_device_check(const void *p, const char *who) { return device_of(p, who); }

// ---------- Device state (main+0x984e90 / 0x984ea4) ----------
void wonder_nvnDeviceSetWindowOriginMode(void *device, int mode) {
    struct device *d = (struct device *)device_of(device, "nvnDeviceSetWindowOriginMode");
    if (mode != 0 && mode != 1) wonder_nvn_stop("NVN_INVALID_ARGUMENT nvnDeviceSetWindowOriginMode mode=%d", mode);
    d->window_origin = mode;
    wonder_nvn_report("NVN_CALL nvnDeviceSetWindowOriginMode %s", mode ? "UPPER_LEFT (matches Metal)" : "LOWER_LEFT (GL; host must flip)");
}
void wonder_nvnDeviceSetDepthMode(void *device, int mode) {
    struct device *d = (struct device *)device_of(device, "nvnDeviceSetDepthMode");
    if (mode != 0 && mode != 1) wonder_nvn_stop("NVN_INVALID_ARGUMENT nvnDeviceSetDepthMode mode=%d", mode);
    d->depth_mode = mode;
    wonder_nvn_report("NVN_CALL nvnDeviceSetDepthMode %s", mode ? "NEAR_IS_ZERO (matches Metal)" : "NEAR_IS_MINUS_W (GL; host must remap)");
}

// ---------- Fast-clear registration (main+0x42367c) ----------
// TX1 keeps a table of colors that render-target compression can clear for free. The host has no such
// compression, so registration is bookkeeping only (it changes no result the game can observe); the table
// is kept so a later Metal path can use it, and a full table returns false like the console.
static uint32_t register_fast_clear(void *device, int kind, const void *value, int format, const char *who) {
    struct device *d = (struct device *)device_of(device, who);
    if (!value && kind != 4) wonder_nvn_stop("NVN_INVALID_ARGUMENT %s value=NULL", who);
    if (d->fast_clears >= FAST_CLEAR_MAX) return 0;
    struct fast_clear *f = &d->fast_clear[d->fast_clears++];
    f->kind = kind; f->format = format;
    if (kind != 4) memcpy(f->value, value, sizeof f->value); else memcpy(f->value, &format, sizeof(float));
    wonder_nvn_report("NVN_CALL %s format=%#x entries=%d", who, (unsigned)format, d->fast_clears);
    return 1;
}
uint32_t wonder_nvnDeviceRegisterFastClearColor(void *d, const float *c, int f) { return register_fast_clear(d, 1, c, f, "nvnDeviceRegisterFastClearColor"); }
uint32_t wonder_nvnDeviceRegisterFastClearColori(void *d, const int *c, int f) { return register_fast_clear(d, 2, c, f, "nvnDeviceRegisterFastClearColori"); }
uint32_t wonder_nvnDeviceRegisterFastClearColorui(void *d, const uint32_t *c, int f) { return register_fast_clear(d, 3, c, f, "nvnDeviceRegisterFastClearColorui"); }
uint32_t wonder_nvnDeviceRegisterFastClearDepth(void *d, float depth) {
    int bits; memcpy(&bits, &depth, sizeof bits); return register_fast_clear(d, 4, 0, bits, "nvnDeviceRegisterFastClearDepth");
}

// ---------- Queue (main+0x5bba14..0x5bbb78) ----------
static struct queue_builder *qbuilder_of(void *p, const char *who) {
    struct queue_builder *b = p;
    if (!b || b->magic != QBUILDER_MAGIC) wonder_nvn_stop("NVN_INVALID_OBJECT %s queue_builder=%p (SetDefaults not called)", who, p);
    return b;
}
// size==0 is allowed only where NVN permits omitting the region (compute memory).
static uint32_t queue_size(const char *who, size_t n, int min_info, int granularity_info, int zero_ok) {
    size_t min = (size_t)info(min_info), granularity = (size_t)info(granularity_info);
    if (!(zero_ok && n == 0) && (n < min || n % granularity || n > 0x40000000u))
        wonder_nvn_stop("NVN_INVALID_ARGUMENT %s size=%#zx (min %#zx, granularity %#zx)", who, n, min, granularity);
    return (uint32_t)n;
}
void wonder_nvnQueueBuilderSetDefaults(void *builder) {
    if (!builder) wonder_nvn_stop("NVN_INVALID_OBJECT nvnQueueBuilderSetDefaults builder=NULL");
    memset(builder, 0, WONDER_NVN_QUEUE_BUILDER_SIZE);
    struct queue_builder *b = builder;
    b->magic = QBUILDER_MAGIC; b->command = (uint32_t)info(0x52); b->compute = (uint32_t)info(0x55);
    b->control = (uint32_t)info(0x5C); b->flush = (uint32_t)info(0x56);
    wonder_nvn_report("NVN_CALL nvnQueueBuilderSetDefaults builder=%p", builder);
}
void wonder_nvnQueueBuilderSetDevice(void *builder, const void *device) {
    struct queue_builder *b = qbuilder_of(builder, "nvnQueueBuilderSetDevice");
    b->device = device_of(device, "nvnQueueBuilderSetDevice");
}
void wonder_nvnQueueBuilderSetFlags(void *builder, int flags) {
    struct queue_builder *b = qbuilder_of(builder, "nvnQueueBuilderSetFlags");
    // NO_FRAGMENT_INTERLOCK (1) / NO_ZCULL (2) only disable features; the host never provides either.
    if (flags & ~3) wonder_nvn_stop("NVN_UNSUPPORTED nvnQueueBuilderSetFlags flags=%#x", (unsigned)flags);
    b->flags = (uint32_t)flags;
    wonder_nvn_report("NVN_CALL nvnQueueBuilderSetFlags flags=%#x", (unsigned)flags);
}
void wonder_nvnQueueBuilderSetCommandMemorySize(void *builder, size_t n) {
    qbuilder_of(builder, "nvnQueueBuilderSetCommandMemorySize")->command = queue_size("nvnQueueBuilderSetCommandMemorySize", n, 0x51, 0x50, 0);
}
void wonder_nvnQueueBuilderSetComputeMemorySize(void *builder, size_t n) {
    qbuilder_of(builder, "nvnQueueBuilderSetComputeMemorySize")->compute = queue_size("nvnQueueBuilderSetComputeMemorySize", n, 0x54, 0x53, 1);
}
void wonder_nvnQueueBuilderSetControlMemorySize(void *builder, size_t n) {
    qbuilder_of(builder, "nvnQueueBuilderSetControlMemorySize")->control = queue_size("nvnQueueBuilderSetControlMemorySize", n, 0x5B, 0x5D, 0);
}
// Host-defined layout: the three regions back to back, rounded to 4 KiB. The host keeps no GPU state
// in it yet; the game owns and frees it.
static size_t queue_memory_need(const struct queue_builder *b) { return ((size_t)b->command + b->compute + b->control + 0xfff) & ~(size_t)0xfff; }
size_t wonder_nvnQueueBuilderGetQueueMemorySize(const void *builder) {
    const struct queue_builder *b = qbuilder_of((void *)builder, "nvnQueueBuilderGetQueueMemorySize");
    size_t size = queue_memory_need(b);
    wonder_nvn_report("NVN_CALL nvnQueueBuilderGetQueueMemorySize command=%#x compute=%#x control=%#x -> %#zx", b->command, b->compute, b->control, size);
    return size;
}
void wonder_nvnQueueBuilderSetQueueMemory(void *builder, void *memory, size_t size) {
    struct queue_builder *b = qbuilder_of(builder, "nvnQueueBuilderSetQueueMemory");
    size_t need = queue_memory_need(b);
    if (!memory || ((uintptr_t)memory & 0xfff) || size < need)
        wonder_nvn_stop("NVN_INVALID_ARGUMENT nvnQueueBuilderSetQueueMemory memory=%p size=%#zx (need 4 KiB aligned, >= %#zx)", memory, size, need);
    b->memory = memory; b->memory_size = size;
}
uint32_t wonder_nvnQueueInitialize(void *queue, const void *builder) {
    const struct queue_builder *b = qbuilder_of((void *)builder, "nvnQueueInitialize");
    if (!queue) wonder_nvn_stop("NVN_INVALID_OBJECT nvnQueueInitialize queue=NULL");
    if (!b->device) wonder_nvn_stop("NVN_INVALID_ARGUMENT nvnQueueInitialize: builder has no device");
    device_of(b->device, "nvnQueueInitialize");
    if (!b->memory) wonder_nvn_stop("NVN_UNSUPPORTED nvnQueueInitialize: driver-allocated queue memory not implemented");
    if (!wonder_host_driver_ready()) wonder_nvn_stop("NVN_DEVICE_HOST_UNAVAILABLE nvnQueueInitialize: host Metal queue not established");
    struct queue *q = queue;
    memset(q, 0, sizeof *q);
    *q = (struct queue){QUEUE_MAGIC, b->flags, b->device, b->memory, b->memory_size, b->command, b->compute, b->control, 0};
    wonder_nvn_report("NVN_QUEUE_READY queue=%p device=%p flags=%#x memory=%p+%#llx host=persistent MTLCommandQueue (no submissions yet)",
                      queue, b->device, q->flags, q->memory, (unsigned long long)q->memory_size);
    return 1;
}

// ---------- Memory pools (main+0x7338f8..0x733964): GPU address = CPU address ----------
#define PBUILDER_MAGIC 0x42504d57u
#define POOL_MAGIC 0x4c4f4f50u          /* "POOL" */
struct pool_builder { uint32_t magic, flags; const void *device; void *memory; uint64_t size; };
struct virtual_mapping { const struct memory_pool *physical; uint64_t physical_offset, virtual_offset, size; int32_t storage_class; };
struct memory_pool { uint32_t magic, flags; const void *device; uint8_t *memory; uint64_t size; void *host_buffer; uint64_t host_offset;
                     struct virtual_mapping *mappings; uint32_t mapping_count, mapping_capacity; };
_Static_assert(sizeof(struct pool_builder) <= WONDER_NVN_BUILDER_SLOT, "pool builder slot");
_Static_assert(sizeof(struct memory_pool) <= WONDER_NVN_MEMORY_POOL_SIZE, "memory pool storage");
static struct pool_builder *pbuilder_of(void *p, const char *who) {
    struct pool_builder *b = p;
    if (!b || b->magic != PBUILDER_MAGIC) wonder_nvn_stop("NVN_INVALID_OBJECT %s pool_builder=%p (SetDefaults not called)", who, p);
    return b;
}
static const struct memory_pool *pool_of(const void *p, const char *who) {
    const struct memory_pool *m = p;
    if (!m || m->magic != POOL_MAGIC) wonder_nvn_stop("NVN_INVALID_OBJECT %s memory_pool=%p (not initialized)", who, p);
    return m;
}
void wonder_nvn_pool_check(const void *pool, const char *who, struct wonder_nvn_pool_view *view) {
    const struct memory_pool *m = pool_of(pool, who);
    *view = (struct wonder_nvn_pool_view){m->memory, m->size, m->flags, m->host_buffer, m->host_offset};
}
void wonder_nvn_pool_span(const void *pool, uint64_t offset, uint64_t size, struct wonder_nvn_pool_view *view) {
    const struct memory_pool *m = pool_of(pool, "pool_span");
    if (!size || offset > m->size || size > m->size - offset)
        wonder_nvn_stop("NVN_INVALID_ARGUMENT pool_span offset=%#llx size=%#llx", (unsigned long long)offset, (unsigned long long)size);
    if (m->flags & 0x200) {
        for (uint32_t i = 0; i < m->mapping_count; i++) {
            const struct virtual_mapping *mapping = &m->mappings[i];
            if (offset < mapping->virtual_offset || offset - mapping->virtual_offset >= mapping->size) continue;
            uint64_t relative = offset - mapping->virtual_offset;
            uint64_t available = mapping->size - relative;
            wonder_nvn_pool_span(mapping->physical, mapping->physical_offset + relative,
                                 size < available ? size : available, view);
            return;
        }
        wonder_nvn_stop("NVN_UNMAPPED pool_span virtual offset=%#llx", (unsigned long long)offset);
    }
    *view = (struct wonder_nvn_pool_view){m->memory + offset, size, m->flags, m->host_buffer, m->host_offset + offset};
}

void wonder_nvnMemoryPoolBuilderSetDefaults(void *builder) {
    if (!builder) wonder_nvn_stop("NVN_INVALID_OBJECT nvnMemoryPoolBuilderSetDefaults builder=NULL");
    memset(builder, 0, sizeof(struct pool_builder));   // only our bytes: the slot may border live locals
    struct pool_builder *b = builder;
    b->magic = PBUILDER_MAGIC; b->flags = 0x2 | 0x20;   // NVN default: CPU_UNCACHED | GPU_CACHED
}
void wonder_nvnMemoryPoolBuilderSetDevice(void *builder, const void *device) {
    pbuilder_of(builder, "nvnMemoryPoolBuilderSetDevice")->device = device_of(device, "nvnMemoryPoolBuilderSetDevice");
}
void wonder_nvnMemoryPoolBuilderSetFlags(void *builder, int flags) {
    struct pool_builder *b = pbuilder_of(builder, "nvnMemoryPoolBuilderSetFlags");
    unsigned f = (unsigned)flags, cpu = f & 7, gpu = f & 0x38;
    // Exactly one CPU and one GPU access mode; SHADER_CODE / COMPRESSIBLE are hints for real memory.
    // PHYSICAL (backs virtual pools) and VIRTUAL (sparse, no storage, CPU_NO_ACCESS) are exclusive.
    if ((cpu != 1 && cpu != 2 && cpu != 4) || (gpu != 8 && gpu != 0x10 && gpu != 0x20) || (f & ~0x3ffu) ||
        (f & 0x300) == 0x300 || ((f & 0x200) && cpu != 1))
        wonder_nvn_stop("NVN_UNSUPPORTED nvnMemoryPoolBuilderSetFlags flags=%#x", f);
    b->flags = f;
}
void wonder_nvnMemoryPoolBuilderSetStorage(void *builder, void *memory, size_t size) {
    struct pool_builder *b = pbuilder_of(builder, "nvnMemoryPoolBuilderSetStorage");
    // NVN_MEMORY_POOL_STORAGE_ALIGNMENT / GRANULARITY are both 4 KiB; virtual pools have no storage and are
    // sized in NVN_DEVICE_INFO_MEMORY_POOL_PAGE_SIZE (64 KiB) pages (main+0x42dd38 passes NULL).
    if (b->flags & 0x200) {
        if (memory || !size || size % (size_t)info(0x3A))
            wonder_nvn_stop("NVN_INVALID_ARGUMENT nvnMemoryPoolBuilderSetStorage virtual memory=%p size=%#zx (NULL, 64 KiB pages)", memory, size);
    } else if (!memory || ((uintptr_t)memory & 0xfff) || !size || (size & 0xfff))
        wonder_nvn_stop("NVN_INVALID_ARGUMENT nvnMemoryPoolBuilderSetStorage memory=%p size=%#zx (4 KiB aligned and granular)", memory, size);
    b->memory = memory; b->size = size;
}
uint32_t wonder_nvnMemoryPoolInitialize(void *pool, const void *builder) {
    const struct pool_builder *b = pbuilder_of((void *)builder, "nvnMemoryPoolInitialize");
    if (!pool) wonder_nvn_stop("NVN_INVALID_OBJECT nvnMemoryPoolInitialize pool=NULL");
    if (!b->device) wonder_nvn_stop("NVN_INVALID_ARGUMENT nvnMemoryPoolInitialize: builder has no device");
    if (b->flags & 0x200) {
        // Virtual pool: a GPU address range without storage. GPU address = CPU address still holds, so a
        // PROT_NONE host reservation gives it unique addresses; pages become meaningful through MapVirtual.
        void *range = wonder_host_reserve(b->size);
        if (!range) wonder_nvn_stop("NVN_HOST_RESERVE_FAILED nvnMemoryPoolInitialize virtual size=%#llx", (unsigned long long)b->size);
        struct memory_pool *m = pool;
        memset(m, 0, sizeof *m);
        *m = (struct memory_pool){POOL_MAGIC, b->flags, b->device, range, b->size, 0, 0, 0, 0, 0};
        wonder_nvn_report("NVN_POOL_READY pool=%p virtual range=%p size=%#llx flags=%#x", pool, range, (unsigned long long)b->size, b->flags);
        return 1;
    }
    if (!b->memory) wonder_nvn_stop("NVN_INVALID_ARGUMENT nvnMemoryPoolInitialize: no storage");
    size_t offset = 0;
    void *buffer = (b->flags & 0x8) ? 0 : wonder_host_pool_buffer(b->memory, b->size, &offset);   // GPU_NO_ACCESS needs no GPU view
    if (!(b->flags & 0x8) && !buffer)
        wonder_nvn_stop("NVN_HOST_BUFFER_FAILED nvnMemoryPoolInitialize memory=%p size=%#llx", b->memory, (unsigned long long)b->size);
    struct memory_pool *m = pool;
    memset(m, 0, sizeof *m);
    *m = (struct memory_pool){POOL_MAGIC, b->flags, b->device, b->memory, b->size, buffer, offset, 0, 0, 0};
    wonder_nvn_report("NVN_POOL_READY pool=%p memory=%p size=%#llx flags=%#x host_buffer=%p offset=%#zx", pool, b->memory,
                      (unsigned long long)b->size, b->flags, buffer, offset);
    return 1;
}

// NVNmappingRequest (public NVN layout; main+0x42dd88 fills x29-0x48): physical pool, physical offset,
// virtual offset, size, storage class.
struct mapping_request { const void *physical; int64_t physical_offset, virtual_offset; uint64_t size; int32_t storage_class; };
// The virtual range is an opaque GPU-address token. Resource consumers must use pool_span to reach
// physical storage, including aliasing/remapping; dereferencing pool_check().memory is forbidden for
// VIRTUAL pools. Metal texture-cache alias coherence still needs to be implemented at first GPU use.
uint32_t wonder_nvnMemoryPoolMapVirtual(void *pool, int count, const void *requests) {
    struct memory_pool *v = (struct memory_pool *)pool_of(pool, "nvnMemoryPoolMapVirtual");
    if (!(v->flags & 0x200)) wonder_nvn_stop("NVN_INVALID_ARGUMENT nvnMemoryPoolMapVirtual on a non-virtual pool flags=%#x", v->flags);
    if (count <= 0 || !requests) wonder_nvn_stop("NVN_INVALID_ARGUMENT nvnMemoryPoolMapVirtual count=%d", count);
    uint64_t page = (uint64_t)info(0x3A);
    const struct mapping_request *r = requests;
    for (int i = 0; i < count; i++) {
        if (r[i].virtual_offset < 0 || (uint64_t)r[i].virtual_offset % page || !r[i].size || r[i].size % page ||
            (uint64_t)r[i].virtual_offset > v->size || r[i].size > v->size - (uint64_t)r[i].virtual_offset)
            wonder_nvn_stop("NVN_INVALID_ARGUMENT nvnMemoryPoolMapVirtual[%d] virtual=%#llx size=%#llx pool=%#llx", i,
                            (unsigned long long)r[i].virtual_offset, (unsigned long long)r[i].size, (unsigned long long)v->size);
        const struct memory_pool *p = r[i].physical ? pool_of(r[i].physical, "nvnMemoryPoolMapVirtual physical") : 0;
        if (p && (!(p->flags & 0x100) || r[i].physical_offset < 0 || (uint64_t)r[i].physical_offset % page ||
                  (uint64_t)r[i].physical_offset > p->size || r[i].size > p->size - (uint64_t)r[i].physical_offset))
            wonder_nvn_stop("NVN_INVALID_ARGUMENT nvnMemoryPoolMapVirtual[%d] physical flags=%#x offset=%#llx size=%#llx", i,
                            p->flags, (unsigned long long)r[i].physical_offset, (unsigned long long)r[i].size);
    }
    for (int i = 0; i < count; i++) {
        // Rebuild the list: old mappings keep only their parts outside [lo, hi); then the new mapping is added
        // (a NULL physical pool just unmaps the range).
        uint64_t lo = (uint64_t)r[i].virtual_offset, hi = lo + r[i].size;
        uint32_t cap = v->mapping_count * 2 + 2;
        struct virtual_mapping *next = malloc(cap * sizeof *next); uint32_t n = 0;
        if (!next) wonder_nvn_stop("NVN_HOST_OUT_OF_MEMORY nvnMemoryPoolMapVirtual");
        for (uint32_t j = 0; j < v->mapping_count; j++) {
            struct virtual_mapping m = v->mappings[j]; uint64_t end = m.virtual_offset + m.size;
            if (end <= lo || m.virtual_offset >= hi) { next[n++] = m; continue; }
            if (m.virtual_offset < lo) { struct virtual_mapping left = m; left.size = lo - m.virtual_offset; next[n++] = left; }
            if (end > hi) { struct virtual_mapping right = m; uint64_t cut = hi - m.virtual_offset;
                            right.virtual_offset = hi; right.physical_offset += cut; right.size = end - hi; next[n++] = right; }
        }
        if (r[i].physical) next[n++] = (struct virtual_mapping){r[i].physical, (uint64_t)r[i].physical_offset, lo, r[i].size, r[i].storage_class};
        free(v->mappings); v->mappings = next; v->mapping_count = n; v->mapping_capacity = cap;
    }
    static int logged; if (logged++ < 16)
        wonder_nvn_report("NVN_CALL nvnMemoryPoolMapVirtual pool=%p requests=%d mappings=%u", pool, count, v->mapping_count);
    return 1;
}

// ---------- Descriptor pools: host registry beside the game's descriptor memory ----------
#define TPOOL_MAGIC 0x4c505854u   /* "TXPL" */
#define SPOOL_MAGIC 0x4c505053u   /* "SPPL" */
struct descriptor_pool { uint32_t magic; int32_t count; const struct memory_pool *memory; int64_t offset; void *host; };
_Static_assert(sizeof(struct descriptor_pool) <= WONDER_NVN_DESCRIPTOR_POOL_SIZE, "descriptor pool storage");
static uint32_t descriptor_pool_init(void *pool, const void *memory_pool, ptrdiff_t offset, int count, uint32_t magic,
                                     int descriptor_info, int reserved_info, size_t host_entry, const char *who) {
    const struct memory_pool *m = pool_of(memory_pool, who);
    size_t descriptor = (size_t)info(descriptor_info);
    if (!pool || count < info(reserved_info) || offset < 0 || (size_t)offset % descriptor ||
        (uint64_t)offset + (uint64_t)count * descriptor > m->size)
        wonder_nvn_stop("NVN_INVALID_ARGUMENT %s offset=%#tx count=%d (descriptor %zu, reserved %d, pool %#llx)", who, offset, count,
                        descriptor, info(reserved_info), (unsigned long long)m->size);
    void *host = calloc((size_t)count, host_entry);
    if (!host) wonder_nvn_stop("NVN_HOST_OUT_OF_MEMORY %s count=%d", who, count);
    *(struct descriptor_pool *)pool = (struct descriptor_pool){magic, count, m, offset, host};
    wonder_nvn_report("NVN_CALL %s pool=%p memory_pool=%p offset=%#tx count=%d", who, pool, memory_pool, offset, count);
    return 1;
}
uint32_t wonder_nvnTexturePoolInitialize(void *pool, const void *memory_pool, ptrdiff_t offset, int count) {
    return descriptor_pool_init(pool, memory_pool, offset, count, TPOOL_MAGIC, 0x0F, 0x11, wonder_nvn_texture_registration_size(), "nvnTexturePoolInitialize");
}
uint32_t wonder_nvnSamplerPoolInitialize(void *pool, const void *memory_pool, ptrdiff_t offset, int count) {
    return descriptor_pool_init(pool, memory_pool, offset, count, SPOOL_MAGIC, 0x10, 0x12, sizeof(struct wonder_nvn_sampler_state), "nvnSamplerPoolInitialize");
}

// ---------- Samplers (main+0x7339c4..0x733a48) ----------
#define SBUILDER_MAGIC 0x42504d53u
#define SAMPLER_MAGIC 0x504d4153u
struct sampler_builder { uint32_t magic, device_mark; const void *device; struct wonder_nvn_sampler_state state; };
struct sampler { uint32_t magic; struct wonder_nvn_sampler_state state; };
_Static_assert(sizeof(struct sampler_builder) <= WONDER_NVN_BUILDER_SLOT, "sampler builder slot");
_Static_assert(sizeof(struct sampler) <= WONDER_NVN_SAMPLER_SIZE, "sampler storage");
static struct sampler_builder *sbuilder_of(void *p, const char *who) {
    struct sampler_builder *b = p;
    if (!b || b->magic != SBUILDER_MAGIC) wonder_nvn_stop("NVN_INVALID_OBJECT %s sampler_builder=%p (SetDefaults not called)", who, p);
    return b;
}
void wonder_nvnSamplerBuilderSetDefaults(void *builder) {
    if (!builder) wonder_nvn_stop("NVN_INVALID_OBJECT nvnSamplerBuilderSetDefaults builder=NULL");
    struct sampler_builder *b = builder;
    const void *device = b->device_mark == 0x56454454u ? b->device : 0;
    memset(builder, 0, sizeof *b);
    b->magic = SBUILDER_MAGIC; b->device = device; b->device_mark = device ? 0x56454454u : 0;
    // GL/NVN defaults: NEAREST_MIPMAP_LINEAR / LINEAR, REPEAT, compare off with LESS, LOD [-1000, 1000].
    b->state = (struct wonder_nvn_sampler_state){.min = 4, .mag = 1, .wrap_s = 1, .wrap_t = 1, .wrap_r = 1, .compare_mode = 0, .compare_func = 2,
                                                 .valid = 1, .lod_min = -1000.f, .lod_max = 1000.f, .anisotropy = 1.f};
}
void wonder_nvnSamplerBuilderSetDevice(void *builder, const void *device) {
    if (!builder) wonder_nvn_stop("NVN_INVALID_OBJECT nvnSamplerBuilderSetDevice builder=NULL");
    // main+0x9afdc calls SetDevice before SetDefaults; defaults preserve the device association.
    struct sampler_builder *b = builder;
    b->device = device_of(device, "nvnSamplerBuilderSetDevice"); b->device_mark = 0x56454454u;
}
void wonder_nvnSamplerBuilderSetMinMagFilter(void *builder, int min, int mag) {
    struct sampler_builder *b = sbuilder_of(builder, "nvnSamplerBuilderSetMinMagFilter");
    if (min < 0 || min > 5 || mag < 0 || mag > 1) wonder_nvn_stop("NVN_INVALID_ARGUMENT nvnSamplerBuilderSetMinMagFilter min=%d mag=%d", min, mag);
    b->state.min = (uint8_t)min; b->state.mag = (uint8_t)mag;
}
// Value setters only; Metal sampler creation must check these values for actual hardware support.
void wonder_nvnSamplerBuilderSetWrapMode(void *builder, int x, int y, int z) {
    struct sampler_builder *b = sbuilder_of(builder, "nvnSamplerBuilderSetWrapMode");
    if ((unsigned)x > 7 || (unsigned)y > 7 || (unsigned)z > 7)
        wonder_nvn_stop("NVN_INVALID_ARGUMENT nvnSamplerBuilderSetWrapMode %d,%d,%d", x, y, z);
    b->state.wrap_s = x; b->state.wrap_t = y; b->state.wrap_r = z;
}
void wonder_nvnSamplerBuilderSetLodClamp(void *builder, float min, float max) {
    struct sampler_builder *b = sbuilder_of(builder, "nvnSamplerBuilderSetLodClamp");
    if (!__builtin_isfinite(min) || !__builtin_isfinite(max) || min > max)
        wonder_nvn_stop("NVN_INVALID_ARGUMENT nvnSamplerBuilderSetLodClamp");
    b->state.lod_min = min; b->state.lod_max = max;
}
void wonder_nvnSamplerBuilderSetLodBias(void *builder, float bias) {
    struct sampler_builder *b = sbuilder_of(builder, "nvnSamplerBuilderSetLodBias");
    if (!__builtin_isfinite(bias)) wonder_nvn_stop("NVN_INVALID_ARGUMENT nvnSamplerBuilderSetLodBias");
    b->state.lod_bias = bias;
}
void wonder_nvnSamplerBuilderSetCompare(void *builder, int mode, int func) {
    struct sampler_builder *b = sbuilder_of(builder, "nvnSamplerBuilderSetCompare");
    if ((unsigned)mode > 1 || func < 1 || func > 8) wonder_nvn_stop("NVN_INVALID_ARGUMENT nvnSamplerBuilderSetCompare");
    b->state.compare_mode = mode; b->state.compare_func = func;
}
void wonder_nvnSamplerBuilderSetBorderColor(void *builder, const float *color) {
    struct sampler_builder *b = sbuilder_of(builder, "nvnSamplerBuilderSetBorderColor");
    if (!color) wonder_nvn_stop("NVN_INVALID_ARGUMENT nvnSamplerBuilderSetBorderColor NULL");
    memcpy(b->state.border, color, sizeof b->state.border);
}
void wonder_nvnSamplerBuilderSetMaxAnisotropy(void *builder, float value) {
    struct sampler_builder *b = sbuilder_of(builder, "nvnSamplerBuilderSetMaxAnisotropy");
    if (!__builtin_isfinite(value) || value < 1.f || value > 16.f)
        wonder_nvn_stop("NVN_INVALID_ARGUMENT nvnSamplerBuilderSetMaxAnisotropy");
    b->state.anisotropy = value;
}
void wonder_nvnSamplerBuilderSetReductionFilter(void *builder, int filter) {
    struct sampler_builder *b = sbuilder_of(builder, "nvnSamplerBuilderSetReductionFilter");
    if ((unsigned)filter > 2) wonder_nvn_stop("NVN_INVALID_ARGUMENT nvnSamplerBuilderSetReductionFilter");
    b->state.reduction = filter;
}
uint32_t wonder_nvnSamplerInitialize(void *sampler, const void *builder) {
    const struct sampler_builder *b = sbuilder_of((void *)builder, "nvnSamplerInitialize");
    if (!sampler) wonder_nvn_stop("NVN_INVALID_OBJECT nvnSamplerInitialize sampler=NULL");
    if (!b->device) wonder_nvn_stop("NVN_INVALID_ARGUMENT nvnSamplerInitialize: builder has no device");
    *(struct sampler *)sampler = (struct sampler){SAMPLER_MAGIC, b->state};
    return 1;
}
void wonder_nvnSamplerPoolRegisterSampler(const void *pool, int id, const void *sampler) {
    const struct descriptor_pool *p = pool;
    const struct sampler *s = sampler;
    if (!p || p->magic != SPOOL_MAGIC) wonder_nvn_stop("NVN_INVALID_OBJECT nvnSamplerPoolRegisterSampler pool=%p", pool);
    if (!s || s->magic != SAMPLER_MAGIC) wonder_nvn_stop("NVN_INVALID_OBJECT nvnSamplerPoolRegisterSampler sampler=%p", sampler);
    if (id < 0 || id >= p->count) wonder_nvn_stop("NVN_INVALID_ARGUMENT nvnSamplerPoolRegisterSampler id=%d count=%d", id, p->count);
    ((struct wonder_nvn_sampler_state *)p->host)[id] = s->state;
    wonder_nvn_report("NVN_CALL nvnSamplerPoolRegisterSampler id=%d min=%u mag=%u", id, s->state.min, s->state.mag);
}
const struct wonder_nvn_sampler_state *wonder_nvn_sampler_lookup(const void *pool, int id) {
    const struct descriptor_pool *p = pool;
    if (!p || p->magic != SPOOL_MAGIC || id < 0 || id >= p->count) return 0;
    const struct wonder_nvn_sampler_state *s = &((const struct wonder_nvn_sampler_state *)p->host)[id];
    return s->valid ? s : 0;
}

struct implemented { const char *name; void *fn; };
static const struct implemented implemented[] = {
    {"nvnDeviceBuilderSetDefaults", (void *)wonder_nvnDeviceBuilderSetDefaults},
    {"nvnDeviceBuilderSetFlags", (void *)wonder_nvnDeviceBuilderSetFlags},
    {"nvnDeviceInitialize", (void *)wonder_nvnDeviceInitialize},
    {"nvnDeviceGetInteger", (void *)wonder_nvnDeviceGetInteger},
    {"nvnDeviceGetProcAddress", (void *)wonder_nvnDeviceGetProcAddress},
    {"nvnDeviceSetWindowOriginMode", (void *)wonder_nvnDeviceSetWindowOriginMode},
    {"nvnDeviceSetDepthMode", (void *)wonder_nvnDeviceSetDepthMode},
    {"nvnDeviceRegisterFastClearColor", (void *)wonder_nvnDeviceRegisterFastClearColor},
    {"nvnDeviceRegisterFastClearColori", (void *)wonder_nvnDeviceRegisterFastClearColori},
    {"nvnDeviceRegisterFastClearColorui", (void *)wonder_nvnDeviceRegisterFastClearColorui},
    {"nvnDeviceRegisterFastClearDepth", (void *)wonder_nvnDeviceRegisterFastClearDepth},
    {"nvnQueueBuilderSetDefaults", (void *)wonder_nvnQueueBuilderSetDefaults},
    {"nvnQueueBuilderSetDevice", (void *)wonder_nvnQueueBuilderSetDevice},
    {"nvnQueueBuilderSetFlags", (void *)wonder_nvnQueueBuilderSetFlags},
    {"nvnQueueBuilderSetCommandMemorySize", (void *)wonder_nvnQueueBuilderSetCommandMemorySize},
    {"nvnQueueBuilderSetComputeMemorySize", (void *)wonder_nvnQueueBuilderSetComputeMemorySize},
    {"nvnQueueBuilderSetControlMemorySize", (void *)wonder_nvnQueueBuilderSetControlMemorySize},
    {"nvnQueueBuilderGetQueueMemorySize", (void *)wonder_nvnQueueBuilderGetQueueMemorySize},
    {"nvnQueueBuilderSetQueueMemory", (void *)wonder_nvnQueueBuilderSetQueueMemory},
    {"nvnQueueInitialize", (void *)wonder_nvnQueueInitialize},
    {"nvnMemoryPoolBuilderSetDefaults", (void *)wonder_nvnMemoryPoolBuilderSetDefaults},
    {"nvnMemoryPoolBuilderSetDevice", (void *)wonder_nvnMemoryPoolBuilderSetDevice},
    {"nvnMemoryPoolBuilderSetFlags", (void *)wonder_nvnMemoryPoolBuilderSetFlags},
    {"nvnMemoryPoolBuilderSetStorage", (void *)wonder_nvnMemoryPoolBuilderSetStorage},
    {"nvnMemoryPoolInitialize", (void *)wonder_nvnMemoryPoolInitialize},
    {"nvnMemoryPoolMapVirtual", (void *)wonder_nvnMemoryPoolMapVirtual},
    {"nvnTexturePoolInitialize", (void *)wonder_nvnTexturePoolInitialize},
    {"nvnSamplerPoolInitialize", (void *)wonder_nvnSamplerPoolInitialize},
    {"nvnSamplerBuilderSetDefaults", (void *)wonder_nvnSamplerBuilderSetDefaults},
    {"nvnSamplerBuilderSetDevice", (void *)wonder_nvnSamplerBuilderSetDevice},
    {"nvnSamplerBuilderSetMinMagFilter", (void *)wonder_nvnSamplerBuilderSetMinMagFilter},
    {"nvnSamplerBuilderSetWrapMode", (void *)wonder_nvnSamplerBuilderSetWrapMode},
    {"nvnSamplerBuilderSetLodClamp", (void *)wonder_nvnSamplerBuilderSetLodClamp},
    {"nvnSamplerBuilderSetLodBias", (void *)wonder_nvnSamplerBuilderSetLodBias},
    {"nvnSamplerBuilderSetCompare", (void *)wonder_nvnSamplerBuilderSetCompare},
    {"nvnSamplerBuilderSetBorderColor", (void *)wonder_nvnSamplerBuilderSetBorderColor},
    {"nvnSamplerBuilderSetMaxAnisotropy", (void *)wonder_nvnSamplerBuilderSetMaxAnisotropy},
    {"nvnSamplerBuilderSetReductionFilter", (void *)wonder_nvnSamplerBuilderSetReductionFilter},
    {"nvnSamplerInitialize", (void *)wonder_nvnSamplerInitialize},
    {"nvnSamplerPoolRegisterSampler", (void *)wonder_nvnSamplerPoolRegisterSampler},
};
int wonder_nvn_implemented_count(void) {
    return (int)(sizeof implemented / sizeof *implemented + wonder_nvn_swapchain_count + wonder_nvn_buffer_count + wonder_nvn_texture_count + wonder_nvn_state_count);
}
int wonder_nvn_name_index(const char *name) {
    if (!name) return -1;
    for (int i = 0; i < WONDER_NVN_NAME_COUNT; i++) if (!strcmp(wonder_nvn_names[i].name, name)) return i;
    return -1;
}
void *wonder_nvn_resolve(const char *name, const char *via) {
    int index = wonder_nvn_name_index(name);
    for (unsigned i = 0; i < sizeof implemented / sizeof *implemented; i++)
        if (name && !strcmp(implemented[i].name, name)) {
            if (index < 0) wonder_nvn_stop("NVN_TABLE_MISMATCH %s implemented but absent from Wonder loader", name);
            return implemented[i].fn;
        }
    const struct { const struct wonder_nvn_entry *entries; unsigned count; } units[] = {
        {wonder_nvn_swapchain_entries, wonder_nvn_swapchain_count}, {wonder_nvn_buffer_entries, wonder_nvn_buffer_count},
        {wonder_nvn_texture_entries, wonder_nvn_texture_count}, {wonder_nvn_state_entries, wonder_nvn_state_count}};
    for (unsigned u = 0; u < sizeof units / sizeof *units; u++)
        for (unsigned i = 0; i < units[u].count; i++)
            if (name && !strcmp(units[u].entries[i].name, name)) {
                if (index < 0) wonder_nvn_stop("NVN_TABLE_MISMATCH %s implemented but absent from Wonder loader", name);
                return units[u].entries[i].fn;
            }
    if (index >= 0) return wonder_nvn_unimplemented_thunk((unsigned)index);
    // Real NVN returns NULL for an unknown name; Wonder never asks for one, so report it.
    wonder_nvn_report("NVN_UNKNOWN_NAME via=%s name=%s -> NULL", via, name ? name : "(null)");
    return 0;
}
// The loader (main+0x1080ff0) asks for every name in slot order; summarize each completed pass.
static unsigned pass_count, pass_implemented, pass_thunks;
void *wonder_nvnDeviceGetProcAddress(const void *device, const char *name) {
    if (device) device_of(device, "nvnDeviceGetProcAddress");
    void *fn = wonder_nvn_resolve(name, device ? "nvnDeviceGetProcAddress(device)" : "nvnDeviceGetProcAddress(NULL)");
    int index = wonder_nvn_name_index(name);
    if (index >= 0) {
        if (fn == wonder_nvn_unimplemented_thunk((unsigned)index)) pass_thunks++; else pass_implemented++;
        if (index == WONDER_NVN_NAME_COUNT - 1) {
            wonder_nvn_report("NVN_PROC_TABLE pass=%u device=%p implemented=%u stop_thunks=%u", ++pass_count, device, pass_implemented, pass_thunks);
            pass_implemented = pass_thunks = 0;
        }
    }
    return fn;
}
