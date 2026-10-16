// SDK55 command object and control-stream ABI; unknown/native payloads are rejected before replay.
// Command recording follows the target driver (sdk+0x443584 slow path): when a record does not fit, the
// command-memory callback (event 0, at least 0x200 bytes) supplies more memory, and AddCommandMemory
// during recording closes the current span into the handle's segment list. Records are typed host
// packets with snapshotted arguments; replay (nvn_queue.c) executes or stops by name.
#include "nvn_commands.h"
#include "command_store.h"
#include "nvn_draw_census.h"
#include <string.h>
#include <pthread.h>
#include <stdatomic.h>
#include <stdlib.h>
#define COMMAND_BUFFER_SIZE 0xa0
#define MAX_SEGMENTS 64
// The guest inlines accesses to this ABI (main+0x5c2b8..0x5c2fc, including an inline BeginRecording).
// Do not overlay custom structs. SDK+0x442ed0 clears 0xa0 bytes, device=0x60, callbacks=0x68/0x70;
// flag bytes live at 0x10..0x16 (0x11 recording, 0x14 command callback enable, 0x15 control callback enable).
static void *command_objects[1024];
static unsigned command_objects_count;
static pthread_mutex_t command_objects_lock = PTHREAD_MUTEX_INITIALIZER;

struct segment { uint8_t *cpu; uint64_t gpu; size_t bytes; };
// Host-side closed segments of the recording in progress, keyed by the control start (c+0x48), which both
// our BeginRecording and the guest's inline Begin set; a different key means a new recording.
struct recording { void *cmdbuf; const void *key; unsigned count; struct segment segments[MAX_SEGMENTS]; };
static struct recording recordings[64];
static pthread_mutex_t recordings_lock = PTHREAD_MUTEX_INITIALIZER;

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
static uint8_t *command_field(const uint8_t *c, size_t offset) { uint8_t *v; memcpy(&v, c + offset, sizeof v); return v; }
// Recording state for cmdbuf; reset when the key changed. Caller holds recordings_lock.
static struct recording *recording_of(void *cmdbuf, int create) {
    const void *key = command_field(cmdbuf, 0x48);
    struct recording *free_slot = NULL;
    for (unsigned i = 0; i < sizeof recordings / sizeof *recordings; i++) {
        if (recordings[i].cmdbuf == cmdbuf) {
            if (recordings[i].key != key) { recordings[i].key = key; recordings[i].count = 0; }
            return &recordings[i];
        }
        if (!recordings[i].cmdbuf && !free_slot) free_slot = &recordings[i];
    }
    if (!create) return NULL;
    if (!free_slot) { pthread_mutex_unlock(&recordings_lock); wonder_nvn_stop("NVN_HOST_REGISTRY_FULL concurrent recordings"); }
    *free_slot = (struct recording){cmdbuf, key, 0, {{0}}};
    return free_slot;
}
static void recording_forget(void *cmdbuf) {
    pthread_mutex_lock(&recordings_lock);
    for (unsigned i = 0; i < sizeof recordings / sizeof *recordings; i++) if (recordings[i].cmdbuf == cmdbuf) recordings[i].cmdbuf = NULL;
    pthread_mutex_unlock(&recordings_lock);
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
            wonder_nvn_stop("NVN_HOST_REGISTRY_FULL command buffers");
        }
        command_objects[command_objects_count++] = cmdbuf;
    }
    pthread_mutex_unlock(&command_objects_lock);
    recording_forget(cmdbuf);
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
// sdk+0x442f60: strb w1, [x0, #0x14].
void wonder_nvnCommandBufferSetCommandMemoryCallbackEnabled(void *cmdbuf, uint32_t enabled) {
    cmdbuf_of(cmdbuf, "nvnCommandBufferSetCommandMemoryCallbackEnabled")[0x14] = (uint8_t)enabled;
}
void wonder_nvnCommandBufferAddControlMemory(void *cmdbuf, void *memory, size_t size) {
    uint8_t *c = cmdbuf_of(cmdbuf, "nvnCommandBufferAddControlMemory");
    // The advertised minimum (COMMAND_BUFFER_MIN_CONTROL_SIZE 256) is not enforced by the release driver (sdk+0x443150);
    // the host needs the 16-byte Begin control header and the 16-byte link tail. Running out later stops by name.
    if (!memory || ((uintptr_t)memory & 7) || size < 32 || size > UINTPTR_MAX - (uintptr_t)memory)
        wonder_nvn_stop("NVN_INVALID_ARGUMENT nvnCommandBufferAddControlMemory memory=%p size=%#zx", memory, size);
    if (c[0x11]) wonder_nvn_stop("NVN_UNSUPPORTED nvnCommandBufferAddControlMemory during recording (control-segment link pending)");
    // SDK+0x4431e8..0x443204: preserve a 16-byte tail for control-segment links.
    command_pointer(c, 0x20, memory);
    command_pointer(c, 0x28, (uint8_t *)memory + size - 16);
    command_pointer(c, 0x38, memory);
    command_pointer(c, 0x48, memory);
    memset(c + 0x50, 0, 8);
    wonder_nvn_report("NVN_CONTROL_MEMORY command_buffer=%p memory=%p size=%#zx", cmdbuf, memory, size);
}
// GPU address of a CPU command byte inside the current grant (c+0x30 CPU base, c+0x58 GPU base).
static uint64_t command_gpu(const uint8_t *c, const uint8_t *cpu) {
    return (uintptr_t)command_field(c, 0x58) + ((uintptr_t)cpu - (uintptr_t)command_field(c, 0x30));
}
void wonder_nvnCommandBufferAddCommandMemory(void *cmdbuf, const void *pool, ptrdiff_t offset, size_t size) {
    uint8_t *c = cmdbuf_of(cmdbuf, "nvnCommandBufferAddCommandMemory");
    struct wonder_nvn_pool_view view;
    wonder_nvn_pool_check(pool, "nvnCommandBufferAddCommandMemory", &view);
    if (offset < 0 || (offset & 3) || size < 512 || (size & 3) || (uint64_t)offset > view.size)
        wonder_nvn_stop("NVN_INVALID_ARGUMENT nvnCommandBufferAddCommandMemory offset=%#tx size=%#zx", offset, size);
    // The sdk (sdk+0x442f70) does not bound the grant by the pool: round126's game callback handed out 0x200 bytes at
    // pool offset 0xfe30 of a 0x10000 pool (0x30 past its end, in the game's own memory after it). Accept it for
    // plain pools, as the console does; virtual pools keep the mapping check below.
    int past_pool = size > view.size - (uint64_t)offset;
    if (past_pool) {
        if (view.flags & 0x200) wonder_nvn_stop("NVN_INVALID_ARGUMENT nvnCommandBufferAddCommandMemory virtual pool offset=%#tx size=%#zx past %#llx", offset, size, (unsigned long long)view.size);
        static int logged; if (logged++ < 4) wonder_nvn_report("NVN_COMMAND_MEMORY_PAST_POOL offset=%#tx size=%#zx pool=%#llx (accepted, as the sdk does)", offset, size, (unsigned long long)view.size);
    }
    if (view.flags & (1 | 8))
        wonder_nvn_stop("NVN_UNSUPPORTED nvnCommandBufferAddCommandMemory inaccessible pool flags=%#x", view.flags);
    uint8_t *cursor = command_field(c, 0), *segment_start = command_field(c, 0x40);
    if (c[0x16]) wonder_nvn_stop("NVN_UNSUPPORTED nvnCommandBufferAddCommandMemory reserved tail count=%u", c[0x16]);
    if (c[0x11]) {
        // Mid-recording grant (sdk+0x443608..0x4436f8): the commands so far become one closed span.
        if ((uintptr_t)cursor < (uintptr_t)segment_start || ((uintptr_t)cursor - (uintptr_t)segment_start) % 4)
            wonder_nvn_stop("NVN_INVALID_STATE nvnCommandBufferAddCommandMemory cursor before segment start");
        if (cursor != segment_start) {
            pthread_mutex_lock(&recordings_lock);
            struct recording *r = recording_of(cmdbuf, 1);
            if (r->count == MAX_SEGMENTS) { pthread_mutex_unlock(&recordings_lock); wonder_nvn_stop("NVN_HOST_REGISTRY_FULL command segments per recording"); }
            r->segments[r->count++] = (struct segment){segment_start, command_gpu(c, segment_start), (size_t)(cursor - segment_start)};
            pthread_mutex_unlock(&recordings_lock);
        }
    } else if (cursor != segment_start)
        wonder_nvn_stop("NVN_INVALID_STATE nvnCommandBufferAddCommandMemory unrecorded bytes outside a recording");
    if (past_pool) view.memory += offset, view.size = size;   // plain pool: guest memory continues past the pool
    else {
        wonder_nvn_pool_span(pool, (uint64_t)offset, size, &view);
        if (view.size != size) wonder_nvn_stop("NVN_UNSUPPORTED command memory crosses physical mapping boundary");
    }
    // Host GPU addresses are resource-address tokens equal to CPU addresses, never Metal GPU addresses.
    command_pointer(c, 0, view.memory);
    command_pointer(c, 8, view.memory + (size < 0x7ffffc ? size : 0x7ffffc));
    command_pointer(c, 0x18, view.memory + size);
    command_pointer(c, 0x30, view.memory);
    command_pointer(c, 0x40, view.memory);
    command_pointer(c, 0x58, view.memory);
    wonder_nvn_report("NVN_COMMAND_MEMORY command_buffer=%p memory=%p size=%#zx recording=%u", cmdbuf, view.memory, size, c[0x11]);
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
    pthread_mutex_lock(&recordings_lock);
    struct recording *r = recording_of(cmdbuf, 0); if (r) r->count = 0;
    pthread_mutex_unlock(&recordings_lock);
    wonder_nvn_report("NVN_RECORD_BEGIN command_buffer=%p command=%p control=%p", cmdbuf, command, control);
}

