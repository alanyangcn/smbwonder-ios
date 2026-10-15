// Host set: ISettingsServer: vtable ABI, reference counting, language tables and argument stops.
#include "settings_service.h"
#include <assert.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/wait.h>
#include <unistd.h>
_Noreturn void wonder_settings_fatal(const char *format, ...) { va_list ap; va_start(ap, format); vfprintf(stderr, format, ap); va_end(ap); fputc('\n', stderr); exit(3); }
static uint64_t code(const char *s) { uint64_t v = 0; memcpy(&v, s, strlen(s)); return v; }
uint64_t wonder_settings_system_language(void) { return code("zh-Hans"); }
typedef void (*ref_fn)(struct wonder_settings_service *);
typedef uint32_t (*code_fn)(struct wonder_settings_service *, uint64_t *);
typedef uint32_t (*codes_fn)(struct wonder_settings_service *, int32_t *, const struct wonder_settings_array *);
typedef uint32_t (*make_fn)(struct wonder_settings_service *, uint64_t *, int32_t);
typedef uint32_t (*count_fn)(struct wonder_settings_service *, int32_t *);
// The guest reaches every slot through [object] + offset: MakeLanguageCode's table load uses +0x48 (sdk+0x3b7e74),
// its fallback +0x30 (sdk+0x3b7f70), Release +0x8. The test does the same rather than calling the C names.
static const void *slot(struct wonder_settings_service *s, unsigned offset) { return ((const void *const *)s->vtable)[offset / 8]; }
static int stops(void (*body)(void)) {
    pid_t pid = fork(); if (!pid) { fclose(stderr); body(); exit(0); }
    int status; waitpid(pid, &status, 0); return WIFEXITED(status) && WEXITSTATUS(status) == 3;
}
static struct wonder_settings_service *live;
static void make_out_of_range(void) { uint64_t c; ((make_fn)slot(live, 0x30))(live, &c, WONDER_SETTINGS_LANGUAGES); }
static void make_negative(void) { uint64_t c; ((make_fn)slot(live, 0x30))(live, &c, -1); }
static void region(void) { int32_t r; ((count_fn)slot(live, 0x40))(live, &r); }
static void proxy_info(void) { ((void (*)(void))slot(live, 0x10))(); }
static void key_code_map(void) { ((count_fn)slot(live, 0x58))(live, NULL); }
static void base_object(void) { ((void (*)(void))slot(live, 0x80))(); }
static void null_array(void) { int32_t n; struct wonder_settings_array a = {NULL, 4}; ((codes_fn)slot(live, 0x48))(live, &n, &a); }
static void foreign(void) { struct wonder_settings_service fake = *live; int32_t n; ((count_fn)slot(&fake, 0x50))(&fake, &n); }
static void over_release(void) { while (wonder_settings_live() > 0) ((ref_fn)slot(live, 0x8))(live); ((ref_fn)slot(live, 0x8))(live); }
int main(void) {
    assert(sizeof(struct wonder_settings_array) == 16);
    live = wonder_settings_create();
    assert(live && wonder_settings_live() == 1);
    ((ref_fn)slot(live, 0x0))(live); assert(wonder_settings_live() == 2);
    ((ref_fn)slot(live, 0x8))(live); assert(wonder_settings_live() == 1);
    assert(wonder_settings_create() == live && wonder_settings_live() == 2);   // concurrent detail:: calls share it
    ((ref_fn)slot(live, 0x8))(live);
    // Command 0: system language.
    uint64_t c = 0; assert(((code_fn)slot(live, 0x20))(live, &c) == 0 && c == code("zh-Hans"));
    // Command 5 (+0x48): the full table in Language order, as MakeLanguageCode caches it with capacity 64.
    uint64_t table[64]; memset(table, 0xff, sizeof table); int32_t n = -1;
    struct wonder_settings_array a = {table, 64};
    assert(((codes_fn)slot(live, 0x48))(live, &n, &a) == 0 && n == WONDER_SETTINGS_LANGUAGES);
    const char *expect[] = {"ja", "en-US", "fr", "de", "it", "es", "zh-CN", "ko", "nl", "pt", "ru", "zh-TW",
                            "en-GB", "fr-CA", "es-419", "zh-Hans", "zh-Hant", "pt-BR"};
    for (int i = 0; i < WONDER_SETTINGS_LANGUAGES; i++) assert(table[i] == code(expect[i]));
    assert(table[WONDER_SETTINGS_LANGUAGES] == UINT64_MAX);   // nothing written past the count
    // Smaller capacity truncates; count reports what was written.
    struct wonder_settings_array small = {table, 3}; assert(((codes_fn)slot(live, 0x48))(live, &n, &small) == 0 && n == 3);
    struct wonder_settings_array none = {table, 0}; assert(((codes_fn)slot(live, 0x48))(live, &n, &none) == 0 && n == 0);
    assert(((count_fn)slot(live, 0x50))(live, &n) == 0 && n == WONDER_SETTINGS_LANGUAGES);
    // Commands 1 and 3: the pre-4.0.0 view stops at 15 (es-419).
    assert(((codes_fn)slot(live, 0x28))(live, &n, &a) == 0 && n == WONDER_SETTINGS_LANGUAGES_PRE_400);
    assert(((count_fn)slot(live, 0x38))(live, &n) == 0 && n == WONDER_SETTINGS_LANGUAGES_PRE_400);
    // Command 2 (+0x30): MakeLanguageCode.
    assert(((make_fn)slot(live, 0x30))(live, &c, 15) == 0 && c == code("zh-Hans"));
    assert(((make_fn)slot(live, 0x30))(live, &c, 0) == 0 && c == code("ja"));
    assert(wonder_settings_language_code(17) == code("pt-BR") && wonder_settings_language_code(18) == 0);
    assert(stops(make_out_of_range) && stops(make_negative));
    assert(stops(region) && stops(proxy_info) && stops(key_code_map) && stops(base_object));
    assert(stops(null_array) && stops(foreign) && stops(over_release));
    puts("settings_service OK");
    return 0;
}
