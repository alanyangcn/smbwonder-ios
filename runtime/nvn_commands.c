// SDK55 command object and control-stream ABI; unknown/native payloads are rejected before replay.
#include "nvn_commands.h"
#include <string.h>
#include <pthread.h>
#include <stdlib.h>
#define COMMAND_BUFFER_SIZE 0xa0
// The guest inlines accesses to this ABI (main+0x5c2b8..0x5c2fc). Do not overlay custom structs.
// SDK+0x442ed0 clears 0xa0 bytes, device=0x60, callbacks=0x68/0x70; flag bytes live at 0x10..0x15.
static void *command_objects[1024];
static unsigned command_objects_count;
static pthread_mutex_t command_objects_lock = PTHREAD_MUTEX_INITIALIZER;

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

void wonder_nvnCommandBufferBeginRecording(void *cmdbuf) {
    uint8_t *c = cmdbuf_of(cmdbuf, "nvnCommandBufferBeginRecording");
    if (c[0x11]) wonder_nvn_stop("NVN_INVALID_STATE nvnCommandBufferBeginRecording already recording");
    uint8_t *command, *control, *limit;
    memcpy(&command, c, sizeof command);
    memcpy(&control, c + 0x20, sizeof control); memcpy(&limit, c + 0x28, sizeof limit);
    if (!command || !control || (uintptr_t)limit < (uintptr_t)control || (uintptr_t)limit - (uintptr_t)control < 16)
        wonder_nvn_stop("NVN_UNSUPPORTED nvnCommandBufferBeginRecording needs memory callback provisioning");
    // Exact SDK 55 Begin control header, sdk+0x4432b0..443338, constant at sdk+0xaa8250.
    // Keep native metadata so main's inline cursor/recording accesses remain valid. This records no draw.
    const uint64_t header[2] = {UINT64_C(0xffffffffffffff01), 0};
    memset(c + 0x50, 0, 8);
    command_pointer(c, 0x40, command); command_pointer(c, 0x48, control);
    c[0x11] = 1; c[0x12] = 0;
    memcpy(control, header, sizeof header);
    command_pointer(c, 0x20, control + sizeof header);
    wonder_nvn_report("NVN_RECORD_BEGIN command_buffer=%p command=%p control=%p", cmdbuf, command, control);
}

struct recorded_handle {
    uint64_t handle; uint8_t *command; size_t command_size;
    uint8_t *control, *control_end;
    struct recorded_handle *next;
};
static struct recorded_handle *recorded_handles;
static pthread_mutex_t handles_lock = PTHREAD_MUTEX_INITIALIZER;

