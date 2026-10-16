#include "rt_declare.h"
#include <stdlib.h>
#include <string.h>
uint64_t wonder_rt_clamp(uint64_t comp_ns, uint64_t period_ns) {
    uint64_t cap = period_ns * 9 / 10;
    if (comp_ns < 1000000u) comp_ns = 1000000u;
    if (comp_ns > cap) comp_ns = cap;
    return comp_ns;
}
static int cmp(const void *a, const void *b) { double x = *(const double *)a, y = *(const double *)b; return x < y ? -1 : x > y; }
uint64_t wonder_rt_tick(struct wonder_rt_decl *d, uint64_t cpu_ns, uint64_t period_ns, uint64_t fixed_ns) {
    if (!period_ns) return 0;
    uint64_t prev = d->last_cpu_ns; int have = d->have_last; d->last_cpu_ns = cpu_ns; d->have_last = 1;
    if (d->period_ns != period_ns) {   // first frame or a new present interval: start over
        d->period_ns = period_ns; d->n = 0;
        d->comp_ns = wonder_rt_clamp(fixed_ns ? fixed_ns : (d->comp_ns ? d->comp_ns : 2000000u), period_ns);
        return d->comp_ns;
    }
    if (fixed_ns || !have || cpu_ns < prev) return 0;
    d->win[d->n++] = (double)(cpu_ns - prev);
    if (d->n < WONDER_RT_WINDOW) return 0;
    double v[WONDER_RT_WINDOW]; memcpy(v, d->win, sizeof v); qsort(v, WONDER_RT_WINDOW, sizeof *v, cmp); d->n = 0;
    uint64_t comp = wonder_rt_clamp((uint64_t)(v[WONDER_RT_WINDOW * 9 / 10 - 1] * 1.25), period_ns);
    if ((double)comp > d->comp_ns * 1.15 || (double)comp < d->comp_ns * 0.85) { d->comp_ns = comp; return comp; }
    return 0;
}