struct recorded_handle {
    uint64_t handle; unsigned count; struct segment *segments;
    uint8_t *control, *control_end;
    struct recorded_handle *next;
};
static struct recorded_handle *recorded_handles;
static pthread_mutex_t handles_lock = PTHREAD_MUTEX_INITIALIZER;

typedef void (*memory_callback)(void *cmdbuf, int event, size_t min_size, void *data);
static void record_command(void *cmdbuf, unsigned opcode, const void *payload, size_t size) {
    uint8_t *c = cmdbuf_of(cmdbuf, "record_command");
    if (!c[0x11]) wonder_nvn_stop("NVN_INVALID_STATE %s recorded outside Begin/End", wonder_nvn_command_name(opcode));
    if (size > UINT32_MAX - 24) wonder_nvn_stop("NVN_INVALID_ARGUMENT command payload too large");
    // Guest command memory gets an 8-byte stub; the record lives in command_store.c (see there for why).
    size_t bytes = WONDER_NVN_STUB_SIZE;
    uint8_t *cursor = command_field(c, 0), *limit = command_field(c, 8);
    if (!cursor || (uintptr_t)limit < (uintptr_t)cursor || bytes > (uintptr_t)limit - (uintptr_t)cursor) {
        // sdk+0x4435b0..0x4435f8: callback(cmdbuf, OUT_OF_COMMAND_MEMORY, max(need, 0x200), data), then recheck.
        memory_callback callback; void *data;
        memcpy(&callback, c + 0x68, sizeof callback); memcpy(&data, c + 0x70, sizeof data);
        if (!c[0x14] || !callback)
            wonder_nvn_stop("NVN_COMMAND_MEMORY_EXHAUSTED needs=%zu callback=%p enabled=%u", bytes, (void *)callback, c[0x14]);
        wonder_nvn_report("NVN_COMMAND_MEMORY_CALLBACK command_buffer=%p needs=%zu", cmdbuf, bytes);
        callback(cmdbuf, 0, bytes > 0x200 ? bytes : 0x200, data);
        cursor = command_field(c, 0); limit = command_field(c, 8);
        if (!cursor || (uintptr_t)limit < (uintptr_t)cursor || bytes > (uintptr_t)limit - (uintptr_t)cursor)
            wonder_nvn_stop("NVN_COMMAND_MEMORY_EXHAUSTED callback granted too little (needs=%zu)", bytes);
    }
    uint32_t stub[2] = {WONDER_NVN_STUB_MAGIC, 0};
    if (wonder_cmdstore_put((uint64_t)(uintptr_t)cursor, opcode, payload, size, &stub[1])) wonder_nvn_stop("NVN_HOST_OUT_OF_MEMORY command record");
    memcpy(cursor, stub, sizeof stub);
    command_pointer(c, 0, cursor + bytes);
}
// Generic record: nargs 64-bit arguments followed by blob bytes (snapshotted).
static void rec(void *cmdbuf, unsigned opcode, const uint64_t *args, unsigned nargs, const void *blob, size_t blob_size) {
    if (blob_size > 0x10000 + 0x1000) wonder_nvn_stop("NVN_INVALID_ARGUMENT %s inline data %zu bytes", wonder_nvn_command_name(opcode), blob_size);
    size_t size = sizeof(struct wonder_nvn_cmd) + nargs * 8 + blob_size;
    uint8_t stack[512]; uint8_t *buffer = size <= sizeof stack ? stack : malloc(size);
    if (!buffer) wonder_nvn_stop("NVN_HOST_OUT_OF_MEMORY command record");
    struct wonder_nvn_cmd *cmd = (struct wonder_nvn_cmd *)buffer;
    cmd->nargs = nargs; cmd->blob_size = (uint32_t)blob_size;
    if (nargs) memcpy(cmd->a, args, nargs * 8);
    if (blob_size) memcpy(cmd->a + nargs, blob, blob_size);
    record_command(cmdbuf, opcode, buffer, size);
    if (buffer != stack) free(buffer);
}
#define REC(cb, op, ...) do { const uint64_t args_[] = {__VA_ARGS__}; rec(cb, op, args_, sizeof args_ / 8, NULL, 0); } while (0)
static uint64_t fbits(float f) { uint32_t u; memcpy(&u, &f, 4); return u; }
static int finite(float f) { return __builtin_isfinite(f); }
static void need_range(int v, int lo, int hi, const char *who, const char *what) {
    if (v < lo || v > hi) wonder_nvn_stop("NVN_INVALID_ARGUMENT %s %s=%d", who, what, v);
}
// Buffer address arguments must name pool storage (0 = unbind is legal).
static void need_address(uint64_t address, uint64_t size, const char *who) {
    if (!address) return;
    struct wonder_nvn_pool_view view; wonder_nvn_address_span(address, size ? size : 1, &view); (void)who;
}

void wonder_nvnCommandBufferDrawElements(void *cmdbuf, int primitive, int index_type, int count, uint64_t index_address) {
    // Draw templates are valid before any program is bound; state is supplied by their eventual caller.
    if (primitive < 0 || primitive > 14 || index_type < 0 || index_type > 2 || count < 0)
        wonder_nvn_stop("NVN_INVALID_ARGUMENT nvnCommandBufferDrawElements primitive=%d index_type=%d count=%d", primitive, index_type, count);
    const struct wonder_nvn_draw_elements draw = {primitive, index_type, count, 0, index_address};
    record_command(cmdbuf, WONDER_NVN_DRAW_ELEMENTS, &draw, sizeof draw);
}