static void record_command(void *cmdbuf, unsigned opcode, const void *payload, size_t size) {
    uint8_t *c = cmdbuf_of(cmdbuf, "record_command");
    if (!c[0x11]) wonder_nvn_stop("NVN_INVALID_STATE command recorded outside Begin/End");
    uint8_t *cursor, *limit;
    memcpy(&cursor, c, sizeof cursor); memcpy(&limit, c + 8, sizeof limit);
    if (size > UINT32_MAX - 24) wonder_nvn_stop("NVN_INVALID_ARGUMENT command payload too large");
    size_t bytes = (sizeof(struct wonder_nvn_command_header) + size + 7) & ~(size_t)7;
    if (!cursor || (uintptr_t)limit < (uintptr_t)cursor || bytes > (uintptr_t)limit - (uintptr_t)cursor)
        wonder_nvn_stop("NVN_COMMAND_MEMORY_EXHAUSTED needs=%zu (segment callback pending)", bytes);
    struct wonder_nvn_command_header header = {WONDER_NVN_COMMAND_MAGIC, (uint32_t)bytes, opcode, 0};
    memcpy(cursor, &header, sizeof header);
    memset(cursor + sizeof header, 0, bytes - sizeof header);
    if (size) memcpy(cursor + sizeof header, payload, size);
    command_pointer(c, 0, cursor + bytes);
}
void wonder_nvnCommandBufferDrawElements(void *cmdbuf, int primitive, int index_type, int count, uint64_t index_address) {
    // Draw templates are valid before any program is bound; state is supplied by their eventual caller.
    if (primitive < 0 || primitive > 12 || index_type < 0 || index_type > 2 || count < 0)
        wonder_nvn_stop("NVN_INVALID_ARGUMENT nvnCommandBufferDrawElements primitive=%d index_type=%d count=%d", primitive, index_type, count);
    const struct wonder_nvn_draw_elements draw = {primitive, index_type, count, 0, index_address};
    record_command(cmdbuf, WONDER_NVN_DRAW_ELEMENTS, &draw, sizeof draw);
}
uint64_t wonder_nvnCommandBufferEndRecording(void *cmdbuf) {
    uint8_t *c = cmdbuf_of(cmdbuf, "nvnCommandBufferEndRecording");
    if (!c[0x11]) wonder_nvn_stop("NVN_INVALID_STATE nvnCommandBufferEndRecording not recording");
    uint8_t *cursor, *start, *control, *control_start, *limit, *command_base, *gpu_base;
    memcpy(&cursor, c, 8); memcpy(&start, c + 0x40, 8);
    memcpy(&control, c + 0x20, 8); memcpy(&control_start, c + 0x48, 8); memcpy(&limit, c + 0x28, 8);
    memcpy(&command_base, c + 0x30, 8); memcpy(&gpu_base, c + 0x58, 8);
    uint8_t *command_end; memcpy(&command_end, c + 0x18, 8);
    if ((uintptr_t)command_end < (uintptr_t)command_base || (uintptr_t)start < (uintptr_t)command_base ||
        (uintptr_t)cursor < (uintptr_t)start || (uintptr_t)cursor > (uintptr_t)command_end)
        wonder_nvn_stop("NVN_INVALID_STATE EndRecording command cursor outside granted memory");
    uint64_t prior_group; memcpy(&prior_group, c + 0x50, 8);
    if (prior_group || (uintptr_t)cursor < (uintptr_t)start || ((uintptr_t)cursor - (uintptr_t)start) % 4)
        wonder_nvn_stop("NVN_UNSUPPORTED EndRecording native control groups/segment chain pending");
    size_t bytes = (uintptr_t)cursor - (uintptr_t)start;
    size_t need = bytes ? 40 : 8;
    if (!control || (uintptr_t)limit < (uintptr_t)control || need > (uintptr_t)limit - (uintptr_t)control || bytes / 4 >= (1u << 21))
        wonder_nvn_stop("NVN_CONTROL_MEMORY_EXHAUSTED EndRecording needs=%zu", need);
    uint8_t *body = control_start + 16;
    if (control != body) wonder_nvn_stop("NVN_UNSUPPORTED EndRecording pre-existing native control records");
    if (bytes) {
        // SDK55 uncompressed handle form: group header type=3,count=1, then one 24-byte command span.
        const uint64_t group = UINT64_C(0x100000003);
        memcpy(control, &group, 8); control += 8;
        uint64_t address = (uintptr_t)gpu_base + ((uintptr_t)start - (uintptr_t)command_base);
        memcpy(control, &address, 8); memcpy(control + 8, &start, 8);
        uint32_t words = (uint32_t)(bytes / 4); memcpy(control + 16, &words, 4);
        memset(control + 20, 0, 4); control[20] = 3; control += 24;
    }
    memset(control, 0, 8); control[0] = 2; control += 8; // End control marker.
    struct recorded_handle *entry = calloc(1, sizeof *entry);
    if (!entry) wonder_nvn_stop("NVN_HOST_OUT_OF_MEMORY EndRecording");
    *entry = (struct recorded_handle){(uint64_t)(uintptr_t)body, start, bytes, body, control, NULL};
    pthread_mutex_lock(&handles_lock);
    struct recorded_handle **slot = &recorded_handles;
    while (*slot && (*slot)->handle != entry->handle) slot = &(*slot)->next;
    if (*slot) { struct recorded_handle *old = *slot; entry->next = old->next; *slot = entry; free(old); }
    else *slot = entry;
    pthread_mutex_unlock(&handles_lock);
    command_pointer(c, 0x20, control); command_pointer(c, 0x40, cursor); command_pointer(c, 0x48, control);
    memset(c + 0x50, 0, 8); c[0x11] = c[0x12] = 0;
    wonder_nvn_report("NVN_RECORD_END command_buffer=%p handle=%p command_bytes=%zu", cmdbuf, body, bytes);
    return (uint64_t)(uintptr_t)body; // Valid native uncompressed handle; no GPU work has executed.
}
void wonder_nvn_command_walk(uint64_t handle, wonder_nvn_command_visitor visitor, void *context) {
    struct recorded_handle snapshot = {0};
    pthread_mutex_lock(&handles_lock);
    for (struct recorded_handle *p = recorded_handles; p; p = p->next) if (p->handle == handle) { snapshot = *p; break; }
    pthread_mutex_unlock(&handles_lock);
    if (!snapshot.handle || !visitor) wonder_nvn_stop("NVN_INVALID_HANDLE command handle=%#llx", (unsigned long long)handle);
    size_t offset = 0;
    while (offset < snapshot.command_size) {
        struct wonder_nvn_command_header header;
        if (snapshot.command_size - offset < sizeof header) wonder_nvn_stop("NVN_INVALID_COMMAND truncated record");
        memcpy(&header, snapshot.command + offset, sizeof header);
        if (header.magic != WONDER_NVN_COMMAND_MAGIC)
            wonder_nvn_stop("NVN_NATIVE_PACKET_UNSUPPORTED handle=%#llx offset=%#zx word=%#x", (unsigned long long)handle, offset, header.magic);
        if (header.size < sizeof header || header.size % 8 || header.size > snapshot.command_size - offset)
            wonder_nvn_stop("NVN_INVALID_COMMAND record size=%u", header.size);
        size_t payload_size = header.size - sizeof header;
        void *copy = malloc(payload_size ? payload_size : 1);
        if (!copy) wonder_nvn_stop("NVN_HOST_OUT_OF_MEMORY command snapshot");
        memcpy(copy, snapshot.command + offset + sizeof header, payload_size);
        visitor(header.opcode, copy, payload_size, context);
        free(copy); offset += header.size;
    }
}

