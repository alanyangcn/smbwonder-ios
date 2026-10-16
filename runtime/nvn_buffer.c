// NVN buffers and command-buffer objects for Wonder's renderer setup (main+0x5bbcf4..0x5bbffc).
// Buffers use CPU-address resource tokens. Command objects use the target SDK ABI and bind initial
// control/command memory. Recording, segment chaining and submission still stop when called.
#include "nvn_device.h"
#include <string.h>
#define BBUILDER_MAGIC 0x42465542u
#define DEVICE_SET_MARK 0x56454454u
#define BUFFER_MAGIC 0x46465542u           /* "BUFF" */
#define BUFFER_BUILDER_SIZE 0x40           // sp+0x10 slot (header 0x40)
#define BUFFER_SIZE 0x30                   // main+0x5bbf94

struct buffer_builder { uint32_t magic, device_mark; const void *device; const void *pool; int64_t offset; uint64_t size; };
struct buffer { uint32_t magic, pool_flags; const void *device; uint8_t *memory; uint64_t size; const void *pool; int64_t offset; };
_Static_assert(sizeof(struct buffer_builder) <= BUFFER_BUILDER_SIZE, "buffer builder storage");
_Static_assert(sizeof(struct buffer) <= BUFFER_SIZE, "buffer storage");

// ---------- Buffers ----------
static struct buffer_builder *bbuilder_of(void *p, const char *who) {
    struct buffer_builder *b = p;
    if (!b || b->magic != BBUILDER_MAGIC) wonder_nvn_stop("NVN_INVALID_OBJECT %s buffer_builder=%p (SetDefaults not called)", who, p);
    return b;
}
void wonder_nvnBufferBuilderSetDevice(void *builder, const void *device) {
    if (!builder) wonder_nvn_stop("NVN_INVALID_OBJECT nvnBufferBuilderSetDevice builder=NULL");
    struct buffer_builder *b = builder;   // main+0x5bbf50: called before SetDefaults
    b->device = wonder_nvn_device_check(device, "nvnBufferBuilderSetDevice"); b->device_mark = DEVICE_SET_MARK;
}
void wonder_nvnBufferBuilderSetDefaults(void *builder) {
    if (!builder) wonder_nvn_stop("NVN_INVALID_OBJECT nvnBufferBuilderSetDefaults builder=NULL");
    struct buffer_builder *b = builder;
    const void *device = b->device_mark == DEVICE_SET_MARK ? b->device : 0;   // NVN: SetDefaults keeps the device
    memset(b, 0, sizeof *b);
    b->magic = BBUILDER_MAGIC; b->device = device; b->device_mark = device ? DEVICE_SET_MARK : 0;
}
// The sdk's implementation (name index 21 -> sdk+0x442d00) only stores pool/offset/size; the pool (which may be NULL,
// round123: main+0x107fc74 while loading the first course) and the range are checked when the buffer is initialized.
void wonder_nvnBufferBuilderSetStorage(void *builder, const void *pool, ptrdiff_t offset, size_t size) {
    struct buffer_builder *b = bbuilder_of(builder, "nvnBufferBuilderSetStorage");
    b->pool = pool; b->offset = offset; b->size = size;
}
uint32_t wonder_nvnBufferInitialize(void *buffer, const void *builder) {
    const struct buffer_builder *b = bbuilder_of((void *)builder, "nvnBufferInitialize");
    if (!buffer) wonder_nvn_stop("NVN_INVALID_OBJECT nvnBufferInitialize buffer=NULL");
    if (!b->device || !b->pool) wonder_nvn_stop("NVN_INVALID_ARGUMENT nvnBufferInitialize device=%p pool=%p", b->device, b->pool);
    struct wonder_nvn_pool_view view; wonder_nvn_pool_check(b->pool, "nvnBufferInitialize", &view);
    if (b->offset < 0 || !b->size || (uint64_t)b->offset + b->size > view.size)
        wonder_nvn_stop("NVN_INVALID_ARGUMENT nvnBufferInitialize storage offset=%#llx size=%#llx pool=%#llx", (long long)b->offset, (unsigned long long)b->size, (unsigned long long)view.size);
    *(struct buffer *)buffer = (struct buffer){BUFFER_MAGIC, view.flags, b->device, view.memory + b->offset, b->size, b->pool, b->offset};
    wonder_nvn_report("NVN_BUFFER_READY buffer=%p address=%p size=%#llx pool_flags=%#x", buffer, (void *)(view.memory + b->offset),
                      (unsigned long long)b->size, view.flags);
    return 1;
}
void *wonder_nvnBufferMap(const void *buffer) {
    const struct buffer *b = buffer;
    if (!b || b->magic != BUFFER_MAGIC) wonder_nvn_stop("NVN_INVALID_OBJECT nvnBufferMap buffer=%p", buffer);
    if (b->pool_flags & 1) wonder_nvn_stop("NVN_INVALID_ARGUMENT nvnBufferMap: pool is CPU_NO_ACCESS");
    return b->memory;   // CPU and GPU share the same bytes (zero-copy pool buffer)
}

