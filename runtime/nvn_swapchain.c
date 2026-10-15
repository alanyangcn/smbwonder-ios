// NVN display path for Wonder (main+0x9849bc): sync objects and the window over the VI CAMetalLayer.
// Textures live in nvn_texture.c.
#include "nvn_device.h"
#include "nvn_texture.h"
#include <string.h>
#define DEVICE_SET_MARK 0x56454454u      /* SetDevice may precede SetDefaults (main+0x984a4c) */
#define SYNC_MAGIC 0x434e5953u           /* "SYNC" */
#define WBUILDER_MAGIC 0x424e4957u
#define WINDOW_MAGIC 0x574f4457u
#define SYNC_SIZE 0x40                   // main+0x984c78
#define WINDOW_BUILDER_SIZE 0x40         // sp+0x18, texture builders from sp+0x58
#define WINDOW_SIZE 0x180                // main+0x984c9c
#define MAX_WINDOW_TEXTURES 4

struct sync { uint32_t magic, signaled; const void *device; uint64_t fence; };
struct window_builder { uint32_t magic, device_mark; const void *device; const void *native; const void *const *textures; int32_t count, active, interval; };
struct window { uint32_t magic; int32_t count, active, interval, next; const void *device, *native; const void *textures[MAX_WINDOW_TEXTURES]; uint64_t acquired; };
_Static_assert(sizeof(struct sync) <= SYNC_SIZE, "sync storage");
_Static_assert(sizeof(struct window_builder) <= WINDOW_BUILDER_SIZE, "window builder storage");
_Static_assert(sizeof(struct window) <= WINDOW_SIZE, "window storage");

// ---------- Sync ----------
uint32_t wonder_nvnSyncInitialize(void *sync, const void *device) {
    if (!sync) wonder_nvn_stop("NVN_INVALID_OBJECT nvnSyncInitialize sync=NULL");
    *(struct sync *)sync = (struct sync){SYNC_MAGIC, 0, wonder_nvn_device_check(device, "nvnSyncInitialize"), 0};
    return 1;
}

// ---------- Window ----------
static struct window_builder *wbuilder_of(void *p, const char *who) {
    struct window_builder *b = p;
    if (!b || b->magic != WBUILDER_MAGIC) wonder_nvn_stop("NVN_INVALID_OBJECT %s window_builder=%p (SetDefaults not called)", who, p);
    return b;
}
void wonder_nvnWindowBuilderSetDefaults(void *builder) {
    if (!builder) wonder_nvn_stop("NVN_INVALID_OBJECT nvnWindowBuilderSetDefaults builder=NULL");
    struct window_builder *b = builder;
    const void *device = b->device_mark == DEVICE_SET_MARK ? b->device : 0;
    memset(b, 0, sizeof *b);
    b->magic = WBUILDER_MAGIC; b->device = device; b->device_mark = device ? DEVICE_SET_MARK : 0; b->interval = 1;
}
void wonder_nvnWindowBuilderSetDevice(void *builder, const void *device) {
    struct window_builder *b = wbuilder_of(builder, "nvnWindowBuilderSetDevice");
    b->device = wonder_nvn_device_check(device, "nvnWindowBuilderSetDevice"); b->device_mark = DEVICE_SET_MARK;
}
void wonder_nvnWindowBuilderSetNativeWindow(void *builder, const void *native) {
    struct window_builder *b = wbuilder_of(builder, "nvnWindowBuilderSetNativeWindow");
    if (!wonder_host_native_window_valid(native)) wonder_nvn_stop("NVN_INVALID_ARGUMENT nvnWindowBuilderSetNativeWindow native=%p (not the VI CAMetalLayer)", native);
    b->native = native;
}
void wonder_nvnWindowBuilderSetTextures(void *builder, int count, const void *const *textures) {
    struct window_builder *b = wbuilder_of(builder, "nvnWindowBuilderSetTextures");
    if (count < 2 || count > MAX_WINDOW_TEXTURES || !textures) wonder_nvn_stop("NVN_INVALID_ARGUMENT nvnWindowBuilderSetTextures count=%d", count);
    for (int i = 0; i < count; i++) {
        const struct wonder_nvn_texture *t = wonder_nvn_texture_of(textures[i], "nvnWindowBuilderSetTextures");
        const struct wonder_nvn_texture *first = textures[0];
        if (!(t->flags & 1) || t->desc.width != first->desc.width || t->desc.height != first->desc.height || t->desc.format != first->desc.format)
            wonder_nvn_stop("NVN_INVALID_ARGUMENT nvnWindowBuilderSetTextures texture %d is not a matching display texture", i);
    }
    b->textures = textures; b->count = count; b->active = count;
}
void wonder_nvnWindowBuilderSetNumActiveTextures(void *builder, int count) {
    struct window_builder *b = wbuilder_of(builder, "nvnWindowBuilderSetNumActiveTextures");
    if (count < 2 || count > b->count) wonder_nvn_stop("NVN_INVALID_ARGUMENT nvnWindowBuilderSetNumActiveTextures %d of %d", count, b->count);
    b->active = count;
}
void wonder_nvnWindowBuilderSetPresentInterval(void *builder, int interval) {
    struct window_builder *b = wbuilder_of(builder, "nvnWindowBuilderSetPresentInterval");
    if (interval < 0 || interval > 5) wonder_nvn_stop("NVN_INVALID_ARGUMENT nvnWindowBuilderSetPresentInterval %d", interval);
    b->interval = interval;
}
uint32_t wonder_nvnWindowInitialize(void *window, const void *builder) {
    const struct window_builder *b = wbuilder_of((void *)builder, "nvnWindowInitialize");
    if (!window) wonder_nvn_stop("NVN_INVALID_OBJECT nvnWindowInitialize window=NULL");
    if (!b->device || !b->native || !b->textures) wonder_nvn_stop("NVN_INVALID_ARGUMENT nvnWindowInitialize device=%p native=%p textures=%p", b->device, b->native, b->textures);
    struct window *w = window;
    memset(w, 0, sizeof *w);
    w->magic = WINDOW_MAGIC; w->count = b->count; w->active = b->active; w->interval = b->interval;
    w->device = b->device; w->native = b->native;
    for (int i = 0; i < b->count; i++) w->textures[i] = b->textures[i];   // NVN copies the texture list at initialize
    const struct wonder_nvn_texture *t = w->textures[0];
    wonder_nvn_report("NVN_WINDOW_READY window=%p textures=%d active=%d interval=%d %ux%u format=%#x native=%p",
                      window, w->count, w->active, w->interval, t->desc.width, t->desc.height, (unsigned)t->desc.format, w->native);
    return 1;
}
// Textures are host-owned until presented, so acquiring never waits on the GPU yet; the sync is
// signaled immediately. Presentation (nvnQueuePresentTexture) will hand them back in order.
int wonder_nvnWindowAcquireTexture(void *window, void *sync, int *index) {
    struct window *w = window;
    if (!w || w->magic != WINDOW_MAGIC) wonder_nvn_stop("NVN_INVALID_OBJECT nvnWindowAcquireTexture window=%p", window);
    struct sync *s = sync;
    if (!s || s->magic != SYNC_MAGIC) wonder_nvn_stop("NVN_INVALID_OBJECT nvnWindowAcquireTexture sync=%p", sync);
    if (!index) wonder_nvn_stop("NVN_INVALID_ARGUMENT nvnWindowAcquireTexture index=NULL");
    if (w->acquired & (1ull << w->next)) wonder_nvn_stop("NVN_UNSUPPORTED nvnWindowAcquireTexture: texture %d still held (present path not implemented)", w->next);
    *index = w->next; w->acquired |= 1ull << w->next; w->next = (w->next + 1) % w->active;
    s->signaled = 1; s->fence++;
    wonder_nvn_report("NVN_CALL nvnWindowAcquireTexture window=%p -> index %d (sync signaled)", window, *index);
    return 0;   // NVN_WINDOW_ACQUIRE_TEXTURE_RESULT_SUCCESS
}

