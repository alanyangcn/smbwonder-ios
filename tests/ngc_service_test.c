// Host ngc IService: vtable ABI, reference counting, offline Check/Mask results and argument stops.
#include "ngc_service.h"
#include <assert.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/wait.h>
#include <unistd.h>
_Noreturn void wonder_ngc_fatal(const char *format, ...) { va_list ap; va_start(ap, format); vfprintf(stderr, format, ap); va_end(ap); fputc('\n', stderr); exit(3); }
static int observed;
void wonder_ngc_observe(const char *call, uint64_t count, uint32_t language_mask) { (void)call; (void)count; (void)language_mask; observed++; }
typedef void (*ref_fn)(struct wonder_ngc_service *);
typedef uint32_t (*check_fn)(struct wonder_ngc_service *, uint32_t *, const struct wonder_ngc_array *, uint32_t, const struct wonder_ngc_option *);
typedef uint32_t (*mask_fn)(struct wonder_ngc_service *, int32_t *, const struct wonder_ngc_array *, const struct wonder_ngc_array *, uint32_t, const struct wonder_ngc_option *);
typedef uint32_t (*reload_fn)(struct wonder_ngc_service *);
// The guest reaches every slot through [object] + offset exactly as sdk+0x3864a8 (+0x8), +0x3866b4 (+0x20),
// +0x3867fc (+0x28) do; this test does the same rather than calling the C names.
static const void *slot(struct wonder_ngc_service *s, unsigned offset) { return ((const void *const *)s->vtable)[offset / 8]; }
static int stops(void (*body)(void)) {
    pid_t pid = fork(); if (!pid) { fclose(stderr); body(); exit(0); }
    int status; waitpid(pid, &status, 0); return WIFEXITED(status) && WEXITSTATUS(status) == 3;
}
static struct wonder_ngc_service *live;
static struct wonder_ngc_option option = {{0, 0, 0}, 0x2000};
static void unterminated(void) { char t[4] = {'a', 'b', 'c', 'd'}; struct wonder_ngc_array a = {t, 4}; uint32_t m; ((check_fn)slot(live, 0x28))(live, &m, &a, 1, &option); }
static void short_output(void) { char o[2]; struct wonder_ngc_array in = {"abc", 4}, out = {o, 2}; int32_t c; ((mask_fn)slot(live, 0x30))(live, &c, &out, &in, 1, &option); }
static void version(void) { uint32_t v; ((uint32_t (*)(struct wonder_ngc_service *, uint32_t *))slot(live, 0x20))(live, &v); }
static void second_create(void) { wonder_ngc_create(); }
static void after_release(void) { ((ref_fn)slot(live, 0x8))(live); ((reload_fn)slot(live, 0x38))(live); }
static void foreign(void) { struct wonder_ngc_service fake = *live; ((reload_fn)slot(&fake, 0x38))(&fake); }
int main(void) {
    assert(sizeof(struct wonder_ngc_array) == 16);
    live = wonder_ngc_create();
    assert(live && live->references == 1 && wonder_ngc_live() == 1);
    ((ref_fn)slot(live, 0x0))(live); assert(wonder_ngc_live() == 2);
    ((ref_fn)slot(live, 0x8))(live); assert(wonder_ngc_live() == 1);
    // Check: one word per call (sdk+0x3867e4: InArray {word, strlen + 1}); nothing matches.
    uint32_t matched = 0xffffffff; struct wonder_ngc_array word = {"mario", 6};
    assert(((check_fn)slot(live, 0x28))(live, &matched, &word, 0x3fff, &option) == 0 && matched == 0);
    // Mask: text copied unchanged, zero masked words.
    char out[32]; memset(out, 'x', sizeof out); int32_t count = -1;
    struct wonder_ngc_array text = {"Wonder 1-1", 11}, dest = {out, sizeof out};
    assert(((mask_fn)slot(live, 0x30))(live, &count, &dest, &text, 1, &option) == 0 && count == 0 && !strcmp(out, "Wonder 1-1"));
    assert(((reload_fn)slot(live, 0x38))(live) == 0);
    struct wonder_ngc_counts c = wonder_ngc_counts();
    assert(c.created == 1 && c.checks == 1 && c.masks == 1 && c.reloads == 1 && c.last_language_mask == 0x2000 && observed == 3);
    assert(stops(unterminated) && stops(short_output) && stops(version) && stops(second_create) && stops(after_release) && stops(foreign));
    // Last Release ends the instance; ProfanityFilter re-Initialize creates the next one (sdk+0x386598, +0x386488).
    ((ref_fn)slot(live, 0x8))(live); assert(wonder_ngc_live() == 0);
    assert(wonder_ngc_create() == live && wonder_ngc_live() == 1 && wonder_ngc_counts().created == 2);
    puts("ngc_service OK");
    return 0;
}
