// NVN display path for Wonder (main+0x9849bc): sync objects and the window over the VI CAMetalLayer.
// Textures live in nvn_texture.c.
#include "nvn_device.h"
#include "nvn_texture.h"
#include <stdlib.h>
#include <string.h>
#include <stdatomic.h>
#include <time.h>
#define DEVICE_SET_MARK 0x56454454u      /* SetDevice may precede SetDefaults (main+0x984a4c) */
#define SYNC_MAGIC 0x434e5953u           /* "SYNC" */
#define WBUILDER_MAGIC 0x424e4957u
#define WINDOW_MAGIC 0x574f4457u
#define SYNC_SIZE 0x40                   // main+0x984c78
#define WINDOW_BUILDER_SIZE 0x40         // sp+0x18, texture builders from sp+0x58
#define WINDOW_SIZE 0x180                // main+0x984c9c
#define MAX_WINDOW_TEXTURES 4

struct sync { uint32_t magic; _Atomic uint32_t signaled; const void *device; uint64_t fence; };
struct window_builder { uint32_t magic, device_mark; const void *device; const void *native; const void *const *textures; int32_t count, active, interval; };
struct window { uint32_t magic; int32_t count, active, interval, next; const void *device, *native; const void *textures[MAX_WINDOW_TEXTURES]; uint64_t acquired; int32_t crop[4]; uint32_t presents;
                uint32_t presented[MAX_WINDOW_TEXTURES]; _Atomic uint32_t released[MAX_WINDOW_TEXTURES]; };
_Static_assert(sizeof(struct sync) <= SYNC_SIZE, "sync storage");
_Static_assert(sizeof(struct window_builder) <= WINDOW_BUILDER_SIZE, "window builder storage");
_Static_assert(sizeof(struct window) <= WINDOW_SIZE, "window storage");