// ---------- State binds (state objects are value types: snapshot their reserved bytes) ----------
static void bind_state(void *cmdbuf, unsigned opcode, const void *state, size_t size, const char *who) {
    if (!state) wonder_nvn_stop("NVN_INVALID_ARGUMENT %s state=NULL", who);
    rec(cmdbuf, opcode, NULL, 0, state, size);
}
void wonder_nvnCommandBufferBindBlendState(void *cb, const void *s) { bind_state(cb, WONDER_NVN_CMD_BIND_BLEND, s, 8, "nvnCommandBufferBindBlendState"); }
void wonder_nvnCommandBufferBindChannelMaskState(void *cb, const void *s) { bind_state(cb, WONDER_NVN_CMD_BIND_CHANNEL_MASK, s, 4, "nvnCommandBufferBindChannelMaskState"); }
void wonder_nvnCommandBufferBindColorState(void *cb, const void *s) { bind_state(cb, WONDER_NVN_CMD_BIND_COLOR, s, 4, "nvnCommandBufferBindColorState"); }
void wonder_nvnCommandBufferBindDepthStencilState(void *cb, const void *s) { bind_state(cb, WONDER_NVN_CMD_BIND_DEPTH_STENCIL, s, 8, "nvnCommandBufferBindDepthStencilState"); }
void wonder_nvnCommandBufferBindMultisampleState(void *cb, const void *s) { bind_state(cb, WONDER_NVN_CMD_BIND_MULTISAMPLE, s, 0x18, "nvnCommandBufferBindMultisampleState"); }
void wonder_nvnCommandBufferBindPolygonState(void *cb, const void *s) { bind_state(cb, WONDER_NVN_CMD_BIND_POLYGON, s, 4, "nvnCommandBufferBindPolygonState"); }
void wonder_nvnCommandBufferBindVertexAttribState(void *cb, int count, const void *attribs) {
    need_range(count, 0, 16, "nvnCommandBufferBindVertexAttribState", "count");
    if (count && !attribs) wonder_nvn_stop("NVN_INVALID_ARGUMENT nvnCommandBufferBindVertexAttribState attribs=NULL");
    const uint64_t a[] = {(uint64_t)count}; rec(cb, WONDER_NVN_CMD_BIND_VERTEX_ATTRIB, a, 1, attribs, (size_t)count * 4);
}
void wonder_nvnCommandBufferBindVertexStreamState(void *cb, int count, const void *streams) {
    need_range(count, 0, 16, "nvnCommandBufferBindVertexStreamState", "count");
    if (count && !streams) wonder_nvn_stop("NVN_INVALID_ARGUMENT nvnCommandBufferBindVertexStreamState streams=NULL");
    const uint64_t a[] = {(uint64_t)count}; rec(cb, WONDER_NVN_CMD_BIND_VERTEX_STREAM, a, 1, streams, (size_t)count * 8);
}
// Diagnostic (WONDER_TRACE_VS=<vertex key prefix>[,...]): draws recorded while such a vertex shader is bound log the
// guest call chain at record time (round172: the course hedge's colour programs are never replayed; are they recorded?).
void (*wonder_nvn_frames_hook)(uint64_t fp, uint64_t lr, char *out, size_t size);   // sdk_startup.c wonder_guest_frames
static pthread_mutex_t trace_lock = PTHREAD_MUTEX_INITIALIZER;
static struct { const void *cb; char vs[17]; } trace_bound[64];
static void trace_bind(const void *cb, const struct wonder_nvn_program_shaders *s) {
    static int on = -1; if (on < 0) on = getenv("WONDER_TRACE_VS") != NULL; if (!on) return;
    char key[17] = ""; if (s && (s->mask & 1) && s->control[0]) wonder_nvn_shader_key(s->control[0], key);
    pthread_mutex_lock(&trace_lock); int slot = -1;
    for (int i = 0; i < 64 && slot < 0; i++) if (trace_bound[i].cb == cb) slot = i;
    for (int i = 0; i < 64 && slot < 0; i++) if (!trace_bound[i].cb) slot = i;
    if (slot < 0) slot = (int)(((uintptr_t)cb >> 4) & 63);
    trace_bound[slot].cb = cb; memcpy(trace_bound[slot].vs, key, sizeof key);
    pthread_mutex_unlock(&trace_lock);
    const char *list = getenv("WONDER_TRACE_VS"); int match = 0;
    for (const char *p = list; p && *p && !match; ) { size_t n = strcspn(p, ","); match = n && key[0] && !strncmp(p, key, n); p += n; if (*p == ',') p++; }
    if (match) {   // record-time bind of a listed program: which command buffer, from where in the game
        static _Atomic unsigned logged; if (atomic_fetch_add(&logged, 1) < 120) {
            char chain[700] = "";
            if (wonder_nvn_frames_hook) wonder_nvn_frames_hook((uint64_t)(uintptr_t)__builtin_frame_address(0), (uint64_t)(uintptr_t)__builtin_return_address(0), chain, sizeof chain);
            wonder_nvn_report("NVN_TRACE_RECORD_BIND vs=%s cb=%p chain=%s", key, cb, chain);
        }
    }
}
static void trace_draw(const void *cb, const char *what, int count) {
    const char *list = getenv("WONDER_TRACE_VS"); if (!list || !*list) return;
    char vs[17] = ""; pthread_mutex_lock(&trace_lock);
    for (int i = 0; i < 64; i++) if (trace_bound[i].cb == cb) { memcpy(vs, trace_bound[i].vs, sizeof vs); break; }
    pthread_mutex_unlock(&trace_lock);
    { static _Atomic unsigned shown; if (atomic_fetch_add(&shown, 1) < 6) wonder_nvn_report("NVN_TRACE_SEEN cb=%p vs=%s %s", cb, vs, what); }
    int match = 0; for (const char *p = list; *p && !match; ) { size_t n = strcspn(p, ","); match = n && vs[0] && n <= strlen(vs) && !strncmp(p, vs, n); p += n; if (*p == ',') p++; }
    if (!match) return;
    static _Atomic unsigned logged; if (atomic_fetch_add(&logged, 1) >= 400) return;
    char chain[700] = "";
    if (wonder_nvn_frames_hook) wonder_nvn_frames_hook((uint64_t)(uintptr_t)__builtin_frame_address(0), (uint64_t)(uintptr_t)__builtin_return_address(0), chain, sizeof chain);
    wonder_nvn_report("NVN_TRACE_DRAW vs=%s %s count=%d cb=%p chain=%s", vs, what, count, cb, chain);
}
// NVNshaderStageBits: VERTEX 1, TESS_CONTROL 2, TESS_EVALUATION 4, GEOMETRY 8, FRAGMENT 0x10, COMPUTE 0x20.
void wonder_nvnCommandBufferBindProgram(void *cb, const void *program, int stage_bits) {
    if ((unsigned)stage_bits & ~0x3fu) wonder_nvn_stop("NVN_INVALID_ARGUMENT nvnCommandBufferBindProgram stages=%#x", stage_bits);
    struct wonder_nvn_program_shaders shaders = {0};
    if (program && !wonder_nvn_program_snapshot(program, &shaders))
        wonder_nvn_stop("NVN_INVALID_STATE nvnCommandBufferBindProgram program=%p has no shaders", program);
    const uint64_t a[] = {(uint64_t)(unsigned)stage_bits, (uintptr_t)program};
    rec(cb, WONDER_NVN_CMD_BIND_PROGRAM, a, 2, &shaders, sizeof shaders);
    if (!program || (shaders.mask & 1)) trace_bind(cb, program ? &shaders : NULL);   // a graphics program (any stage bits)
}
void wonder_nvnCommandBufferBindVertexBuffer(void *cb, int index, uint64_t address, size_t size) {
    need_range(index, 0, 15, "nvnCommandBufferBindVertexBuffer", "index"); need_address(address, size, "nvnCommandBufferBindVertexBuffer");
    REC(cb, WONDER_NVN_CMD_BIND_VERTEX_BUFFER, (uint64_t)index, address, size);
}
void wonder_nvnCommandBufferBindUniformBuffer(void *cb, int stage, int index, uint64_t address, size_t size) {
    need_range(stage, 0, 5, "nvnCommandBufferBindUniformBuffer", "stage"); need_range(index, 0, 15, "nvnCommandBufferBindUniformBuffer", "index");
    if (size > 0x10000) wonder_nvn_stop("NVN_INVALID_ARGUMENT nvnCommandBufferBindUniformBuffer size=%#zx", size);
    need_address(address, size, "nvnCommandBufferBindUniformBuffer");
    REC(cb, WONDER_NVN_CMD_BIND_UNIFORM_BUFFER, (uint64_t)stage, (uint64_t)index, address, size);
}
void wonder_nvnCommandBufferBindStorageBuffer(void *cb, int stage, int index, uint64_t address, size_t size) {
    need_range(stage, 0, 5, "nvnCommandBufferBindStorageBuffer", "stage"); need_range(index, 0, 15, "nvnCommandBufferBindStorageBuffer", "index");
    need_address(address, size, "nvnCommandBufferBindStorageBuffer");
    REC(cb, WONDER_NVN_CMD_BIND_STORAGE_BUFFER, (uint64_t)stage, (uint64_t)index, address, size);
}
void wonder_nvnCommandBufferBindTexture(void *cb, int stage, int index, uint64_t handle) {
    need_range(stage, 0, 5, "nvnCommandBufferBindTexture", "stage"); need_range(index, 0, 31, "nvnCommandBufferBindTexture", "index");
    REC(cb, WONDER_NVN_CMD_BIND_TEXTURE, (uint64_t)stage, (uint64_t)index, handle);
}
// sdk+0x469e60: (stage w1, first w2, count w3, handles x4).
void wonder_nvnCommandBufferBindTexturesDeferred(void *cb, int stage, int first, int count, const uint64_t *handles) {
    need_range(stage, 0, 5, "nvnCommandBufferBindTexturesDeferred", "stage");
    if (first < 0 || count < 0 || first > 32 || count > 32 - first || (count && !handles))
        wonder_nvn_stop("NVN_INVALID_ARGUMENT nvnCommandBufferBindTexturesDeferred first=%d count=%d", first, count);
    const uint64_t a[] = {(uint64_t)stage, (uint64_t)first, (uint64_t)count};
    rec(cb, WONDER_NVN_CMD_BIND_TEXTURES, a, 3, handles, (size_t)count * 8);
}
void wonder_nvnCommandBufferBindImage(void *cb, int stage, int index, uint64_t handle) {
    need_range(stage, 0, 5, "nvnCommandBufferBindImage", "stage"); need_range(index, 0, 7, "nvnCommandBufferBindImage", "index");
    REC(cb, WONDER_NVN_CMD_BIND_IMAGE, (uint64_t)stage, (uint64_t)index, handle);
}
void wonder_nvnCommandBufferCallCommands(void *cb, int count, const uint64_t *handles) {
    if (count < 0 || count > 1024 || (count && !handles)) wonder_nvn_stop("NVN_INVALID_ARGUMENT nvnCommandBufferCallCommands count=%d", count);
    for (int i = 0; i < count; i++)
        if (!wonder_nvn_command_handle_valid(handles[i]))
            wonder_nvn_stop("NVN_INVALID_HANDLE nvnCommandBufferCallCommands handle[%d]=%#llx", i, (unsigned long long)handles[i]);
    // NVN copies the called handles' control entries (command memory address + size) into this command buffer, so
    // the call keeps the commands recorded so far even if the handle's control memory is re-recorded before the
    // queue runs it (round107: worker threads reuse handle addresses for the next frame during this frame's submit).
    struct call_segment { uint64_t cpu, bytes; } *list = NULL; size_t used = 0;
    pthread_mutex_lock(&handles_lock);
    for (int i = 0; i < count; i++)
        for (struct recorded_handle *p = recorded_handles; p; p = p->next) if (p->handle == handles[i]) {
            struct call_segment *grown = realloc(list, (used + p->count + 1) * sizeof *list);
            if (!grown) { pthread_mutex_unlock(&handles_lock); wonder_nvn_stop("NVN_HOST_OUT_OF_MEMORY CallCommands"); }
            list = grown;
            for (unsigned k = 0; k < p->count; k++) list[used++] = (struct call_segment){(uint64_t)(uintptr_t)p->segments[k].cpu, p->segments[k].bytes};
            break;
        }
    pthread_mutex_unlock(&handles_lock);
    const uint64_t a[] = {(uint64_t)count, (uint64_t)used};
    rec(cb, WONDER_NVN_CMD_CALL_COMMANDS, a, 2, list, used * sizeof *list); free(list);
    { static int on = -1; if (on < 0) on = getenv("WONDER_DRAW_LOG") != NULL;   // diagnostic: compare with NVN_CALL_REPLAY
      if (on) for (int i = 0; i < count; i++) wonder_nvn_report("NVN_CALL_RECORD command_buffer=%p handle=%#llx bytes=%ld", cb, (unsigned long long)handles[i], wonder_nvn_command_handle_bytes(handles[i])); }
}

