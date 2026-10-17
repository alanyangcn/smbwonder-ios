// On-screen frame rate: half-second windows of presents (round216).
#include "fps_meter.h"
#include <assert.h>
#include <stdio.h>
static int run(struct wonder_fps_meter *m, double *t, double step, int n) {   // n presents step seconds apart; last result
    int r = -1; for (int i = 0; i < n; i++) { int v = wonder_fps_meter_present(m, *t); if (v >= 0) r = v; *t += step; } return r;
}
int main(void) {
    struct wonder_fps_meter m = {0}; double t = 100;
    assert(wonder_fps_meter_present(&m, t) == -1);            // first present opens a window
    t += 1.0 / 60; assert(run(&m, &t, 1.0 / 60, 72) == 600);  // 60 fps
    assert(run(&m, &t, 1.0 / 22, 27) == 220);                 // 22 fps
    assert(run(&m, &t, 1.0 / 30, 36) == 300);
    t += 5; assert(wonder_fps_meter_present(&m, t) == -1);    // load screen gap: the window restarts, no bogus low value
    t += 0.04; assert(run(&m, &t, 0.04, 30) == 250);
    assert(wonder_fps_current() == -1); wonder_fps_publish(223); assert(wonder_fps_current() == 223);
    puts("fps_meter OK");
    return 0;
}