void wonder_nvnBufferSetDebugLabel(void *buffer, const char *label) {
    const struct buffer *b = buffer;
    if (!b || b->magic != BUFFER_MAGIC) wonder_nvn_stop("NVN_INVALID_OBJECT nvnBufferSetDebugLabel buffer=%p", buffer);
    struct wonder_nvn_pool_view view; wonder_nvn_pool_check(b->pool, "nvnBufferSetDebugLabel", &view);
    // Buffers are subranges of the shared Metal pool resource. The log preserves their individual names.
    if (view.host_buffer) wonder_host_resource_label(view.host_buffer, label);
    wonder_nvn_report("NVN_BUFFER_LABEL buffer=%p label=%s", buffer, label ? label : "");
}

// GPU address = CPU address (zero-copy pool buffers), so a buffer's address is its mapping.
uint64_t wonder_nvnBufferGetAddress(const void *buffer) {
    const struct buffer *b = buffer;
    if (!b || b->magic != BUFFER_MAGIC) wonder_nvn_stop("NVN_INVALID_OBJECT nvnBufferGetAddress buffer=%p", buffer);
    return (uint64_t)(uintptr_t)b->memory;
}

// ---------- Memory pool queries / CPU cache maintenance ----------
// Host pools are Metal shared-storage buffers over the guest bytes: CPU and GPU see one coherent copy, and
// GPU work is only submitted at queue submission, so flush/invalidate need no host action. Ranges are still
// validated as a host safety policy. SDK 55 sdk+0x4539a0/0x4539cc tests CPU_CACHED
// (bit 2) and returns immediately when clear, including CPU_NO_ACCESS physical pools.
// CPU_NO_ACCESS forbids mapping; it does not make this cache-maintenance call invalid.
static void pool_range(const void *pool, ptrdiff_t offset, size_t size, const char *who, struct wonder_nvn_pool_view *view) {
    wonder_nvn_pool_check(pool, who, view);
    if (offset < 0 || (uint64_t)offset > view->size || size > view->size - (uint64_t)offset)
        wonder_nvn_stop("NVN_INVALID_ARGUMENT %s offset=%#tx size=%#zx pool=%#llx", who, offset, size, (unsigned long long)view->size);
}
void wonder_nvnMemoryPoolSetDebugLabel(void *pool, const char *label) {
    struct wonder_nvn_pool_view view; wonder_nvn_pool_check(pool, "nvnMemoryPoolSetDebugLabel", &view);
    if (view.host_buffer) wonder_host_resource_label(view.host_buffer, label);
    wonder_nvn_report("NVN_POOL_LABEL pool=%p label=%s", pool, label ? label : "");
}
int wonder_nvnMemoryPoolGetFlags(const void *pool) {
    struct wonder_nvn_pool_view view; wonder_nvn_pool_check(pool, "nvnMemoryPoolGetFlags", &view); return (int)view.flags;
}
uint64_t wonder_nvnMemoryPoolGetBufferAddress(const void *pool) {
    struct wonder_nvn_pool_view view; wonder_nvn_pool_check(pool, "nvnMemoryPoolGetBufferAddress", &view);
    return (view.flags & 8) ? 0 : (uint64_t)(uintptr_t)view.memory;
}
size_t wonder_nvnMemoryPoolGetSize(const void *pool) {
    struct wonder_nvn_pool_view view; wonder_nvn_pool_check(pool, "nvnMemoryPoolGetSize", &view); return view.size;
}
void *wonder_nvnMemoryPoolMap(const void *pool) {
    struct wonder_nvn_pool_view view; wonder_nvn_pool_check(pool, "nvnMemoryPoolMap", &view);
    return (view.flags & 1) ? 0 : view.memory;   // NVN returns NULL for CPU_NO_ACCESS pools
}
void wonder_nvnMemoryPoolFlushMappedRange(const void *pool, ptrdiff_t offset, size_t size) {
    struct wonder_nvn_pool_view view; pool_range(pool, offset, size, "nvnMemoryPoolFlushMappedRange", &view);
}
void wonder_nvnMemoryPoolInvalidateMappedRange(const void *pool, ptrdiff_t offset, size_t size) {
    struct wonder_nvn_pool_view view; pool_range(pool, offset, size, "nvnMemoryPoolInvalidateMappedRange", &view);
}