void wonder_nvnWindowSetDebugLabel(void *p, const char *label) {
    const struct window *o=p;if(!o || o->magic!=WINDOW_MAGIC) wonder_nvn_stop("NVN_INVALID_OBJECT nvnWindowSetDebugLabel window=%p",p);
    wonder_nvn_report("NVN_WINDOW_LABEL object=%p label=%s",p,label?label:"");
}
void wonder_nvnSyncSetDebugLabel(void *p, const char *label) {
    const struct sync *o=p;if(!o || o->magic!=SYNC_MAGIC) wonder_nvn_stop("NVN_INVALID_OBJECT nvnSyncSetDebugLabel sync=%p",p);
    wonder_nvn_report("NVN_SYNC_LABEL object=%p label=%s",p,label?label:"");
}
const struct wonder_nvn_entry wonder_nvn_swapchain_entries[] = {
    {"nvnWindowSetDebugLabel", (void *)wonder_nvnWindowSetDebugLabel},
    {"nvnSyncSetDebugLabel", (void *)wonder_nvnSyncSetDebugLabel},
    {"nvnSyncInitialize", (void *)wonder_nvnSyncInitialize},
    {"nvnWindowBuilderSetDefaults", (void *)wonder_nvnWindowBuilderSetDefaults},
    {"nvnWindowBuilderSetDevice", (void *)wonder_nvnWindowBuilderSetDevice},
    {"nvnWindowBuilderSetNativeWindow", (void *)wonder_nvnWindowBuilderSetNativeWindow},
    {"nvnWindowBuilderSetTextures", (void *)wonder_nvnWindowBuilderSetTextures},
    {"nvnWindowBuilderSetNumActiveTextures", (void *)wonder_nvnWindowBuilderSetNumActiveTextures},
    {"nvnWindowBuilderSetPresentInterval", (void *)wonder_nvnWindowBuilderSetPresentInterval},
    {"nvnWindowInitialize", (void *)wonder_nvnWindowInitialize},
    {"nvnWindowAcquireTexture", (void *)wonder_nvnWindowAcquireTexture},
};
const unsigned wonder_nvn_swapchain_count = sizeof wonder_nvn_swapchain_entries / sizeof *wonder_nvn_swapchain_entries;
