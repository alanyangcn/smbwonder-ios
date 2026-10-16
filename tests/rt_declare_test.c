#include "rt_declare.h"
#include <assert.h>
#include <stdio.h>
#define MS 1000000ull
int main(void) {
    const uint64_t p60 = 16666667ull;
    assert(wonder_rt_clamp(0, p60) == 1 * MS && wonder_rt_clamp(20 * MS, p60) == p60 * 9 / 10 && wonder_rt_clamp(5 * MS, p60) == 5 * MS);
    struct wonder_rt_decl d = {0}; uint64_t cpu = 1000 * MS;
    assert(wonder_rt_tick(&d, cpu, 0, 0) == 0);                    // no period known: nothing declared
    assert(wonder_rt_tick(&d, cpu, p60, 0) == 2 * MS);             // first frame: 2 ms
    // 120 frames of 8 ms (p90 8 ms -> 10 ms declared), then nothing changes while the load stays.
    uint64_t got = 0;
    for (int i = 0; i < 120; i++) { cpu += 8 * MS; uint64_t r = wonder_rt_tick(&d, cpu, p60, 0); if (r) { assert(i == 119); got = r; } }
    assert(got == 10 * MS && d.comp_ns == 10 * MS);
    for (int i = 0; i < 120; i++) { cpu += 8 * MS + (i % 3) * 100000; assert(wonder_rt_tick(&d, cpu, p60, 0) == 0); }   // within 15 %
    // The p90 ignores the slowest 10 % (a load hitch), and a heavy thread is capped at 0.9 period.
    for (int i = 0; i < 120; i++) { cpu += (i < 12 ? 40 : 4) * MS; got = wonder_rt_tick(&d, cpu, p60, 0); }
    assert(got == 5 * MS);
    for (int i = 0; i < 120; i++) { cpu += 30 * MS; got = wonder_rt_tick(&d, cpu, p60, 0); }
    assert(got == p60 * 9 / 10);
    // A new period (30 fps interval) re-declares at once from the current computation, clamped.
    assert(wonder_rt_tick(&d, cpu, 2 * p60, 0) == p60 * 9 / 10);
    // Fixed computation (WONDER_RT_COMP): declared once, never moved by measurements.
    struct wonder_rt_decl f = {0}; cpu = 0;
    assert(wonder_rt_tick(&f, cpu, p60, 6 * MS) == 6 * MS);
    for (int i = 0; i < 500; i++) { cpu += 12 * MS; assert(wonder_rt_tick(&f, cpu, p60, 6 * MS) == 0); }
    puts("RT_DECLARE_OK");
}
