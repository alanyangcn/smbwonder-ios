#include "ngc_service.h"
#include <pthread.h>
#include <string.h>
#define NGC_TEXT_MAX 0x1000u  // sanity bound: the client sends words of <= 64 and texts of <= 512 code points (sdk+0x386820, +0x386d78)
static pthread_mutex_t lock = PTHREAD_MUTEX_INITIALIZER;
static struct wonder_ngc_service instance;
static struct wonder_ngc_counts counts;
static void stop_proxy_info(void) { wonder_ngc_fatal("ngc IService::GetProxyInfo is not provided by the host service"); }
static void stop_type_info(void) { wonder_ngc_fatal("ngc IService::GetInterfaceTypeInfo is not provided by the host service"); }
static uint32_t stop_content_version(struct wonder_ngc_service *self, uint32_t *out) {
    (void)self; (void)out;
    wonder_ngc_fatal("ngc IService::GetContentVersion: the host has no NG-word database version to report");
}
static const void *const vtable[8] = {
    (const void *)wonder_ngc_add_reference, (const void *)wonder_ngc_release, (const void *)stop_proxy_info,
    (const void *)stop_type_info, (const void *)stop_content_version, (const void *)wonder_ngc_check,
    (const void *)wonder_ngc_mask, (const void *)wonder_ngc_reload,
};
static void require(struct wonder_ngc_service *self, const char *who) {
    if (self != &instance || self->vtable != vtable) wonder_ngc_fatal("ngc %s on an object the host did not create (%p)", who, (void *)self);
    if (self->references <= 0) wonder_ngc_fatal("ngc %s after the last Release", who);
}
static size_t text_length(const struct wonder_ngc_array *text, const char *who) {
    if (!text || !text->data || !text->size || text->size > NGC_TEXT_MAX) wonder_ngc_fatal("ngc %s: invalid text array", who);
    const char *end = memchr(text->data, 0, text->size);
    if (!end) wonder_ngc_fatal("ngc %s: text is not NUL-terminated within %llu bytes", who, (unsigned long long)text->size);
    return (size_t)(end - text->data);
}
static void check_option(uint32_t patterns, const struct wonder_ngc_option *option, const char *who) {
    if (!option) wonder_ngc_fatal("ngc %s: option=NULL", who);
    (void)patterns;   // ProfanityFilterPatternList bits; with no database every list is empty
    counts.last_language_mask = option->language_mask;
}
struct wonder_ngc_service *wonder_ngc_create(void) {
    pthread_mutex_lock(&lock);
    if (instance.references > 0) { pthread_mutex_unlock(&lock); wonder_ngc_fatal("ngc CreateServiceByHipc while the previous service is still referenced"); }
    instance.vtable = vtable; instance.references = 1; counts.created++;
    pthread_mutex_unlock(&lock);
    return &instance;
}
int wonder_ngc_live(void) { pthread_mutex_lock(&lock); int r = instance.references; pthread_mutex_unlock(&lock); return r; }
struct wonder_ngc_counts wonder_ngc_counts(void) { pthread_mutex_lock(&lock); struct wonder_ngc_counts c = counts; pthread_mutex_unlock(&lock); return c; }
void wonder_ngc_add_reference(struct wonder_ngc_service *self) {
    pthread_mutex_lock(&lock); require(self, "AddReference"); self->references++; pthread_mutex_unlock(&lock);
}
void wonder_ngc_release(struct wonder_ngc_service *self) {
    pthread_mutex_lock(&lock); require(self, "Release"); self->references--; pthread_mutex_unlock(&lock);
}
uint32_t wonder_ngc_check(struct wonder_ngc_service *self, uint32_t *out_matched, const struct wonder_ngc_array *text,
                          uint32_t patterns, const struct wonder_ngc_option *option) {
    pthread_mutex_lock(&lock);
    require(self, "Check");
    if (!out_matched) wonder_ngc_fatal("ngc Check: out=NULL");
    text_length(text, "Check"); check_option(patterns, option, "Check");
    *out_matched = 0;   // no database: no pattern matches
    uint64_t n = ++counts.checks; uint32_t language = counts.last_language_mask;
    pthread_mutex_unlock(&lock);
    if (n == 1 || n % 1000 == 0) wonder_ngc_observe("Check", n, language);
    return 0;
}
uint32_t wonder_ngc_mask(struct wonder_ngc_service *self, int32_t *out_count, const struct wonder_ngc_array *out_text,
                         const struct wonder_ngc_array *text, uint32_t patterns, const struct wonder_ngc_option *option) {
    pthread_mutex_lock(&lock);
    require(self, "Mask");
    if (!out_count || !out_text || !out_text->data) wonder_ngc_fatal("ngc Mask: invalid output");
    size_t n = text_length(text, "Mask"); check_option(patterns, option, "Mask");
    if (out_text->size < n + 1) wonder_ngc_fatal("ngc Mask: output %llu bytes for %zu-byte text", (unsigned long long)out_text->size, n + 1);
    memmove((char *)out_text->data, text->data, n + 1);   // unchanged: nothing masked
    *out_count = 0; uint64_t m = ++counts.masks; uint32_t language = counts.last_language_mask;
    pthread_mutex_unlock(&lock);
    if (m == 1 || m % 1000 == 0) wonder_ngc_observe("Mask", m, language);
    return 0;
}
uint32_t wonder_ngc_reload(struct wonder_ngc_service *self) {
    pthread_mutex_lock(&lock); require(self, "Reload"); uint64_t n = ++counts.reloads; pthread_mutex_unlock(&lock);
    wonder_ngc_observe("Reload", n, 0);
    return 0;
}