static size_t command_distance(const void *cmdbuf, size_t high, size_t low, size_t tail) {
    uint8_t *c = cmdbuf_of((void *)cmdbuf, "command memory query");
    uintptr_t hi, lo; memcpy(&hi, c + high, sizeof hi); memcpy(&lo, c + low, sizeof lo);
    if (hi < lo || hi - lo > SIZE_MAX - tail) wonder_nvn_stop("NVN_INVALID_STATE command memory cursor order");
    return hi - lo + tail;
}
// Target SDK+443240..4432ac: control end excludes its 16-byte link reserve; queries include it.
size_t wonder_nvnCommandBufferGetCommandMemorySize(const void *p) { return command_distance(p, 0x18, 0x30, 0); }
size_t wonder_nvnCommandBufferGetCommandMemoryUsed(const void *p) { return command_distance(p, 0, 0x30, 0); }
size_t wonder_nvnCommandBufferGetCommandMemoryFree(const void *p) { return command_distance(p, 0x18, 0, 0); }
size_t wonder_nvnCommandBufferGetControlMemorySize(const void *p) { return command_distance(p, 0x28, 0x38, 16); }
size_t wonder_nvnCommandBufferGetControlMemoryUsed(const void *p) { return command_distance(p, 0x20, 0x38, 0); }
size_t wonder_nvnCommandBufferGetControlMemoryFree(const void *p) { return command_distance(p, 0x28, 0x20, 16); }
uint32_t wonder_nvnCommandBufferIsRecording(const void *p) { return cmdbuf_of((void *)p, "nvnCommandBufferIsRecording")[0x11]; }
void *wonder_nvnCommandBufferGetMemoryCallback(const void *p) {
    void *value; memcpy(&value, cmdbuf_of((void *)p, "nvnCommandBufferGetMemoryCallback") + 0x68, sizeof value); return value;
}
void *wonder_nvnCommandBufferGetMemoryCallbackData(const void *p) {
    void *value; memcpy(&value, cmdbuf_of((void *)p, "nvnCommandBufferGetMemoryCallbackData") + 0x70, sizeof value); return value;
}
void wonder_nvnCommandBufferFinalize(void *p) {
    uint8_t *c = cmdbuf_of(p, "nvnCommandBufferFinalize");
    if (c[0x11]) wonder_nvn_stop("NVN_INVALID_STATE Finalize while recording");
    pthread_mutex_lock(&command_objects_lock);
    for (unsigned i = 0; i < command_objects_count; i++) if (command_objects[i] == p) {
        command_objects[i] = command_objects[--command_objects_count]; break;
    }
    pthread_mutex_unlock(&command_objects_lock);
    // SDK Finalize is ret: do not erase caller-owned bytes or invalidate independent handles.
}
void wonder_nvnDeviceFinalizeCommandHandle(void *device, uint64_t handle) {
    wonder_nvn_device_check(device, "nvnDeviceFinalizeCommandHandle");
    pthread_mutex_lock(&handles_lock);
    struct recorded_handle **slot = &recorded_handles;
    while (*slot && (*slot)->handle != handle) slot = &(*slot)->next;
    struct recorded_handle *entry = *slot;
    if (entry) *slot = entry->next;
    pthread_mutex_unlock(&handles_lock);
    if (!entry) wonder_nvn_stop("NVN_INVALID_HANDLE Finalize handle=%#llx", (unsigned long long)handle);
    free(entry);
}