// sdk+0x442da0 BufferFinalize is `ret`: the buffer is a view of pool storage with no host resource.
void wonder_nvnBufferFinalize(void *buffer) {
    const struct buffer *b = buffer;
    if (!b || b->magic != BUFFER_MAGIC) wonder_nvn_stop("NVN_INVALID_OBJECT nvnBufferFinalize buffer=%p", buffer);
}
// sdk+0x442df0/0x442e00 forward to the pool call with offset = buffer offset + range offset.
static void buffer_range(const void *buffer, ptrdiff_t offset, size_t size, int invalidate) {
    const struct buffer *b = buffer;
    const char *who = invalidate ? "nvnBufferInvalidateMappedRange" : "nvnBufferFlushMappedRange";
    if (!b || b->magic != BUFFER_MAGIC) wonder_nvn_stop("NVN_INVALID_OBJECT %s buffer=%p", who, buffer);
    if (offset < 0 || (uint64_t)offset > b->size || size > b->size - (uint64_t)offset)
        wonder_nvn_stop("NVN_INVALID_ARGUMENT %s offset=%#tx size=%#zx buffer_size=%#llx", who, offset, size, (unsigned long long)b->size);
    struct wonder_nvn_pool_view view; pool_range(b->pool, b->offset + offset, size, who, &view);
}
void wonder_nvnBufferFlushMappedRange(const void *buffer, ptrdiff_t offset, size_t size) { buffer_range(buffer, offset, size, 0); }
void wonder_nvnBufferInvalidateMappedRange(const void *buffer, ptrdiff_t offset, size_t size) { buffer_range(buffer, offset, size, 1); }
const struct wonder_nvn_entry wonder_nvn_buffer_entries[] = {
    {"nvnBufferFinalize", (void *)wonder_nvnBufferFinalize},
    {"nvnBufferFlushMappedRange", (void *)wonder_nvnBufferFlushMappedRange},
    {"nvnBufferInvalidateMappedRange", (void *)wonder_nvnBufferInvalidateMappedRange},
    {"nvnBufferBuilderSetDevice", (void *)wonder_nvnBufferBuilderSetDevice},
    {"nvnBufferBuilderSetDefaults", (void *)wonder_nvnBufferBuilderSetDefaults},
    {"nvnBufferBuilderSetStorage", (void *)wonder_nvnBufferBuilderSetStorage},
    {"nvnBufferInitialize", (void *)wonder_nvnBufferInitialize},
    {"nvnBufferMap", (void *)wonder_nvnBufferMap},
    {"nvnBufferSetDebugLabel", (void *)wonder_nvnBufferSetDebugLabel},
    {"nvnBufferGetAddress", (void *)wonder_nvnBufferGetAddress},
    {"nvnMemoryPoolSetDebugLabel", (void *)wonder_nvnMemoryPoolSetDebugLabel},
    {"nvnMemoryPoolGetFlags", (void *)wonder_nvnMemoryPoolGetFlags},
    {"nvnMemoryPoolGetBufferAddress", (void *)wonder_nvnMemoryPoolGetBufferAddress},
    {"nvnMemoryPoolGetSize", (void *)wonder_nvnMemoryPoolGetSize},
    {"nvnMemoryPoolMap", (void *)wonder_nvnMemoryPoolMap},
    {"nvnMemoryPoolFlushMappedRange", (void *)wonder_nvnMemoryPoolFlushMappedRange},
    {"nvnMemoryPoolInvalidateMappedRange", (void *)wonder_nvnMemoryPoolInvalidateMappedRange},
};
const unsigned wonder_nvn_buffer_count = sizeof wonder_nvn_buffer_entries / sizeof *wonder_nvn_buffer_entries;