// ---------- Clears, copies, draws, dispatch ----------
void wonder_nvnCommandBufferClearBuffer(void *cb, uint64_t address, size_t size, uint32_t value) {
    if (!address || !size || (size & 3) || (address & 3)) wonder_nvn_stop("NVN_INVALID_ARGUMENT nvnCommandBufferClearBuffer address=%#llx size=%#zx", (unsigned long long)address, size);
    need_address(address, size, "nvnCommandBufferClearBuffer");
    REC(cb, WONDER_NVN_CMD_CLEAR_BUFFER, address, size, value);
}
void wonder_nvnCommandBufferClearColor(void *cb, int index, const float *color, int mask) {
    need_range(index, 0, 7, "nvnCommandBufferClearColor", "index");
    if (!color || (unsigned)mask > 0xf) wonder_nvn_stop("NVN_INVALID_ARGUMENT nvnCommandBufferClearColor color=%p mask=%#x", (const void *)color, mask);
    const uint64_t a[] = {(uint64_t)index, (uint64_t)(unsigned)mask}; rec(cb, WONDER_NVN_CMD_CLEAR_COLOR, a, 2, color, 16);
}
void wonder_nvnCommandBufferClearDepthStencil(void *cb, float depth, uint32_t depth_mask, int stencil, int stencil_mask) {
    if (!finite(depth)) wonder_nvn_stop("NVN_INVALID_ARGUMENT nvnCommandBufferClearDepthStencil depth");
    REC(cb, WONDER_NVN_CMD_CLEAR_DEPTH_STENCIL, fbits(depth), (uint64_t)(depth_mask & 0xff), (uint64_t)(uint32_t)stencil, (uint64_t)(uint32_t)stencil_mask);
}
static void snapshot_texture(struct wonder_nvn_texture_snapshot *s, const void *texture, const void *view, const char *who) {
    memset(s, 0, sizeof *s);
    if (!texture) return;
    s->texture = *wonder_nvn_texture_of(texture, who); s->valid = 1;
    if (view) { memcpy(&s->view, view, sizeof s->view); s->has_view = 1; }
}
static void snapshot_region(struct wonder_nvn_copy_region *r, const void *region, const char *who) {
    if (!region) wonder_nvn_stop("NVN_INVALID_ARGUMENT %s region=NULL", who);
    memcpy(r, region, sizeof *r);
    if (r->x < 0 || r->y < 0 || r->z < 0 || r->width < 0 || r->height < 0 || r->depth < 0)
        wonder_nvn_stop("NVN_INVALID_ARGUMENT %s region %d,%d,%d %dx%dx%d", who, r->x, r->y, r->z, r->width, r->height, r->depth);
}
void wonder_nvnCommandBufferClearTexture(void *cb, const void *texture, const void *view, const void *region, const float *color, int mask) {
    const char *who = "nvnCommandBufferClearTexture";
    if (!texture || !color || (unsigned)mask > 0xf) wonder_nvn_stop("NVN_INVALID_ARGUMENT %s", who);
    struct { struct wonder_nvn_texture_snapshot t; struct wonder_nvn_copy_region r; float color[4]; } blob;
    snapshot_texture(&blob.t, texture, view, who); snapshot_region(&blob.r, region, who); memcpy(blob.color, color, 16);
    const uint64_t a[] = {(uint64_t)(unsigned)mask}; rec(cb, WONDER_NVN_CMD_CLEAR_TEXTURE, a, 1, &blob, sizeof blob);
}
void wonder_nvnCommandBufferCopyTextureToBuffer(void *cb, const void *texture, const void *view, const void *region, uint64_t address, int flags) {
    const char *who = "nvnCommandBufferCopyTextureToBuffer";
    if (!texture || !address) wonder_nvn_stop("NVN_INVALID_ARGUMENT %s", who);
    struct { struct wonder_nvn_texture_snapshot t; struct wonder_nvn_copy_region r; } blob;
    snapshot_texture(&blob.t, texture, view, who); snapshot_region(&blob.r, region, who); need_address(address, 1, who);
    const uint64_t a[] = {address, (uint64_t)(unsigned)flags}; rec(cb, WONDER_NVN_CMD_COPY_TEXTURE_TO_BUFFER, a, 2, &blob, sizeof blob);
}
void wonder_nvnCommandBufferCopyTextureToTexture(void *cb, const void *src, const void *src_view, const void *src_region,
                                                 const void *dst, const void *dst_view, const void *dst_region, int flags) {
    const char *who = "nvnCommandBufferCopyTextureToTexture";
    if (!src || !dst) wonder_nvn_stop("NVN_INVALID_ARGUMENT %s", who);
    struct { struct wonder_nvn_texture_snapshot s, d; struct wonder_nvn_copy_region sr, dr; } blob;
    snapshot_texture(&blob.s, src, src_view, who); snapshot_texture(&blob.d, dst, dst_view, who);
    snapshot_region(&blob.sr, src_region, who); snapshot_region(&blob.dr, dst_region, who);
    const uint64_t a[] = {(uint64_t)(unsigned)flags}; rec(cb, WONDER_NVN_CMD_COPY_TEXTURE_TO_TEXTURE, a, 1, &blob, sizeof blob);
}
void wonder_nvnCommandBufferDownsample(void *cb, const void *src, const void *dst) {
    const char *who = "nvnCommandBufferDownsample";
    if (!src || !dst) wonder_nvn_stop("NVN_INVALID_ARGUMENT %s", who);
    struct { struct wonder_nvn_texture_snapshot s, d; } blob;
    snapshot_texture(&blob.s, src, NULL, who); snapshot_texture(&blob.d, dst, NULL, who);
    rec(cb, WONDER_NVN_CMD_DOWNSAMPLE, NULL, 0, &blob, sizeof blob);
}
void wonder_nvnCommandBufferDispatchCompute(void *cb, int x, int y, int z) {
    if (x < 0 || y < 0 || z < 0) wonder_nvn_stop("NVN_INVALID_ARGUMENT nvnCommandBufferDispatchCompute %d,%d,%d", x, y, z);
    REC(cb, WONDER_NVN_CMD_DISPATCH, (uint64_t)x, (uint64_t)y, (uint64_t)z);
}
void wonder_nvnCommandBufferDispatchComputeIndirect(void *cb, uint64_t address) {
    need_address(address, 12, "nvnCommandBufferDispatchComputeIndirect"); REC(cb, WONDER_NVN_CMD_DISPATCH_INDIRECT, address);
}
static void draw_mode(int mode, const char *who) { need_range(mode, 0, 14, who, "mode"); }   // NVNdrawPrimitive POINTS..PATCHES
static void index_type(int type, const char *who) { need_range(type, 0, 2, who, "index_type"); }
void wonder_nvnCommandBufferDrawArrays(void *cb, int mode, int first, int count) {
    draw_mode(mode, "nvnCommandBufferDrawArrays");
    if (first < 0 || count < 0) wonder_nvn_stop("NVN_INVALID_ARGUMENT nvnCommandBufferDrawArrays first=%d count=%d", first, count);
    REC(cb, WONDER_NVN_CMD_DRAW_ARRAYS, (uint64_t)mode, (uint64_t)first, (uint64_t)count);
}
void wonder_nvnCommandBufferDrawArraysInstanced(void *cb, int mode, int first, int count, int base_instance, int instances) {
    draw_mode(mode, "nvnCommandBufferDrawArraysInstanced");
    if (first < 0 || count < 0 || base_instance < 0 || instances < 0) wonder_nvn_stop("NVN_INVALID_ARGUMENT nvnCommandBufferDrawArraysInstanced");
    REC(cb, WONDER_NVN_CMD_DRAW_ARRAYS_INSTANCED, (uint64_t)mode, (uint64_t)first, (uint64_t)count, (uint64_t)base_instance, (uint64_t)instances);
}
void wonder_nvnCommandBufferDrawElementsBaseVertex(void *cb, int mode, int type, int count, uint64_t address, int base_vertex) {
    draw_mode(mode, "nvnCommandBufferDrawElementsBaseVertex"); index_type(type, "nvnCommandBufferDrawElementsBaseVertex");
    if (count < 0) wonder_nvn_stop("NVN_INVALID_ARGUMENT nvnCommandBufferDrawElementsBaseVertex count=%d", count);
    REC(cb, WONDER_NVN_CMD_DRAW_ELEMENTS_BASE_VERTEX, (uint64_t)mode, (uint64_t)type, (uint64_t)count, address, (uint64_t)(int64_t)base_vertex);
}
void wonder_nvnCommandBufferDrawElementsIndirect(void *cb, int mode, int type, uint64_t index_address, uint64_t indirect_address) {
    draw_mode(mode, "nvnCommandBufferDrawElementsIndirect"); index_type(type, "nvnCommandBufferDrawElementsIndirect");
    need_address(indirect_address, 20, "nvnCommandBufferDrawElementsIndirect");
    REC(cb, WONDER_NVN_CMD_DRAW_ELEMENTS_INDIRECT, (uint64_t)mode, (uint64_t)type, index_address, indirect_address);
}
void wonder_nvnCommandBufferDrawElementsInstanced(void *cb, int mode, int type, int count, uint64_t address, int base_vertex, int base_instance, int instances) {
    draw_mode(mode, "nvnCommandBufferDrawElementsInstanced"); index_type(type, "nvnCommandBufferDrawElementsInstanced");
    if (count < 0 || base_instance < 0 || instances < 0) wonder_nvn_stop("NVN_INVALID_ARGUMENT nvnCommandBufferDrawElementsInstanced");
    REC(cb, WONDER_NVN_CMD_DRAW_ELEMENTS_INSTANCED, (uint64_t)mode, (uint64_t)type, (uint64_t)count, address, (uint64_t)(int64_t)base_vertex,
        (uint64_t)base_instance, (uint64_t)instances);
    trace_draw(cb, "DrawElementsInstanced", count);
}

