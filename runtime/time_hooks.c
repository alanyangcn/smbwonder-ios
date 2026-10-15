// nn::time on the iPhone clock. The time:u/time:a services keep the user/network system clocks and the device
// time zone; on this host they are the iOS clock (network-synchronized by the system) and the iOS time zone.
// PosixTime = seconds since 1970-01-01 UTC. CalendarTime = {s16 year; s8 month 1..12; s8 day; s8 hour; s8 minute;
// s8 second; pad}. CalendarAdditionalInfo = {s32 day of week 0=Sunday; s32 day of year 0-based; TimeZone
// {char standard_time_name[8]; bool daylight; s32 utc_offset_seconds}}. LocationName = char[0x24] (IANA name).
#include "hle.h"
#include <CoreFoundation/CoreFoundation.h>
#include <stdatomic.h>
#include <string.h>
#include <time.h>
#include <pthread.h>
#include <stdlib.h>
#define RESULT_NOT_INITIALIZED 0x1f474u   // time module 116, description 0x3fa (service used before Initialize)
static _Atomic int time_initialized;
static void *required(const char *name) { void *p = guest_sym(name); if (!p) die("time missing SDK entry %s", name); return p; }

struct calendar_time { int16_t year; int8_t month, day, hour, minute, second, pad; };
struct calendar_additional { int32_t day_of_week, day_of_year; char zone_name[8]; uint8_t daylight, pad[3]; int32_t utc_offset; };
_Static_assert(sizeof(struct calendar_time) == 8 && sizeof(struct calendar_additional) == 0x18, "nn::time layouts");

HLE_FORCE("_ZN2nn4time10InitializeEv", "Host clock and time zone instead of time:u", Result, h_time_init, void) {
    atomic_store(&time_initialized, 1);
    CFTimeZoneRef tz = CFTimeZoneCopySystem(); char name[64] = "UTC";
    if (tz) { CFStringGetCString(CFTimeZoneGetName(tz), name, sizeof name, kCFStringEncodingUTF8); CFRelease(tz); }
    LOG("HOST_TIME_READY now=%lld zone=%s", (long long)time(NULL), name);
    return 0;
}
HLE_FORCE("_ZN2nn4time8FinalizeEv", "Host clock: nothing to release", Result, h_time_fini, void) { atomic_store(&time_initialized, 0); return 0; }
HLE_FORCE("_ZN2nn4time13IsInitializedEv", "Host clock initialization state", uint32_t, h_time_is_init, void) { return (uint32_t)atomic_load(&time_initialized); }
static Result current_time(int64_t *out, const char *who) {
    if (!out) die("time %s out=NULL", who);
    if (!atomic_load(&time_initialized)) return RESULT_NOT_INITIALIZED;
    *out = (int64_t)time(NULL); return 0;
}
HLE_FORCE("_ZN2nn4time23StandardUserSystemClock14GetCurrentTimeEPNS0_9PosixTimeE", "User system clock = iOS clock (UTC seconds)",
          Result, h_user_clock, int64_t *out) { return current_time(out, "StandardUserSystemClock"); }
HLE_FORCE("_ZN2nn4time26StandardNetworkSystemClock14GetCurrentTimeEPNS0_9PosixTimeE", "Network system clock = iOS clock, which iOS keeps network-synchronized",
          Result, h_network_clock, int64_t *out) { return current_time(out, "StandardNetworkSystemClock"); }
static void fill_calendar(const struct tm *t, struct calendar_time *c) {
    *c = (struct calendar_time){(int16_t)(t->tm_year + 1900), (int8_t)(t->tm_mon + 1), (int8_t)t->tm_mday, (int8_t)t->tm_hour, (int8_t)t->tm_min, (int8_t)t->tm_sec, 0};
}
HLE_FORCE("_ZN2nn4time14ToCalendarTimeEPNS0_12CalendarTimeEPNS0_22CalendarAdditionalInfoERKNS0_9PosixTimeE", "Convert with the iOS device time zone",
          Result, h_to_calendar, struct calendar_time *out, struct calendar_additional *info, const int64_t *posix) {
    if (!out || !posix) die("time ToCalendarTime out=%p posix=%p", (void *)out, (const void *)posix);
    if (!atomic_load(&time_initialized)) return RESULT_NOT_INITIALIZED;
    time_t t = (time_t)*posix; struct tm local;
    if (!localtime_r(&t, &local)) return 0x1f074u;   // time: out of range
    fill_calendar(&local, out);
    if (info) {
        memset(info, 0, sizeof *info);
        info->day_of_week = local.tm_wday; info->day_of_year = local.tm_yday;
        strncpy(info->zone_name, local.tm_zone ? local.tm_zone : "UTC", sizeof info->zone_name);
        info->daylight = local.tm_isdst > 0; info->utc_offset = (int32_t)local.tm_gmtoff;
    }
    return 0;
}
// Returned by value in x0 (8 bytes).
HLE_FORCE("_ZN2nn4time19ToCalendarTimeInUtcERKNS0_9PosixTimeE", "UTC calendar conversion", uint64_t, h_to_calendar_utc, const int64_t *posix) {
    if (!posix) die("time ToCalendarTimeInUtc posix=NULL");
    time_t t = (time_t)*posix; struct tm utc; struct calendar_time c = {0};
    if (gmtime_r(&t, &utc)) fill_calendar(&utc, &c);
    uint64_t v; memcpy(&v, &c, 8); return v;
}
HLE_FORCE("_ZN2nn4time21GetDeviceLocationNameEPNS0_12LocationNameE", "Device location = iOS system time zone (IANA name)",
          void, h_location_name, char *out) {
    if (!out) die("time GetDeviceLocationName out=NULL");
    memset(out, 0, 0x24);
    CFTimeZoneRef tz = CFTimeZoneCopySystem();
    if (!tz || !CFStringGetCString(CFTimeZoneGetName(tz), out, 0x24, kCFStringEncodingUTF8)) strcpy(out, "UTC");
    if (tz) CFRelease(tz);
}

