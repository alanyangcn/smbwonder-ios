// NVN buffers and command-buffer objects for Wonder's renderer setup (main+0x5bbcf4..0x5bbffc).
// Buffers use CPU-address resource tokens. Command objects use the target SDK ABI and bind initial
// control/command memory. Recording, segment chaining and submission still stop when called.
#include "nvn_device.h"
#include <string.h>
#include <pthread.h>
#define BBUILDER_MAGIC 0x42465542u
#define DEVICE_SET_MARK 0x56454454u
#define BUFFER_MAGIC 0x46465542u           /* "BUFF" */
#define BUFFER_BUILDER_SIZE 0x40           // sp+0x10 slot (header 0x40)
#define BUFFER_SIZE 0x30                   // main+0x5bbf94
#define COMMAND_BUFFER_SIZE 0xa0           // main+0x5bbcd4

struct buffer_builder { uint32_t magic, device_mark; const void *device; const void *pool; int64_t offset; uint64_t size; };
struct buffer { uint32_t magic, pool_flags; const void *device; uint8_t *memory; uint64_t size; const void *pool; int64_t offset; };
// The guest inlines accesses to this ABI (main+0x5c2b8..0x5c2fc). Do not overlay custom structs.
// SDK+0x442ed0 clears 0xa0 bytes, device=0x60, callbacks=0x68/0x70; flag bytes live at 0x10..0x15.
static void *command_objects[1024];
static unsigned command_objects_count;
static pthread_mutex_t command_objects_lock = PTHREAD_MUTEX_INITIALIZER;
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
void wonder_nvnBufferBuilderSetStorage(void *builder, const void *pool, ptrdiff_t offset, size_t size) {
    struct buffer_builder *b = bbuilder_of(builder, "nvnBufferBuilderSetStorage");
    struct wonder_nvn_pool_view view; wonder_nvn_pool_check(pool, "nvnBufferBuilderSetStorage", &view);
    if (offset < 0 || !size || (uint64_t)offset + size > view.size)
        wonder_nvn_stop("NVN_INVALID_ARGUMENT nvnBufferBuilderSetStorage offset=%#tx size=%#zx pool=%#llx", offset, size, (unsigned long long)view.size);
    b->pool = pool; b->offset = offset; b->size = size;
}
uint32_t wonder_nvnBufferInitialize(void *buffer, const void *builder) {
    const struct buffer_builder *b = bbuilder_of((void *)builder, "nvnBufferInitialize");
    if (!buffer) wonder_nvn_stop("NVN_INVALID_OBJECT nvnBufferInitialize buffer=NULL");
    if (!b->device || !b->pool) wonder_nvn_stop("NVN_INVALID_ARGUMENT nvnBufferInitialize device=%p pool=%p", b->device, b->pool);
    struct wonder_nvn_pool_view view; wonder_nvn_pool_check(b->pool, "nvnBufferInitialize", &view);
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
int wonder_nvnMemoryPoolGetFlags(const void *pool) {
    struct wonder_nvn_pool_view view; wonder_nvn_pool_check(pool, "nvnMemoryPoolGetFlags", &view); return (int)view.flags;
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

// ---------- Command buffers (object only) ----------
static uint8_t *cmdbuf_of(void *p, const char *who) {
    int found = 0;
    pthread_mutex_lock(&command_objects_lock);
    for (unsigned i = 0; i < command_objects_count; i++) if (command_objects[i] == p) { found = 1; break; }
    pthread_mutex_unlock(&command_objects_lock);
    if (!p || !found) wonder_nvn_stop("NVN_INVALID_OBJECT %s command_buffer=%p", who, p);
    return p;
}
static void command_pointer(uint8_t *object, size_t offset, const void *value) {
    memcpy(object + offset, &value, sizeof value);
}
uint32_t wonder_nvnCommandBufferInitialize(void *cmdbuf, const void *device) {
    if (!cmdbuf) wonder_nvn_stop("NVN_INVALID_OBJECT nvnCommandBufferInitialize command_buffer=NULL");
    wonder_nvn_device_check(device, "nvnCommandBufferInitialize");
    pthread_mutex_lock(&command_objects_lock);
    unsigned i;
    for (i = 0; i < command_objects_count; i++) if (command_objects[i] == cmdbuf) break;
    if (i == command_objects_count) {
        if (i == sizeof command_objects / sizeof *command_objects) {
            pthread_mutex_unlock(&command_objects_lock);
            wonder_nvn_stop("NVN_HOST_REGISTRY_FULL command buffers (Finalize not implemented)");
        }
        command_objects[command_objects_count++] = cmdbuf;
    }
    pthread_mutex_unlock(&command_objects_lock);
    memset(cmdbuf, 0, COMMAND_BUFFER_SIZE);
    uint8_t *c = cmdbuf;
    command_pointer(c, 0x60, device);
    c[0x14] = c[0x15] = 1; // SDK 55 defaults; debug flag at 0x13 stays zero (device flags=0).
    wonder_nvn_report("NVN_CALL nvnCommandBufferInitialize command_buffer=%p abi=sdk55", cmdbuf);
    return 1;
}
void wonder_nvnCommandBufferSetMemoryCallback(void *cmdbuf, void *callback) {
    uint8_t *c = cmdbuf_of(cmdbuf, "nvnCommandBufferSetMemoryCallback");
    if (!callback) wonder_nvn_stop("NVN_INVALID_ARGUMENT nvnCommandBufferSetMemoryCallback callback=NULL");
    command_pointer(c, 0x68, callback);
}
void wonder_nvnCommandBufferSetMemoryCallbackData(void *cmdbuf, void *data) {
    command_pointer(cmdbuf_of(cmdbuf, "nvnCommandBufferSetMemoryCallbackData"), 0x70, data);
}
void wonder_nvnCommandBufferAddControlMemory(void *cmdbuf, void *memory, size_t size) {
    uint8_t *c = cmdbuf_of(cmdbuf, "nvnCommandBufferAddControlMemory");
    if (!memory || ((uintptr_t)memory & 7) || size < 256 || size > UINTPTR_MAX - (uintptr_t)memory)
        wonder_nvn_stop("NVN_INVALID_ARGUMENT nvnCommandBufferAddControlMemory memory=%p size=%#zx", memory, size);
    if (c[0x11]) wonder_nvn_stop("NVN_UNSUPPORTED nvnCommandBufferAddControlMemory during recording (segment chaining pending)");
    // SDK+0x4431e8..0x443204: preserve a 16-byte tail for control-segment links.
    command_pointer(c, 0x20, memory);
    command_pointer(c, 0x28, (uint8_t *)memory + size - 16);
    command_pointer(c, 0x38, memory);
    command_pointer(c, 0x48, memory);
    memset(c + 0x50, 0, 8);
    wonder_nvn_report("NVN_CONTROL_MEMORY command_buffer=%p memory=%p size=%#zx", cmdbuf, memory, size);
}

void wonder_nvnCommandBufferAddCommandMemory(void *cmdbuf, const void *pool, ptrdiff_t offset, size_t size) {
    uint8_t *c = cmdbuf_of(cmdbuf, "nvnCommandBufferAddCommandMemory");
    struct wonder_nvn_pool_view view;
    wonder_nvn_pool_check(pool, "nvnCommandBufferAddCommandMemory", &view);
    if (offset < 0 || (offset & 3) || size < 512 || (size & 3) ||
        (uint64_t)offset > view.size || size > view.size - (uint64_t)offset)
        wonder_nvn_stop("NVN_INVALID_ARGUMENT nvnCommandBufferAddCommandMemory offset=%#tx size=%#zx", offset, size);
    if (view.flags & (1 | 8))
        wonder_nvn_stop("NVN_UNSUPPORTED nvnCommandBufferAddCommandMemory inaccessible pool flags=%#x", view.flags);
    const void *cursor, *segment_start;
    memcpy(&cursor, c, sizeof cursor); memcpy(&segment_start, c + 0x40, sizeof segment_start);
    if (c[0x11] || cursor != segment_start || c[0x16])
        wonder_nvn_stop("NVN_UNSUPPORTED nvnCommandBufferAddCommandMemory segment chaining/reserved tail pending");
    wonder_nvn_pool_span(pool, (uint64_t)offset, size, &view);
    if (view.size != size) wonder_nvn_stop("NVN_UNSUPPORTED command memory crosses physical mapping boundary");
    // SDK+0x44308c..0x4430f8, initial grant: no commands to chain, reserved-tail count at 0x16 is zero.
    // Host GPU addresses are resource-address tokens equal to CPU addresses, never Metal GPU addresses.
    command_pointer(c, 0, view.memory);
    command_pointer(c, 8, view.memory + (size < 0x7ffffc ? size : 0x7ffffc));
    command_pointer(c, 0x18, view.memory + size);
    command_pointer(c, 0x30, view.memory);
    command_pointer(c, 0x40, view.memory);
    command_pointer(c, 0x58, view.memory);
    wonder_nvn_report("NVN_COMMAND_MEMORY command_buffer=%p memory=%p size=%#zx", cmdbuf, view.memory, size);
}

const struct wonder_nvn_entry wonder_nvn_buffer_entries[] = {
    {"nvnBufferBuilderSetDevice", (void *)wonder_nvnBufferBuilderSetDevice},
    {"nvnBufferBuilderSetDefaults", (void *)wonder_nvnBufferBuilderSetDefaults},
    {"nvnBufferBuilderSetStorage", (void *)wonder_nvnBufferBuilderSetStorage},
    {"nvnBufferInitialize", (void *)wonder_nvnBufferInitialize},
    {"nvnBufferMap", (void *)wonder_nvnBufferMap},
    {"nvnBufferGetAddress", (void *)wonder_nvnBufferGetAddress},
    {"nvnMemoryPoolGetFlags", (void *)wonder_nvnMemoryPoolGetFlags},
    {"nvnMemoryPoolMap", (void *)wonder_nvnMemoryPoolMap},
    {"nvnMemoryPoolFlushMappedRange", (void *)wonder_nvnMemoryPoolFlushMappedRange},
    {"nvnMemoryPoolInvalidateMappedRange", (void *)wonder_nvnMemoryPoolInvalidateMappedRange},
    {"nvnCommandBufferAddCommandMemory", (void *)wonder_nvnCommandBufferAddCommandMemory},
    {"nvnCommandBufferAddControlMemory", (void *)wonder_nvnCommandBufferAddControlMemory},
    {"nvnCommandBufferInitialize", (void *)wonder_nvnCommandBufferInitialize},
    {"nvnCommandBufferSetMemoryCallback", (void *)wonder_nvnCommandBufferSetMemoryCallback},
    {"nvnCommandBufferSetMemoryCallbackData", (void *)wonder_nvnCommandBufferSetMemoryCallbackData},
};
const unsigned wonder_nvn_buffer_count = sizeof wonder_nvn_buffer_entries / sizeof *wonder_nvn_buffer_entries;