// ---------- Counters / reports ----------
void wonder_nvnCommandBufferReportCounter(void *cb, int type, uint64_t address) {
    need_range(type, 0, 0x1f, "nvnCommandBufferReportCounter", "type");
    if (!address || (address & 15)) wonder_nvn_stop("NVN_INVALID_ARGUMENT nvnCommandBufferReportCounter address=%#llx (16-byte aligned)", (unsigned long long)address);
    need_address(address, 16, "nvnCommandBufferReportCounter");
    REC(cb, WONDER_NVN_CMD_REPORT_COUNTER, (uint64_t)type, address);
}
void wonder_nvnCommandBufferReportValue(void *cb, uint32_t value, uint64_t address) {
    if (!address || (address & 3)) wonder_nvn_stop("NVN_INVALID_ARGUMENT nvnCommandBufferReportValue address=%#llx", (unsigned long long)address);
    need_address(address, 4, "nvnCommandBufferReportValue");
    REC(cb, WONDER_NVN_CMD_REPORT_VALUE, value, address);
}
void wonder_nvnCommandBufferResetCounter(void *cb, int type) {
    need_range(type, 0, 0x1f, "nvnCommandBufferResetCounter", "type"); REC(cb, WONDER_NVN_CMD_RESET_COUNTER, (uint64_t)type);
}

