#include "fps_meter.h"
#include <stdatomic.h>
#define WINDOW 0.5
int wonder_fps_meter_present(struct wonder_fps_meter *m, double t) {
    if (!m->count || t < m->start || t - m->start > 2.0 + WINDOW) { m->start = t; m->count = 1; return -1; }
    m->count++;
    double span = t - m->start;
    if (span < WINDOW) return -1;
    int tenths = (int)((m->count - 1) * 10.0 / span + 0.5);   // count - 1 frame intervals between the first and this present
    m->start = t; m->count = 1;
    return tenths;
}
static _Atomic int current = -1;
void wonder_fps_publish(int tenths) { atomic_store(&current, tenths); }
int wonder_fps_current(void) { return atomic_load(&current); }
