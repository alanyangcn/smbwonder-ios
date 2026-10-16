#pragma once
#include <stdint.h>
// Thread / synchronization / event SVCs (kernel_threads.c). Returns 1 when handled (result in r[0]).
int wonder_event_call(int number, uint64_t *r);
// Host-side signal of an event write end (e.g. CADisplayLink vsync).
uint32_t wonder_event_signal(uint32_t handle);
// Register the calling host thread as a guest thread; the first call also installs the per-thread
// guest preparation, the guest entry trampoline and the log sink. Returns its handle (0x1001 first).
uint32_t wonder_threads_register_current(int prio, int core, void (*prepare)(void),
                                         void (*enter)(uint64_t entry, uint64_t arg, uint64_t sp),
                                         void (*reporter)(const char *line));
// Host kernel objects behind ordinary handles (CloseHandle invokes close(obj)).
uint32_t wonder_handle_new_object(uint32_t kind, void *obj, void (*close)(void *));
void *wonder_handle_object(uint32_t handle, uint32_t kind);
// Diagnostics: log every guest thread blocked longer than min_ns (KERNEL_WAIT lines), frames formatted by the hook.
#include <stddef.h>
void wonder_kernel_dump_waits(int64_t min_ns, void (*frames)(uint64_t fp, uint64_t lr, char *out, size_t size));
