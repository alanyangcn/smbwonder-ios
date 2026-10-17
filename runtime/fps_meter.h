#pragma once
// On-screen frame rate (round216, at the player's request): presents counted over half-second windows. The present
// thread feeds it; the UI reads the last finished window as tenths of a frame per second.
struct wonder_fps_meter { double start; unsigned count; };
// One present at time t (seconds, monotonic). Returns the rate of the window it closes (tenths of fps), or -1 while the
// window is still open. A gap of more than 2 s (a load screen that stopped presenting) restarts the window.
int wonder_fps_meter_present(struct wonder_fps_meter *m, double t);
// Process-wide rate the UI shows: last value published by the present path (-1 = no present yet).
void wonder_fps_publish(int tenths);
int wonder_fps_current(void);