// ---------- Fixed-function state ----------
void wonder_nvnCommandBufferSetAlphaRef(void *cb, float ref) {
    if (!finite(ref)) wonder_nvn_stop("NVN_INVALID_ARGUMENT nvnCommandBufferSetAlphaRef");
    REC(cb, WONDER_NVN_CMD_SET_ALPHA_REF, fbits(ref));
}
void wonder_nvnCommandBufferSetBlendColor(void *cb, const float *color) {
    if (!color) wonder_nvn_stop("NVN_INVALID_ARGUMENT nvnCommandBufferSetBlendColor color=NULL");
    rec(cb, WONDER_NVN_CMD_SET_BLEND_COLOR, NULL, 0, color, 16);
}
void wonder_nvnCommandBufferSetColorReductionEnable(void *cb, uint32_t enable) { REC(cb, WONDER_NVN_CMD_SET_COLOR_REDUCTION_ENABLE, (uint64_t)(enable & 0xff)); }
void wonder_nvnCommandBufferSetColorReductionThresholds(void *cb, int format_class, int t1, int t2) {
    need_range(format_class, 0, 15, "nvnCommandBufferSetColorReductionThresholds", "class");
    REC(cb, WONDER_NVN_CMD_SET_COLOR_REDUCTION_THRESHOLDS, (uint64_t)format_class, (uint64_t)(uint32_t)t1, (uint64_t)(uint32_t)t2);
}
void wonder_nvnCommandBufferSetConservativeRasterEnable(void *cb, uint32_t enable) { REC(cb, WONDER_NVN_CMD_SET_CONSERVATIVE_RASTER, (uint64_t)(enable & 0xff)); }
void wonder_nvnCommandBufferSetDepthBounds(void *cb, uint32_t enable, float lo, float hi) {
    if (!finite(lo) || !finite(hi)) wonder_nvn_stop("NVN_INVALID_ARGUMENT nvnCommandBufferSetDepthBounds");
    REC(cb, WONDER_NVN_CMD_SET_DEPTH_BOUNDS, (uint64_t)(enable & 0xff), fbits(lo), fbits(hi));
}
void wonder_nvnCommandBufferSetDepthClamp(void *cb, uint32_t enable) { REC(cb, WONDER_NVN_CMD_SET_DEPTH_CLAMP, (uint64_t)(enable & 0xff)); }
// Single forms map to the multi forms exactly as sdk+0x46c7c0/0x46c910 do (SetViewport converts ints to floats).
static void viewports(void *cb, unsigned opcode, int first, int count, const float *values, int per, const char *who) {
    if (first < 0 || count < 0 || first > 16 || count > 16 - first || (count && !values))
        wonder_nvn_stop("NVN_INVALID_ARGUMENT %s first=%d count=%d", who, first, count);
    for (int i = 0; i < count * per; i++) if (!finite(values[i])) wonder_nvn_stop("NVN_INVALID_ARGUMENT %s non-finite value", who);
    const uint64_t a[] = {(uint64_t)first, (uint64_t)count}; rec(cb, opcode, a, 2, values, (size_t)count * per * 4);
}
void wonder_nvnCommandBufferSetViewports(void *cb, int first, int count, const float *xywh) {
    viewports(cb, WONDER_NVN_CMD_SET_VIEWPORTS, first, count, xywh, 4, "nvnCommandBufferSetViewports");
}
void wonder_nvnCommandBufferSetViewport(void *cb, int x, int y, int w, int h) {
    const float v[4] = {(float)x, (float)y, (float)w, (float)h};
    viewports(cb, WONDER_NVN_CMD_SET_VIEWPORTS, 0, 1, v, 4, "nvnCommandBufferSetViewport");
}
void wonder_nvnCommandBufferSetDepthRanges(void *cb, int first, int count, const float *ranges) {
    viewports(cb, WONDER_NVN_CMD_SET_DEPTH_RANGES, first, count, ranges, 2, "nvnCommandBufferSetDepthRanges");
}
void wonder_nvnCommandBufferSetDepthRange(void *cb, float n, float f) {
    const float v[2] = {n, f}; viewports(cb, WONDER_NVN_CMD_SET_DEPTH_RANGES, 0, 1, v, 2, "nvnCommandBufferSetDepthRange");
}
void wonder_nvnCommandBufferSetScissors(void *cb, int first, int count, const int *rects) {
    if (first < 0 || count < 0 || first > 16 || count > 16 - first || (count && !rects))
        wonder_nvn_stop("NVN_INVALID_ARGUMENT nvnCommandBufferSetScissors first=%d count=%d", first, count);
    for (int i = 0; i < count; i++) if (rects[i * 4 + 2] < 0 || rects[i * 4 + 3] < 0) wonder_nvn_stop("NVN_INVALID_ARGUMENT nvnCommandBufferSetScissors size");
    const uint64_t a[] = {(uint64_t)first, (uint64_t)count}; rec(cb, WONDER_NVN_CMD_SET_SCISSORS, a, 2, rects, (size_t)count * 16);
}
void wonder_nvnCommandBufferSetScissor(void *cb, int x, int y, int w, int h) {
    const int r[4] = {x, y, w, h}; wonder_nvnCommandBufferSetScissors(cb, 0, 1, r);
}
void wonder_nvnCommandBufferSetLineWidth(void *cb, float w) {
    if (!finite(w) || w <= 0) wonder_nvn_stop("NVN_INVALID_ARGUMENT nvnCommandBufferSetLineWidth");
    REC(cb, WONDER_NVN_CMD_SET_LINE_WIDTH, fbits(w));
}
void wonder_nvnCommandBufferSetPatchSize(void *cb, int size) {
    need_range(size, 1, 32, "nvnCommandBufferSetPatchSize", "size"); REC(cb, WONDER_NVN_CMD_SET_PATCH_SIZE, (uint64_t)size);
}
void wonder_nvnCommandBufferSetPointSize(void *cb, float s) {
    if (!finite(s) || s <= 0) wonder_nvn_stop("NVN_INVALID_ARGUMENT nvnCommandBufferSetPointSize");
    REC(cb, WONDER_NVN_CMD_SET_POINT_SIZE, fbits(s));
}
void wonder_nvnCommandBufferSetPolygonOffsetClamp(void *cb, float factor, float units, float clamp) {
    if (!finite(factor) || !finite(units) || !finite(clamp)) wonder_nvn_stop("NVN_INVALID_ARGUMENT nvnCommandBufferSetPolygonOffsetClamp");
    REC(cb, WONDER_NVN_CMD_SET_POLYGON_OFFSET_CLAMP, fbits(factor), fbits(units), fbits(clamp));
}
void wonder_nvnCommandBufferSetPrimitiveRestart(void *cb, uint32_t enable, int index) {
    REC(cb, WONDER_NVN_CMD_SET_PRIMITIVE_RESTART, (uint64_t)(enable & 0xff), (uint64_t)(uint32_t)index);
}
void wonder_nvnCommandBufferSetRasterizerDiscard(void *cb, uint32_t discard) { REC(cb, WONDER_NVN_CMD_SET_RASTERIZER_DISCARD, (uint64_t)(discard & 0xff)); }
void wonder_nvnCommandBufferSetRenderEnable(void *cb, uint32_t enable) { REC(cb, WONDER_NVN_CMD_SET_RENDER_ENABLE, (uint64_t)(enable & 0xff)); }
void wonder_nvnCommandBufferSetRenderEnableConditional(void *cb, int mode, uint64_t address) {
    need_range(mode, 0, 1, "nvnCommandBufferSetRenderEnableConditional", "mode");
    if (!address || (address & 15)) wonder_nvn_stop("NVN_INVALID_ARGUMENT nvnCommandBufferSetRenderEnableConditional address=%#llx", (unsigned long long)address);
    need_address(address, 32, "nvnCommandBufferSetRenderEnableConditional");
    REC(cb, WONDER_NVN_CMD_SET_RENDER_ENABLE_CONDITIONAL, (uint64_t)mode, address);
}
void wonder_nvnCommandBufferSetRenderTargets(void *cb, int count, const void *const *colors, const void *const *color_views,
                                            const void *depth, const void *depth_view) {
    const char *who = "nvnCommandBufferSetRenderTargets";
    need_range(count, 0, 8, who, "count");
    if (count && !colors) wonder_nvn_stop("NVN_INVALID_ARGUMENT %s colors=NULL", who);
    struct wonder_nvn_texture_snapshot targets[9];
    for (int i = 0; i < count; i++) snapshot_texture(&targets[i], colors[i], color_views ? color_views[i] : NULL, who);
    snapshot_texture(&targets[count], depth, depth_view, who);
    const uint64_t a[] = {(uint64_t)count}; rec(cb, WONDER_NVN_CMD_SET_RENDER_TARGETS, a, 1, targets, sizeof targets[0] * (size_t)(count + 1));
}
void wonder_nvnCommandBufferSetSampleMask(void *cb, int mask) { REC(cb, WONDER_NVN_CMD_SET_SAMPLE_MASK, (uint64_t)(uint32_t)mask); }
void wonder_nvnCommandBufferSetSamplerPool(void *cb, const void *pool) {
    wonder_nvn_descriptor_pool_check(pool, 1, "nvnCommandBufferSetSamplerPool"); REC(cb, WONDER_NVN_CMD_SET_SAMPLER_POOL, (uintptr_t)pool);
}
void wonder_nvnCommandBufferSetTexturePool(void *cb, const void *pool) {
    wonder_nvn_descriptor_pool_check(pool, 0, "nvnCommandBufferSetTexturePool"); REC(cb, WONDER_NVN_CMD_SET_TEXTURE_POOL, (uintptr_t)pool);
}
void wonder_nvnCommandBufferSetShaderScratchMemory(void *cb, const void *pool, ptrdiff_t offset, size_t size) {
    struct wonder_nvn_pool_view view; wonder_nvn_pool_check(pool, "nvnCommandBufferSetShaderScratchMemory", &view);
    if (offset < 0 || (uint64_t)offset > view.size || size > view.size - (uint64_t)offset)
        wonder_nvn_stop("NVN_INVALID_ARGUMENT nvnCommandBufferSetShaderScratchMemory offset=%#tx size=%#zx", offset, size);
    REC(cb, WONDER_NVN_CMD_SET_SHADER_SCRATCH, (uintptr_t)pool, (uint64_t)offset, size);
}
// NVNface: FRONT 1, BACK 2, FRONT_AND_BACK 3.
void wonder_nvnCommandBufferSetStencilMask(void *cb, int face, int mask) {
    need_range(face, 1, 3, "nvnCommandBufferSetStencilMask", "face"); REC(cb, WONDER_NVN_CMD_SET_STENCIL_MASK, (uint64_t)face, (uint64_t)(uint32_t)mask);
}
void wonder_nvnCommandBufferSetStencilRef(void *cb, int face, int ref) {
    need_range(face, 1, 3, "nvnCommandBufferSetStencilRef", "face"); REC(cb, WONDER_NVN_CMD_SET_STENCIL_REF, (uint64_t)face, (uint64_t)(uint32_t)ref);
}
void wonder_nvnCommandBufferSetStencilValueMask(void *cb, int face, int mask) {
    need_range(face, 1, 3, "nvnCommandBufferSetStencilValueMask", "face"); REC(cb, WONDER_NVN_CMD_SET_STENCIL_VALUE_MASK, (uint64_t)face, (uint64_t)(uint32_t)mask);
}
// Tiled-cache controls are TX1 binning hints with no visible output (replay records, does not emulate).
void wonder_nvnCommandBufferSetTiledCacheAction(void *cb, int action) {
    need_range(action, 1, 8, "nvnCommandBufferSetTiledCacheAction", "action"); REC(cb, WONDER_NVN_CMD_SET_TILED_CACHE_ACTION, (uint64_t)action);
}
void wonder_nvnCommandBufferSetTiledCacheTileSize(void *cb, int w, int h) {
    if (w <= 0 || h <= 0) wonder_nvn_stop("NVN_INVALID_ARGUMENT nvnCommandBufferSetTiledCacheTileSize %dx%d", w, h);
    REC(cb, WONDER_NVN_CMD_SET_TILED_CACHE_TILE_SIZE, (uint64_t)w, (uint64_t)h);
}
// UpdateUniformBuffer inlines the data into the command stream (snapshot) and writes it in GPU order.
void wonder_nvnCommandBufferUpdateUniformBuffer(void *cb, uint64_t address, size_t buffer_size, ptrdiff_t offset, size_t size, const void *data) {
    const char *who = "nvnCommandBufferUpdateUniformBuffer";
    if (!address || buffer_size > 0x10000 || offset < 0 || (uint64_t)offset > buffer_size || size > buffer_size - (uint64_t)offset || (size && !data) || (size & 3) || (offset & 3))
        wonder_nvn_stop("NVN_INVALID_ARGUMENT %s address=%#llx buffer=%#zx offset=%#tx size=%#zx", who, (unsigned long long)address, buffer_size, offset, size);
    need_address(address, buffer_size, who);
    const uint64_t a[] = {address, buffer_size, (uint64_t)offset}; rec(cb, WONDER_NVN_CMD_UPDATE_UNIFORM_BUFFER, a, 3, data, size);
}
void wonder_nvnCommandBufferBarrier(void *cb, int bits) {
    // Bits 0..8: the vendored nvn.h stops at INVALIDATE_ZCULL (0x80); SDK 55 also handles bit 8 (sdk+0x478fc8).
    if ((unsigned)bits & ~0x1ffu) wonder_nvn_stop("NVN_INVALID_ARGUMENT nvnCommandBufferBarrier bits=%#x", bits);
    REC(cb, WONDER_NVN_CMD_BARRIER, (uint64_t)(unsigned)bits);
}