// AsyncEnsureNetworkClockAvailabilityTask: the network clock is always available here (iOS time), so the task
// completes as soon as it starts with success and its finish event is signaled. The task object's contents are
// host-defined; the finish event is a real nn::os SystemEvent kept per task object.
struct clock_task { const void *object; uint8_t event[0x40]; int initialized, started; struct clock_task *next; };
static struct clock_task *clock_tasks;
static pthread_mutex_t clock_tasks_lock = PTHREAD_MUTEX_INITIALIZER;
static struct clock_task *task_of(const void *object, int create, const char *who) {
    pthread_mutex_lock(&clock_tasks_lock);
    struct clock_task *t = clock_tasks;
    while (t && t->object != object) t = t->next;
    if (!t && create) { t = calloc(1, sizeof *t); if (t) { t->object = object; t->next = clock_tasks; clock_tasks = t; } }
    pthread_mutex_unlock(&clock_tasks_lock);
    if (!t) die("time %s task=%p unknown", who, object);
    return t;
}
HLE_FORCE("_ZN2nn4time39AsyncEnsureNetworkClockAvailabilityTaskC1Ev", "Host task record for the network clock check",
          void, h_clock_task_ctor, void *object) { task_of(object, 1, "ctor"); }
HLE_FORCE("_ZN2nn4time39AsyncEnsureNetworkClockAvailabilityTaskD1Ev", "Release the host task record and its event",
          void, h_clock_task_dtor, void *object) {
    pthread_mutex_lock(&clock_tasks_lock);
    struct clock_task **p = &clock_tasks; while (*p && (*p)->object != object) p = &(*p)->next;
    struct clock_task *t = *p; if (t) *p = t->next;
    pthread_mutex_unlock(&clock_tasks_lock);
    if (t && t->initialized) { void (*destroy)(void *) = required("_ZN2nn2os18DestroySystemEventEPNS0_15SystemEventTypeE"); destroy(t->event); }
    free(t);
}
HLE_FORCE("_ZN2nn4time39AsyncEnsureNetworkClockAvailabilityTask10InitializeENS_2os14EventClearModeE", "Finish event is a real SystemEvent",
          Result, h_clock_task_init, void *object, int clear_mode) {
    struct clock_task *t = task_of(object, 0, "Initialize");
    if (t->initialized) die("time task %p initialized twice", object);
    Result (*create)(void *, int, unsigned) = required("_ZN2nn2os17CreateSystemEventEPNS0_15SystemEventTypeENS0_14EventClearModeEb");
    Result r = create(t->event, clear_mode, 0); if (!r) t->initialized = 1;
    return r;
}
HLE_FORCE("_ZN2nn4time39AsyncEnsureNetworkClockAvailabilityTask9StartTaskEv", "Network clock already available: complete immediately",
          Result, h_clock_task_start, void *object) {
    struct clock_task *t = task_of(object, 0, "StartTask");
    if (!t->initialized) die("time task %p started before Initialize", object);
    void (*signal)(void *) = required("_ZN2nn2os17SignalSystemEventEPNS0_15SystemEventTypeE");
    t->started = 1; signal(t->event);
    LOG("HOST_TIME_NETWORK_CLOCK_TASK task=%p complete (iOS clock)", object);
    return 0;
}
HLE_FORCE("_ZN2nn4time39AsyncEnsureNetworkClockAvailabilityTask14GetFinishEventEv", "The task's SystemEvent",
          void *, h_clock_task_event, void *object) {
    struct clock_task *t = task_of(object, 0, "GetFinishEvent");
    if (!t->initialized) die("time task %p GetFinishEvent before Initialize", object);
    return t->event;
}
HLE_FORCE("_ZN2nn4time39AsyncEnsureNetworkClockAvailabilityTask9GetResultEv", "Task result: success once started",
          Result, h_clock_task_result, void *object) {
    struct clock_task *t = task_of(object, 0, "GetResult");
    if (!t->started) die("time task %p GetResult before StartTask", object);
    return 0;
}