// ---------- Sync ----------
uint32_t wonder_nvnSyncInitialize(void *sync, const void *device) {
    if (!sync) wonder_nvn_stop("NVN_INVALID_OBJECT nvnSyncInitialize sync=NULL");
    struct sync *s = sync; s->magic = SYNC_MAGIC; atomic_store(&s->signaled, 0); s->device = wonder_nvn_device_check(device, "nvnSyncInitialize"); s->fence = 0;
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
// A presented texture returns to the window at present time: Metal orders the presentation blit before any
// later rendering into it on the same host queue, so the acquire sync is signaled immediately.
int wonder_nvnWindowAcquireTexture(void *window, void *sync, int *index) {
    struct window *w = window;
    if (!w || w->magic != WINDOW_MAGIC) wonder_nvn_stop("NVN_INVALID_OBJECT nvnWindowAcquireTexture window=%p", window);
    struct sync *s = sync;
    if (!s || s->magic != SYNC_MAGIC) wonder_nvn_stop("NVN_INVALID_OBJECT nvnWindowAcquireTexture sync=%p", sync);
    if (!index) wonder_nvn_stop("NVN_INVALID_ARGUMENT nvnWindowAcquireTexture index=NULL");
    if (w->acquired & (1ull << w->next)) wonder_nvn_stop("NVN_INVALID_STATE nvnWindowAcquireTexture: texture %d still held (acquired without present)", w->next);
    *index = w->next; w->acquired |= 1ull << w->next; w->next = (w->next + 1) % w->active;
    wonder_host_frame_threads_tick(w->interval);   // the presenting thread, once per frame
    // The texture is available once the GPU work that last presented it has completed. The game reuses the
    // per-frame resources tied to it after this point, so returning early lets the CPU overwrite data the
    // GPU has not read yet (round96/97: random black / blue / half-drawn frames). Blocking here is what a
    // console acquire does when no texture is free.
    if (atomic_load(&w->released[*index]) != w->presented[*index]) {
        wonder_host_flush();
        while (atomic_load(&w->released[*index]) != w->presented[*index]) { struct timespec ts = {0, 100000}; nanosleep(&ts, NULL); }
    }
    atomic_store(&s->signaled, 1); s->fence++;
    wonder_nvn_report("NVN_CALL nvnWindowAcquireTexture window=%p -> index %d (sync signaled)", window, *index);
    return 0;   // NVN_WINDOW_ACQUIRE_TEXTURE_RESULT_SUCCESS
}

static struct window *window_of(void *p, const char *who) {
    struct window *w = p;
    if (!w || w->magic != WINDOW_MAGIC) wonder_nvn_stop("NVN_INVALID_OBJECT %s window=%p", who, p);
    return w;
}
// sdk+0x45e6c0: stores the rectangle when width and height are non-zero (otherwise the full texture).
void wonder_nvnWindowSetCrop(void *window, int x, int y, int w, int h) {
    struct window *o = window_of(window, "nvnWindowSetCrop");
    if (x < 0 || y < 0 || w < 0 || h < 0) wonder_nvn_stop("NVN_INVALID_ARGUMENT nvnWindowSetCrop %d,%d %dx%d", x, y, w, h);
    o->crop[0] = x; o->crop[1] = y; o->crop[2] = w; o->crop[3] = h;
}
void wonder_nvnWindowSetNumActiveTextures(void *window, int count) {
    struct window *o = window_of(window, "nvnWindowSetNumActiveTextures");
    if (count < 2 || count > o->count) wonder_nvn_stop("NVN_INVALID_ARGUMENT nvnWindowSetNumActiveTextures %d of %d", count, o->count);
    o->active = count; if (o->next >= count) o->next = 0;
}
void wonder_nvnWindowSetPresentInterval(void *window, int interval) {
    struct window *o = window_of(window, "nvnWindowSetPresentInterval");
    if (interval < 0 || interval > 5) wonder_nvn_stop("NVN_INVALID_ARGUMENT nvnWindowSetPresentInterval %d", interval);
    o->interval = interval;
}
static void release_texture(void *released) { atomic_fetch_add((_Atomic uint32_t *)released, 1); }
// Present: the acquired texture is drawn to the CAMetalLayer drawable after all previously submitted work.
void wonder_nvnQueuePresentTexture(void *queue, void *window, int index) {
    wonder_nvn_queue_check(queue, "nvnQueuePresentTexture");
    struct window *w = window_of(window, "nvnQueuePresentTexture");
    if (index < 0 || index >= w->count || !(w->acquired & (1ull << index)))
        wonder_nvn_stop("NVN_INVALID_ARGUMENT nvnQueuePresentTexture index=%d (not acquired, mask=%#llx)", index, (unsigned long long)w->acquired);
    const struct wonder_nvn_texture *t = wonder_nvn_texture_of(w->textures[index], "nvnQueuePresentTexture");
    int crop[4] = {w->crop[0], w->crop[1], w->crop[2], w->crop[3]};
    if (!crop[2] || !crop[3]) { crop[0] = crop[1] = 0; crop[2] = (int)t->desc.width; crop[3] = (int)t->desc.height; }
    if ((int64_t)crop[0] + crop[2] > (int64_t)t->desc.width || (int64_t)crop[1] + crop[3] > (int64_t)t->desc.height)
        wonder_nvn_stop("NVN_INVALID_ARGUMENT nvnQueuePresentTexture crop %d,%d %dx%d outside %ux%u", crop[0], crop[1], crop[2], crop[3], t->desc.width, t->desc.height);
    w->presented[index]++; wonder_host_after_gpu(release_texture, (void *)&w->released[index]);   // same command buffer as the present
    if (wonder_host_present(t->host, crop, w->interval)) wonder_nvn_stop("NVN_HOST_PRESENT_FAILED nvnQueuePresentTexture index=%d", index);
    w->acquired &= ~(1ull << index); w->presents++; wonder_nvn_frame_advance();
    { static int every = -1; if (every < 0) { const char *e = getenv("WONDER_PRESENT_EVERY"); every = e ? atoi(e) : 0; }
      if (every > 0) { unsigned draws, clears; uint64_t hash; wonder_nvn_frame_summary(&draws, &clears, &hash);
          wonder_nvn_report("NVN_FRAME present=%u index=%d draws=%u clears=%u hash=%016llx", w->presents, index, draws, clears, (unsigned long long)hash); } }
    if (w->presents <= 8 || w->presents % 600 == 0)
        wonder_nvn_report("NVN_PRESENT window=%p index=%d present=%u crop=%d,%d %dx%d interval=%d", window, index, w->presents, crop[0], crop[1], crop[2], crop[3], w->interval);
}
// Fences: the sync becomes signaled when all work submitted before it completes on the host GPU queue.
static void signal_sync(void *arg) { atomic_store(&((struct sync *)arg)->signaled, 1); }
void wonder_nvnQueueFenceSync(void *queue, void *sync, int condition, int flags) {
    wonder_nvn_queue_check(queue, "nvnQueueFenceSync");
    struct sync *s = sync;
    if (!s || s->magic != SYNC_MAGIC) wonder_nvn_stop("NVN_INVALID_OBJECT nvnQueueFenceSync sync=%p", sync);
    if (condition < 0 || condition > 1 || flags & ~1) wonder_nvn_stop("NVN_INVALID_ARGUMENT nvnQueueFenceSync condition=%d flags=%#x", condition, flags);
    atomic_store(&s->signaled, 0); s->fence++;
    wonder_host_fence(signal_sync, s);   // commits pending host work; the completion handler signals
}
// NVNsyncWaitResult: ALREADY_SIGNALED 0, CONDITION_SATISFIED 1, TIMEOUT_EXPIRED 2.
int wonder_nvnSyncWait(const void *sync, uint64_t timeout_ns) {
    struct sync *s = (struct sync *)sync;
    if (!s || s->magic != SYNC_MAGIC) wonder_nvn_stop("NVN_INVALID_OBJECT nvnSyncWait sync=%p", sync);
    if (atomic_load(&s->signaled)) return 0;
    if (!s->fence) wonder_nvn_stop("NVN_INVALID_STATE nvnSyncWait on a sync that was never fenced (would wait forever)");
    struct timespec start; clock_gettime(CLOCK_MONOTONIC, &start);
    for (;;) {
        if (atomic_load(&s->signaled)) return 1;
        struct timespec now; clock_gettime(CLOCK_MONOTONIC, &now);
        uint64_t elapsed = (uint64_t)(now.tv_sec - start.tv_sec) * 1000000000u + (uint64_t)(now.tv_nsec - start.tv_nsec);
        if (timeout_ns != UINT64_MAX && elapsed >= timeout_ns) return 2;
        struct timespec nap = {0, 50000}; nanosleep(&nap, NULL);
    }
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
    {"nvnWindowSetCrop", (void *)wonder_nvnWindowSetCrop},
    {"nvnWindowSetNumActiveTextures", (void *)wonder_nvnWindowSetNumActiveTextures},
    {"nvnWindowSetPresentInterval", (void *)wonder_nvnWindowSetPresentInterval},
    {"nvnQueuePresentTexture", (void *)wonder_nvnQueuePresentTexture},
    {"nvnQueueFenceSync", (void *)wonder_nvnQueueFenceSync},
    {"nvnSyncWait", (void *)wonder_nvnSyncWait},
};
const unsigned wonder_nvn_swapchain_count = sizeof wonder_nvn_swapchain_entries / sizeof *wonder_nvn_swapchain_entries;