void wonder_nvnCommandBufferSetDebugLabel(void *p, const char *label) {
    cmdbuf_of(p, "nvnCommandBufferSetDebugLabel");wonder_nvn_report("NVN_COMMAND_BUFFER_LABEL object=%p label=%s",p,label?label:"");
}
const struct wonder_nvn_entry wonder_nvn_command_entries[] = {
    {"nvnCommandBufferSetDebugLabel", (void *)wonder_nvnCommandBufferSetDebugLabel},
    {"nvnCommandBufferGetCommandMemorySize", (void *)wonder_nvnCommandBufferGetCommandMemorySize},
    {"nvnCommandBufferGetCommandMemoryUsed", (void *)wonder_nvnCommandBufferGetCommandMemoryUsed},
    {"nvnCommandBufferGetCommandMemoryFree", (void *)wonder_nvnCommandBufferGetCommandMemoryFree},
    {"nvnCommandBufferGetControlMemorySize", (void *)wonder_nvnCommandBufferGetControlMemorySize},
    {"nvnCommandBufferGetControlMemoryUsed", (void *)wonder_nvnCommandBufferGetControlMemoryUsed},
    {"nvnCommandBufferGetControlMemoryFree", (void *)wonder_nvnCommandBufferGetControlMemoryFree},
    {"nvnCommandBufferIsRecording", (void *)wonder_nvnCommandBufferIsRecording},
    {"nvnCommandBufferGetMemoryCallback", (void *)wonder_nvnCommandBufferGetMemoryCallback},
    {"nvnCommandBufferGetMemoryCallbackData", (void *)wonder_nvnCommandBufferGetMemoryCallbackData},
    {"nvnCommandBufferFinalize", (void *)wonder_nvnCommandBufferFinalize},
    {"nvnDeviceFinalizeCommandHandle", (void *)wonder_nvnDeviceFinalizeCommandHandle},

    {"nvnCommandBufferDrawElements", (void *)wonder_nvnCommandBufferDrawElements},
    {"nvnCommandBufferEndRecording", (void *)wonder_nvnCommandBufferEndRecording},
    {"nvnCommandBufferBeginRecording", (void *)wonder_nvnCommandBufferBeginRecording},
    {"nvnCommandBufferAddCommandMemory", (void *)wonder_nvnCommandBufferAddCommandMemory},
    {"nvnCommandBufferAddControlMemory", (void *)wonder_nvnCommandBufferAddControlMemory},
    {"nvnCommandBufferInitialize", (void *)wonder_nvnCommandBufferInitialize},
    {"nvnCommandBufferSetMemoryCallback", (void *)wonder_nvnCommandBufferSetMemoryCallback},
    {"nvnCommandBufferSetMemoryCallbackData", (void *)wonder_nvnCommandBufferSetMemoryCallbackData},
};
const unsigned wonder_nvn_command_count = sizeof wonder_nvn_command_entries / sizeof *wonder_nvn_command_entries;
