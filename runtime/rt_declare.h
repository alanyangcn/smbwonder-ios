#pragma once
// Time-constraint (real-time) declarations of the frame threads (host_graphics.m: the game thread that acquires and
// presents, and the host replay thread). iOS learns a thread's frame deadline and per-frame work only from this
// declaration: period = constraint = the present period, computation = the CPU time the thread really uses per frame
// (a real-time thread that runs past its declared computation is demoted; the performance controller sets clocks from
// it). Pure bookkeeping here, no Mach calls: the computation starts at 2 ms, then every 120 frames becomes 1.25 x the
// p90 of the measured per-frame CPU time, re-declared when it moved by more than 15 %, kept within [1 ms, 0.9 period].
#include <stdint.h>
#define WONDER_RT_WINDOW 120
struct wonder_rt_decl {
    uint64_t period_ns, comp_ns;   // declared (0 = not yet)
    uint64_t last_cpu_ns; int have_last;
    double win[WONDER_RT_WINDOW]; int n;
};
uint64_t wonder_rt_clamp(uint64_t comp_ns, uint64_t period_ns);
// One frame of the thread: cpu_ns = its CPU clock now. Returns the computation to declare now (period changed, first
// frame, or the p90 moved), 0 = keep the current declaration. fixed_ns > 0 pins the computation (WONDER_RT_COMP).
uint64_t wonder_rt_tick(struct wonder_rt_decl *d, uint64_t cpu_ns, uint64_t period_ns, uint64_t fixed_ns);