// ---------- End / handles / walk ----------
uint64_t wonder_nvnCommandBufferEndRecording(void *cmdbuf) {
    uint8_t *c = cmdbuf_of(cmdbuf, "nvnCommandBufferEndRecording");
    if (!c[0x11]) wonder_nvn_stop("NVN_INVALID_STATE nvnCommandBufferEndRecording not recording");
    uint8_t *cursor, *start, *control, *control_start, *limit, *command_base;
    memcpy(&cursor, c, 8); memcpy(&start, c + 0x40, 8);
    memcpy(&control, c + 0x20, 8); memcpy(&control_start, c + 0x48, 8); memcpy(&limit, c + 0x28, 8);
    memcpy(&command_base, c + 0x30, 8);
    uint8_t *command_end; memcpy(&command_end, c + 0x18, 8);
    if ((uintptr_t)command_end < (uintptr_t)command_base || (uintptr_t)start < (uintptr_t)command_base ||
        (uintptr_t)cursor < (uintptr_t)start || (uintptr_t)cursor > (uintptr_t)command_end)
        wonder_nvn_stop("NVN_INVALID_STATE EndRecording command cursor outside granted memory");
    uint64_t prior_group; memcpy(&prior_group, c + 0x50, 8);
    if (prior_group || ((uintptr_t)cursor - (uintptr_t)start) % 4)
        wonder_nvn_stop("NVN_UNSUPPORTED EndRecording native control groups pending");
    struct segment segments[MAX_SEGMENTS + 1]; unsigned count = 0;
    pthread_mutex_lock(&recordings_lock);
    struct recording *r = recording_of(cmdbuf, 0);
    if (r) { memcpy(segments, r->segments, r->count * sizeof *segments); count = r->count; r->count = 0; r->key = NULL; }
    pthread_mutex_unlock(&recordings_lock);
    if (cursor != start) segments[count++] = (struct segment){start, command_gpu(c, start), (size_t)(cursor - start)};
    for (unsigned i = 0; i < count; i++)
        if (segments[i].bytes / 4 >= (1u << 21)) wonder_nvn_stop("NVN_UNSUPPORTED EndRecording span over 2^21 words");
    size_t need = count ? 8 + 24 * (size_t)count + 8 : 8;
    if (!control || (uintptr_t)limit < (uintptr_t)control || need > (uintptr_t)limit - (uintptr_t)control)
        wonder_nvn_stop("NVN_CONTROL_MEMORY_EXHAUSTED EndRecording needs=%zu (control-segment link pending)", need);
    uint8_t *body = control_start + 16;
    if (control != body) wonder_nvn_stop("NVN_UNSUPPORTED EndRecording pre-existing native control records");
    if (count) {
        // SDK55 uncompressed handle form (sdk+0x4436a0..0x4436ec): group header type=3 with the span count in
        // the high word, then one 24-byte span {GPU address, CPU address, u32 words, u8 type 3} per segment.
        const uint64_t group = (uint64_t)count << 32 | 3;
        memcpy(control, &group, 8); control += 8;
        for (unsigned i = 0; i < count; i++) {
            memcpy(control, &segments[i].gpu, 8); memcpy(control + 8, &segments[i].cpu, 8);
            uint32_t words = (uint32_t)(segments[i].bytes / 4); memcpy(control + 16, &words, 4);
            memset(control + 20, 0, 4); control[20] = 3; control += 24;
        }
    }
    memset(control, 0, 8); control[0] = 2; control += 8; // End control marker.
    struct recorded_handle *entry = calloc(1, sizeof *entry);
    struct segment *copy = count ? malloc(count * sizeof *copy) : NULL;
    if (!entry || (count && !copy)) wonder_nvn_stop("NVN_HOST_OUT_OF_MEMORY EndRecording");
    if (count) memcpy(copy, segments, count * sizeof *copy);
    *entry = (struct recorded_handle){(uint64_t)(uintptr_t)body, count, copy, body, control, NULL};
    pthread_mutex_lock(&handles_lock);
    struct recorded_handle **slot = &recorded_handles;
    while (*slot && (*slot)->handle != entry->handle) slot = &(*slot)->next;
    if (*slot) { struct recorded_handle *old = *slot; entry->next = old->next; *slot = entry; free(old->segments); free(old); }
    else *slot = entry;
    pthread_mutex_unlock(&handles_lock);
    size_t bytes = 0; for (unsigned i = 0; i < count; i++) bytes += segments[i].bytes;
    command_pointer(c, 0x20, control); command_pointer(c, 0x40, cursor); command_pointer(c, 0x48, control);
    memset(c + 0x50, 0, 8); c[0x11] = c[0x12] = 0;
    wonder_nvn_report("NVN_RECORD_END command_buffer=%p handle=%p command_bytes=%zu segments=%u", cmdbuf, body, bytes, count);
    return (uint64_t)(uintptr_t)body; // Valid native uncompressed handle; no GPU work has executed.
}
// Command bytes the handle currently refers to (diagnostics); -1 = unknown handle.
long wonder_nvn_command_handle_bytes(uint64_t handle) {
    long bytes = -1;
    pthread_mutex_lock(&handles_lock);
    for (struct recorded_handle *p = recorded_handles; p; p = p->next)
        if (p->handle == handle) { bytes = 0; for (unsigned i = 0; i < p->count; i++) bytes += (long)p->segments[i].bytes; break; }
    pthread_mutex_unlock(&handles_lock);
    return bytes;
}
int wonder_nvn_command_handle_valid(uint64_t handle) {
    int found = 0;
    pthread_mutex_lock(&handles_lock);
    for (struct recorded_handle *p = recorded_handles; p; p = p->next) if (p->handle == handle) { found = 1; break; }
    pthread_mutex_unlock(&handles_lock);
    return found;
}
static void walk_segments(const struct segment *segments, unsigned count, uint64_t handle, wonder_nvn_command_visitor visitor, void *context);
void wonder_nvn_command_walk(uint64_t handle, wonder_nvn_command_visitor visitor, void *context) {
    struct segment segments[MAX_SEGMENTS + 1]; unsigned count = 0; int found = 0;
    pthread_mutex_lock(&handles_lock);
    for (struct recorded_handle *p = recorded_handles; p; p = p->next)
        if (p->handle == handle) { found = 1; count = p->count; memcpy(segments, p->segments, count * sizeof *segments); break; }
    pthread_mutex_unlock(&handles_lock);
    if (!found || !visitor) wonder_nvn_stop("NVN_INVALID_HANDLE command handle=%#llx", (unsigned long long)handle);
    walk_segments(segments, count, handle, visitor, context);
}
void wonder_nvn_call_walk(const struct wonder_nvn_cmd *call, wonder_nvn_command_visitor visitor, void *context) {
    if (call->nargs < 2 || call->blob_size != call->a[1] * 16) wonder_nvn_stop("NVN_INVALID_COMMAND CallCommands snapshot malformed");
    const uint8_t *blob = (const uint8_t *)(call->a + call->nargs);
    for (uint64_t i = 0; i < call->a[1]; i++) {
        struct segment one; uint64_t cpu, bytes; memcpy(&cpu, blob + i * 16, 8); memcpy(&bytes, blob + i * 16 + 8, 8);
        one.cpu = (uint8_t *)(uintptr_t)cpu; one.gpu = 0; one.bytes = (size_t)bytes;
        walk_segments(&one, 1, 0, visitor, context);
    }
}
static void walk_segments(const struct segment *segments, unsigned count, uint64_t handle, wonder_nvn_command_visitor visitor, void *context) {
    for (unsigned s = 0; s < count; s++) {
        const uint8_t *command = segments[s].cpu; size_t size = segments[s].bytes, offset = 0;
        while (offset < size) {
            uint32_t stub[2];
            if (size - offset < sizeof stub) wonder_nvn_stop("NVN_INVALID_COMMAND truncated record");
            memcpy(stub, command + offset, sizeof stub);
            if (stub[0] != WONDER_NVN_STUB_MAGIC)
                wonder_nvn_stop("NVN_NATIVE_PACKET_UNSUPPORTED handle=%#llx segment=%u offset=%#zx word=%#x", (unsigned long long)handle, s, offset, stub[0]);
            unsigned opcode = 0; size_t payload_size = 0;
            void *copy = wonder_cmdstore_get((uint64_t)(uintptr_t)(command + offset), stub[1], &opcode, &payload_size);
            if (!copy) wonder_nvn_stop("NVN_INVALID_COMMAND stale or unknown record handle=%#llx segment=%u offset=%#zx seq=%u", (unsigned long long)handle, s, offset, stub[1]);
            if (!opcode || opcode >= WONDER_NVN_CMD_COUNT) wonder_nvn_stop("NVN_INVALID_COMMAND record opcode=%u", opcode);
            if (opcode != WONDER_NVN_DRAW_ELEMENTS) {
                const struct wonder_nvn_cmd *cmd = copy;
                if (payload_size < sizeof *cmd || cmd->nargs > 64 || sizeof *cmd + cmd->nargs * 8 + cmd->blob_size > payload_size)
                    wonder_nvn_stop("NVN_INVALID_COMMAND %s malformed payload", wonder_nvn_command_name(opcode));
            }
            visitor(opcode, copy, payload_size, context);
            free(copy); offset += sizeof stub;
        }
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
    recording_forget(p);
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
    free(entry->segments); free(entry);
}

void wonder_nvnCommandBufferSetDebugLabel(void *p, const char *label) {
    cmdbuf_of(p, "nvnCommandBufferSetDebugLabel");wonder_nvn_report("NVN_COMMAND_BUFFER_LABEL object=%p label=%s",p,label?label:"");
}
static const char *const command_names[WONDER_NVN_CMD_COUNT] = {
    "?", "DrawElements", "BindBlendState", "BindChannelMaskState", "BindColorState", "BindDepthStencilState", "BindMultisampleState",
    "BindPolygonState", "BindVertexAttribState", "BindVertexStreamState", "BindProgram", "BindVertexBuffer", "BindUniformBuffer",
    "BindStorageBuffer", "BindTexture", "BindTexturesDeferred", "BindImage", "CallCommands", "ClearBuffer", "ClearColor",
    "ClearDepthStencil", "ClearTexture", "CopyTextureToBuffer", "CopyTextureToTexture", "DispatchCompute", "DispatchComputeIndirect",
    "Downsample", "DrawArrays", "DrawArraysInstanced", "DrawElementsBaseVertex", "DrawElementsIndirect", "DrawElementsInstanced",
    "ReportCounter", "ReportValue", "ResetCounter", "SetAlphaRef", "SetBlendColor", "SetColorReductionEnable",
    "SetColorReductionThresholds", "SetConservativeRasterEnable", "SetDepthBounds", "SetDepthClamp", "SetDepthRanges", "SetLineWidth",
    "SetPatchSize", "SetPointSize", "SetPolygonOffsetClamp", "SetPrimitiveRestart", "SetRasterizerDiscard", "SetRenderEnable",
    "SetRenderEnableConditional", "SetRenderTargets", "SetSampleMask", "SetSamplerPool", "SetTexturePool", "SetScissors",
    "SetShaderScratchMemory", "SetStencilMask", "SetStencilRef", "SetStencilValueMask", "SetTiledCacheAction", "SetTiledCacheTileSize",
    "SetViewports", "UpdateUniformBuffer", "Barrier",
};
_Static_assert(sizeof command_names / sizeof *command_names == WONDER_NVN_CMD_COUNT, "command names");
const char *wonder_nvn_command_name(unsigned opcode) { return opcode < WONDER_NVN_CMD_COUNT && command_names[opcode] ? command_names[opcode] : "?"; }

#define E(n) {#n, (void *)wonder_##n}
const struct wonder_nvn_entry wonder_nvn_command_entries[] = {
    E(nvnCommandBufferSetDebugLabel), E(nvnCommandBufferGetCommandMemorySize), E(nvnCommandBufferGetCommandMemoryUsed),
    E(nvnCommandBufferGetCommandMemoryFree), E(nvnCommandBufferGetControlMemorySize), E(nvnCommandBufferGetControlMemoryUsed),
    E(nvnCommandBufferGetControlMemoryFree), E(nvnCommandBufferIsRecording), E(nvnCommandBufferGetMemoryCallback),
    E(nvnCommandBufferGetMemoryCallbackData), E(nvnCommandBufferFinalize), E(nvnDeviceFinalizeCommandHandle),
    E(nvnCommandBufferDrawElements), E(nvnCommandBufferEndRecording), E(nvnCommandBufferBeginRecording),
    E(nvnCommandBufferAddCommandMemory), E(nvnCommandBufferAddControlMemory), E(nvnCommandBufferInitialize),
    E(nvnCommandBufferSetMemoryCallback), E(nvnCommandBufferSetMemoryCallbackData), E(nvnCommandBufferSetCommandMemoryCallbackEnabled),
    E(nvnCommandBufferBindBlendState), E(nvnCommandBufferBindChannelMaskState), E(nvnCommandBufferBindColorState),
    E(nvnCommandBufferBindDepthStencilState), E(nvnCommandBufferBindMultisampleState), E(nvnCommandBufferBindPolygonState),
    E(nvnCommandBufferBindVertexAttribState), E(nvnCommandBufferBindVertexStreamState), E(nvnCommandBufferBindProgram),
    E(nvnCommandBufferBindVertexBuffer), E(nvnCommandBufferBindUniformBuffer), E(nvnCommandBufferBindStorageBuffer),
    E(nvnCommandBufferBindTexture), E(nvnCommandBufferBindTexturesDeferred), E(nvnCommandBufferBindImage),
    E(nvnCommandBufferCallCommands), E(nvnCommandBufferClearBuffer), E(nvnCommandBufferClearColor),
    E(nvnCommandBufferClearDepthStencil), E(nvnCommandBufferClearTexture), E(nvnCommandBufferCopyTextureToBuffer),
    E(nvnCommandBufferCopyTextureToTexture), E(nvnCommandBufferDownsample), E(nvnCommandBufferDispatchCompute),
    E(nvnCommandBufferDispatchComputeIndirect), E(nvnCommandBufferDrawArrays), E(nvnCommandBufferDrawArraysInstanced),
    E(nvnCommandBufferDrawElementsBaseVertex), E(nvnCommandBufferDrawElementsIndirect), E(nvnCommandBufferDrawElementsInstanced),
    E(nvnCommandBufferReportCounter), E(nvnCommandBufferReportValue), E(nvnCommandBufferResetCounter),
    E(nvnCommandBufferSetAlphaRef), E(nvnCommandBufferSetBlendColor), E(nvnCommandBufferSetColorReductionEnable),
    E(nvnCommandBufferSetColorReductionThresholds), E(nvnCommandBufferSetConservativeRasterEnable), E(nvnCommandBufferSetDepthBounds),
    E(nvnCommandBufferSetDepthClamp), E(nvnCommandBufferSetViewport), E(nvnCommandBufferSetViewports),
    E(nvnCommandBufferSetDepthRange), E(nvnCommandBufferSetDepthRanges), E(nvnCommandBufferSetScissor), E(nvnCommandBufferSetScissors),
    E(nvnCommandBufferSetLineWidth), E(nvnCommandBufferSetPatchSize), E(nvnCommandBufferSetPointSize),
    E(nvnCommandBufferSetPolygonOffsetClamp), E(nvnCommandBufferSetPrimitiveRestart), E(nvnCommandBufferSetRasterizerDiscard),
    E(nvnCommandBufferSetRenderEnable), E(nvnCommandBufferSetRenderEnableConditional), E(nvnCommandBufferSetRenderTargets),
    E(nvnCommandBufferSetSampleMask), E(nvnCommandBufferSetSamplerPool), E(nvnCommandBufferSetTexturePool),
    E(nvnCommandBufferSetShaderScratchMemory), E(nvnCommandBufferSetStencilMask), E(nvnCommandBufferSetStencilRef),
    E(nvnCommandBufferSetStencilValueMask), E(nvnCommandBufferSetTiledCacheAction), E(nvnCommandBufferSetTiledCacheTileSize),
    E(nvnCommandBufferUpdateUniformBuffer), E(nvnCommandBufferBarrier),
};
const unsigned wonder_nvn_command_count = sizeof wonder_nvn_command_entries / sizeof *wonder_nvn_command_entries;
