// Guest boundary for NVN: nvnBootstrapLoader is taken over so the console NVN driver in sdk never runs.
// Implemented names get typed host entries (nvn_device.c); every other loaded name gets a stop thunk.
#include <stdatomic.h>
#include "hle.h"
#include "nvn_device.h"
#include "nvn_names.h"
#include "log_throttle.h"
#include <stdarg.h>
#include <mach/mach.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <unistd.h>
#include <stdlib.h>
#include <string.h>
#include <errno.h>
_Noreturn void wonder_game_thread_stop(const char *reason);
extern const uint64_t wonder_nvn_unimpl_stubs[WONDER_NVN_NAME_COUNT];

// Per-call NVN reports are rate limited (log_throttle.h); WONDER_NVN_VERBOSE=1 prints every line.
void wonder_nvn_report(const char *format, ...) {
    static _Thread_local char line[2048];   // off the caller's stack: guest threads may run on small stacks
    va_list args; va_start(args, format); vsnprintf(line, sizeof line, format, args); va_end(args);
    static int verbose = -1; if (verbose < 0) { const char *v = getenv("WONDER_NVN_VERBOSE"); verbose = v && *v && strcmp(v, "0"); }
    unsigned long count = 0;
    if (verbose || wonder_log_throttle(line, &count)) {
        if (count > WONDER_LOG_FIRST) LOG("%s (occurrence %lu)", line, count); else LOG("%s", line);
    }
}
static void caller_chain(uint64_t fp, uint64_t pc) {
    for (unsigned depth = 0; depth < 12; depth++) {
        module_t *m = module_of((void *)pc);
        if (m) LOG("NVN_CALLER depth=%u %s+%#llx", depth, m->name, (unsigned long long)(pc - (uint64_t)m->base));
        uint64_t frame[2]; vm_size_t count = 0;
        if (!fp || (fp & 15) || vm_read_overwrite(mach_task_self(), fp, sizeof frame, (vm_address_t)frame, &count) != KERN_SUCCESS ||
            count != sizeof frame || frame[0] <= fp) break;
        fp = frame[0]; pc = frame[1];
    }
}
// Round123/124 diagnostic: main+0x107fc00 (the game's buffer-builder wrapper) passed a NULL pool. Its caller
// (main+0x10cf930, returning to main+0x10cfae0) holds in x21 a pointer to {pool, base} at object+0x178; the object's
// +0x100 is the buffer address and +0x108 the pool/allocator the game found for it (NULL here). The wrapper saves x21 at
// fp+0x28 (stp x22, x21, [sp, #0x60] with fp = sp+0x40). Logs those fields and the host pool holding the address.
static int guest_read(uint64_t address, void *out, size_t size) {
    vm_size_t count = 0;
    return vm_read_overwrite(mach_task_self(), address, size, (vm_address_t)out, &count) == KERN_SUCCESS && count == size;
}
void wonder_nvn_diag_null_pool(void) {
    uint64_t fp = (uint64_t)__builtin_frame_address(0), frame[2];
    for (unsigned depth = 0; depth < 16 && fp && !(fp & 15) && guest_read(fp, frame, sizeof frame); depth++) {
        module_t *m = module_of((void *)frame[1]);
        if (m && !strcmp(m->name, "main") && frame[1] - (uint64_t)m->base == 0x10cfae0) {
            uint64_t holder = 0, obj[0x190 / 8] = {0};   // object +0x100 .. +0x290 (flag at +0x288)
            if (!guest_read(fp + 0x28, &holder, 8) || !holder) { LOG("NVN_NULL_POOL_DIAG no saved x21 at fp=%#llx", (unsigned long long)fp); return; }
            uint64_t object = holder - 0x178; int ok = guest_read(object + 0x100, obj, 0x190);
            uint64_t address = obj[0], allocator = obj[1], base = 0, size = 0, info = 0, info_base = 0;
            if (allocator && guest_read(allocator + 0x110, &info, 8) && info) guest_read(info + 0x18, &info_base, 8);
            const void *pool = ok ? wonder_nvn_pool_containing(address, &base, &size) : NULL;
            LOG("NVN_NULL_POOL_DIAG object=%#llx read=%d address(+0x100)=%#llx allocator(+0x108)=%#llx holder(+0x178)={%#llx,%#llx} flag(+0x288)=%#llx",
                (unsigned long long)object, ok, (unsigned long long)address, (unsigned long long)allocator,
                (unsigned long long)obj[0x78 / 8 + 0], (unsigned long long)obj[0x78 / 8 + 1], (unsigned long long)(obj[0x188 / 8] & 0xff));
            LOG("NVN_NULL_POOL_DIAG host pool holding the address: %p base=%#llx size=%#llx; allocator info=%#llx base=%#llx",
                pool, (unsigned long long)base, (unsigned long long)size, (unsigned long long)info, (unsigned long long)info_base);
            return;
        }
        if (frame[0] <= fp) break;
        fp = frame[0];
    }
    LOG("NVN_NULL_POOL_DIAG caller frame main+0x10cfae0 not found");
}
_Noreturn void wonder_nvn_stop(const char *format, ...) {
    char reason[512]; va_list args; va_start(args, format); vsnprintf(reason, sizeof reason, format, args); va_end(args);
    LOG("NVN_BLOCKED %s", reason);
    caller_chain((uint64_t)__builtin_frame_address(0), (uint64_t)__builtin_return_address(0));
    wonder_game_thread_stop(reason);
}
void *wonder_host_reserve(size_t size) {
    void *p = mmap(NULL, size, PROT_NONE, MAP_PRIVATE | MAP_ANON, -1, 0);
    return p == MAP_FAILED ? NULL : p;
}
void wonder_host_unreserve(void *memory, size_t size) { if (memory && size && munmap(memory, size)) die("munmap virtual pool %p+%#zx failed", memory, size); }
void *wonder_nvn_unimplemented_thunk(unsigned index) {
    if (index >= WONDER_NVN_NAME_COUNT) die("NVN thunk index %u out of range", index);
    return (void *)&wonder_nvn_unimpl_stubs[index];
}
// Capture before the strict stop. GPU tokens are resolved through registered pool storage,
// never cast to CPU pointers (virtual pools reserve PROT_NONE ranges).
static int capture_bytes(const char *directory, unsigned stage, const char *kind, const void *bytes, size_t size) {
    char path[1024];
    if (snprintf(path, sizeof path, "%s/stage-%u.%s.bin", directory, stage, kind) >= sizeof path) return 0;
    FILE *f = fopen(path, "wb"); if (!f) return 0;
    int ok = fwrite(bytes, 1, size, f) == size; if (fflush(f) || fsync(fileno(f))) ok = 0;
    if (fclose(f)) ok = 0; return ok;
}
static int capture_read(uint64_t address, void *bytes, size_t size) {
    vm_size_t count = 0;
    return address && vm_read_overwrite(mach_task_self(), address, size, (vm_address_t)bytes, &count) == KERN_SUCCESS && count == size;
}
// The first program with shaders is captured completely enough for tools/match_shader_capture.py
// (manifest + 0x880-byte control and 256-byte code prefix per stage); later programs are logged.
void wonder_nvn_capture_shaders(const void *program, const struct wonder_nvn_program_shaders *shaders) {
    static _Atomic int captured;
    if (atomic_exchange(&captured, 1)) return;
    const char *home = getenv("HOME"); char directory[1024], path[1100];
    if (!home || snprintf(directory, sizeof directory, "%s/Documents/ShaderCapture", home) >= sizeof directory) return;
    if (mkdir(directory, 0700) && errno != EEXIST) return;
    snprintf(path, sizeof path, "%s/manifest.json", directory);
    FILE *manifest = fopen(path, "w"); if (!manifest) return;
    unsigned count = (unsigned)__builtin_popcount(shaders->mask);
    fprintf(manifest, "{\"pid\":%d,\"program\":%llu,\"count\":%u,\"stages\":[", getpid(), (unsigned long long)(uintptr_t)program, count);
    int ok = 1; unsigned index = 0;
    for (unsigned stage = 0; stage < 6 && ok; stage++) {
        if (!(shaders->mask & (1u << stage))) continue;
        uint8_t control[0x880], code[256]; struct wonder_nvn_pool_view view;
        if (!capture_read((uintptr_t)shaders->control[stage], control, sizeof control)) {ok = 0; break;}
        wonder_nvn_address_span(shaders->code[stage], sizeof code, &view);
        if (view.size < sizeof code || !capture_read((uintptr_t)view.memory, code, sizeof code) ||
            !capture_bytes(directory, index, "control", control, sizeof control) || !capture_bytes(directory, index, "code", code, sizeof code)) {ok = 0; break;}
        fprintf(manifest, "%s{\"index\":%u,\"stage\":%u,\"address\":%llu,\"control_address\":%llu}", index ? "," : "", index, stage,
                (unsigned long long)shaders->code[stage], (unsigned long long)(uintptr_t)shaders->control[stage]);
        index++;
    }
    fprintf(manifest, "],\"complete\":%s}\n", ok ? "true" : "false");
    if (fflush(manifest) || fsync(fileno(manifest))) ok = 0; if (fclose(manifest)) ok = 0;
    LOG("NVN_SHADER_CAPTURE pid=%d stages=%u complete=%d directory=Documents/ShaderCapture", getpid(), count, ok);
}
// First-draw census sink (nvn_draw_census.h): Documents/DrawCensus/<name>, fsynced.
int wonder_host_census_write(const char *name, const void *data, size_t size) {
    const char *home = getenv("HOME"); char directory[1024];
    if (!home || snprintf(directory, sizeof directory, "%s/Documents/DrawCensus", home) >= (int)sizeof directory) return -1;
    if (mkdir(directory, 0700) && errno != EEXIST) return -1;
    char path[1200]; if (snprintf(path, sizeof path, "%s/%s", directory, name) >= (int)sizeof path) return -1;
    char *slash = strrchr(path, '/');   // per-program subdirectory (<vs>-<fs>/...)
    if (slash && slash > path + strlen(directory)) { *slash = 0; if (mkdir(path, 0700) && errno != EEXIST) return -1; *slash = '/'; }
    FILE *f = fopen(path, "wb"); if (!f) return -1;
    int ok = fwrite(data, 1, size, f) == size; if (fflush(f) || fsync(fileno(f))) ok = 0; if (fclose(f)) ok = 0;
    if (ok) LOG("NVN_DRAW_CENSUS wrote Documents/DrawCensus/%s (%zu bytes)", name, size);
    return ok ? 0 : -1;
}
// frame[0]=x29 frame[1]=x30 (guest return address) frame[2..9]=x0..x7, saved by nvn_unimpl.S.
_Noreturn void wonder_nvn_unimplemented(unsigned index, const uint64_t *frame) {
    if (index >= WONDER_NVN_NAME_COUNT) die("NVN thunk index %u out of range", index);
    module_t *m = module_of((void *)frame[1]);
    char reason[512];
    snprintf(reason, sizeof reason, "NVN_UNIMPLEMENTED %s (main slot %#x) caller=%s+%#llx x0=%#llx x1=%#llx x2=%#llx x3=%#llx",
             wonder_nvn_names[index].name, wonder_nvn_names[index].slot, m ? m->name : "?",
             m ? (unsigned long long)(frame[1] - (uint64_t)m->base) : (unsigned long long)frame[1],
             (unsigned long long)frame[2], (unsigned long long)frame[3], (unsigned long long)frame[4], (unsigned long long)frame[5]);
    LOG("NVN_BLOCKED %s", reason);
    caller_chain(frame[0], frame[1]);
    wonder_game_thread_stop(reason);
}
HLE_FORCE("nvnBootstrapLoader", "Host NVN entry table; the console NVN driver in sdk is never executed", void *, h_nvn_bootstrap, const char *name) {
    void *fn = wonder_nvn_resolve(name, "nvnBootstrapLoader");
    LOG("NVN_BOOTSTRAP name=%s -> %s", name ? name : "(null)",
        !fn ? "NULL" : (fn >= (void *)wonder_nvn_unimpl_stubs && fn < (void *)(wonder_nvn_unimpl_stubs + WONDER_NVN_NAME_COUNT)) ? "stop-thunk" : "host");
    return fn;
}
