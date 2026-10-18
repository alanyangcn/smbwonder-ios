// HOST_ENERGY: every 300 presents (host_graphics.m), the device thermal state, Low Power Mode, process CPU use and the
// threads that used the most CPU over the window. Threads are named (guest g<handle>, replay, game main) so the lines
// and Instruments traces say which work burns the time; HOST_THREAD_CREATE maps a guest handle to its entry point.
#import <Foundation/Foundation.h>
#include <mach/mach.h>
#include <stdio.h>
#include <string.h>
#define MAX_THREADS 256
#define TOP 8
void wonder_energy_report(double window_s) {
    static struct { uint64_t id, cpu_us; } last[MAX_THREADS]; static int last_n;
    struct { uint64_t id, delta_us; char name[64]; } now[MAX_THREADS]; int n = 0;
    thread_act_array_t list; mach_msg_type_number_t count = 0;
    if (task_threads(mach_task_self(), &list, &count) != KERN_SUCCESS) return;
    uint64_t total_us = 0; uint64_t ids[MAX_THREADS], cpu[MAX_THREADS]; int m = 0;
    for (mach_msg_type_number_t i = 0; i < count; i++) {
        thread_extended_info_data_t ext; mach_msg_type_number_t c1 = THREAD_EXTENDED_INFO_COUNT;
        thread_identifier_info_data_t ident; mach_msg_type_number_t c2 = THREAD_IDENTIFIER_INFO_COUNT;
        if (thread_info(list[i], THREAD_EXTENDED_INFO, (thread_info_t)&ext, &c1) == KERN_SUCCESS &&
            thread_info(list[i], THREAD_IDENTIFIER_INFO, (thread_info_t)&ident, &c2) == KERN_SUCCESS && m < MAX_THREADS) {
            uint64_t us = (ext.pth_user_time + ext.pth_system_time) / 1000;   // ns -> us
            uint64_t before = 0; int known = 0;
            for (int k = 0; k < last_n; k++) if (last[k].id == ident.thread_id) { before = last[k].cpu_us; known = 1; break; }
            uint64_t d = known && us >= before ? us - before : us;
            total_us += d; ids[m] = ident.thread_id; cpu[m] = us; m++;
            if (n < MAX_THREADS) { now[n].id = ident.thread_id; now[n].delta_us = d; snprintf(now[n].name, sizeof now[n].name, "%s", ext.pth_name[0] ? ext.pth_name : "?"); n++; }
        }
        mach_port_deallocate(mach_task_self(), list[i]);
    }
    vm_deallocate(mach_task_self(), (vm_address_t)list, count * sizeof *list);
    for (int k = 0; k < m; k++) { last[k].id = ids[k]; last[k].cpu_us = cpu[k]; } last_n = m;
    for (int a = 0; a < n; a++) for (int b = a + 1; b < n; b++) if (now[b].delta_us > now[a].delta_us) { __typeof__(now[0]) t = now[a]; now[a] = now[b]; now[b] = t; }
    static const char *thermal[] = {"nominal", "fair", "serious", "critical"};
    NSProcessInfoThermalState ts = NSProcessInfo.processInfo.thermalState;
    char top[1024] = ""; size_t used = 0;
    for (int k = 0; k < n && k < TOP && window_s > 0; k++)
        used += (size_t)snprintf(top + used, sizeof top - used, "%s %s=%.0f%%", k ? "," : "", now[k].name, now[k].delta_us / 1e4 / window_s);
    fprintf(stderr, "HOST_ENERGY thermal=%s low_power=%d threads=%u cpu=%.0f%% (100%% = one core) top:%s\n", ts <= 3 ? thermal[ts] : "?",
            NSProcessInfo.processInfo.lowPowerModeEnabled, count, window_s > 0 ? total_us / 1e4 / window_s : 0.0, top);
}
