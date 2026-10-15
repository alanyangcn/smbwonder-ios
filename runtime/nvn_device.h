// Strict Wonder NVN device bootstrap (API 55.15). No generic success stubs:
// every loaded name maps to a typed host implementation or to a named stop thunk.
#pragma once
#include <stddef.h>
#include <stdint.h>
#define WONDER_NVN_DEVICE_SIZE 0x3000          // main+0x5bb900: allocator request for NVNdevice
#define WONDER_NVN_DEVICE_BUILDER_SIZE 0x40    // main+0x5bb720 frame: builder at sp+0x10, next slot sp+0x50
#define WONDER_NVN_QUEUE_BUILDER_SIZE 0x40     // reuses the 64-byte device builder slot
#define WONDER_NVN_QUEUE_SIZE 0x2000            // main+0x5bbb58: allocator request for NVNqueue
#define WONDER_NVN_MEMORY_POOL_SIZE 0x100       // main+0x7338c8: allocator request for NVNmemoryPool
#define WONDER_NVN_BUILDER_SLOT 0x40            // main+0x7338e4: pool/sampler builders at sp+8, locals from sp+0x48
#define WONDER_NVN_DESCRIPTOR_POOL_SIZE 0x20    // texture pool x19+0x58, sampler pool x19+0x78
#define WONDER_NVN_SAMPLER_SIZE 0x30            // sampler at x19+0x98, next field x19+0xd8 (header 0x30)
#define WONDER_NVN_API_MINOR_REPORTED 15
// Host glue (nvn_bootstrap.c on device, the test harness on the host).
void wonder_nvn_report(const char *format, ...) __attribute__((format(printf, 1, 2)));
_Noreturn void wonder_nvn_stop(const char *format, ...) __attribute__((format(printf, 1, 2)));
int wonder_host_driver_ready(void);
void *wonder_nvn_unimplemented_thunk(unsigned index);
// Zero-copy host GPU buffer over [memory, memory+size): returns a retained handle and the byte offset
// of memory inside it (iOS requires page-aligned no-copy buffers), or NULL.
void *wonder_host_pool_buffer(void *memory, size_t size, size_t *offset);
// PROT_NONE address range for a virtual pool's GPU addresses (GPU address = CPU address); NULL on failure.
void *wonder_host_reserve(size_t size);
// Host render-target texture for an NVN display texture (private storage); retained handle or NULL.
void *wonder_host_display_texture(unsigned width, unsigned height, int nvn_format);
// The NVNnativeWindow must be the CAMetalLayer that VI handed out.
int wonder_host_native_window_valid(const void *native);
// Name resolution shared by nvnBootstrapLoader and nvnDeviceGetProcAddress.
int wonder_nvn_name_index(const char *name);
void *wonder_nvn_resolve(const char *name, const char *via);
// Implemented entries (typed per NVN headers; NVNboolean returned in w0).
void wonder_nvnDeviceBuilderSetDefaults(void *builder);
void wonder_nvnDeviceBuilderSetFlags(void *builder, int flags);
uint32_t wonder_nvnDeviceInitialize(void *device, const void *builder);
void wonder_nvnDeviceGetInteger(const void *device, int what, int *out);
void *wonder_nvnDeviceGetProcAddress(const void *device, const char *name);
void wonder_nvnDeviceSetWindowOriginMode(void *device, int mode);
void wonder_nvnDeviceSetDepthMode(void *device, int mode);
void wonder_nvnQueueBuilderSetDefaults(void *builder);
void wonder_nvnQueueBuilderSetDevice(void *builder, const void *device);
void wonder_nvnQueueBuilderSetFlags(void *builder, int flags);
void wonder_nvnQueueBuilderSetCommandMemorySize(void *builder, size_t size);
void wonder_nvnQueueBuilderSetComputeMemorySize(void *builder, size_t size);
void wonder_nvnQueueBuilderSetControlMemorySize(void *builder, size_t size);
size_t wonder_nvnQueueBuilderGetQueueMemorySize(const void *builder);
void wonder_nvnQueueBuilderSetQueueMemory(void *builder, void *memory, size_t size);
uint32_t wonder_nvnQueueInitialize(void *queue, const void *builder);
void wonder_nvnMemoryPoolBuilderSetDefaults(void *builder);
void wonder_nvnMemoryPoolBuilderSetDevice(void *builder, const void *device);
void wonder_nvnMemoryPoolBuilderSetFlags(void *builder, int flags);
void wonder_nvnMemoryPoolBuilderSetStorage(void *builder, void *memory, size_t size);
uint32_t wonder_nvnMemoryPoolInitialize(void *pool, const void *builder);
uint32_t wonder_nvnTexturePoolInitialize(void *pool, const void *memory_pool, ptrdiff_t offset, int count);
uint32_t wonder_nvnSamplerPoolInitialize(void *pool, const void *memory_pool, ptrdiff_t offset, int count);
void wonder_nvnSamplerBuilderSetDefaults(void *builder);
void wonder_nvnSamplerBuilderSetDevice(void *builder, const void *device);
void wonder_nvnSamplerBuilderSetMinMagFilter(void *builder, int min, int mag);
uint32_t wonder_nvnSamplerInitialize(void *sampler, const void *builder);
void wonder_nvnSamplerPoolRegisterSampler(const void *pool, int id, const void *sampler);
// Host view of registered samplers (for the future Metal path and tests).
struct wonder_nvn_sampler_state { uint8_t min, mag, wrap_s, wrap_t, wrap_r, compare_mode, compare_func; uint8_t valid:1, reduction:2, reserved:5; float lod_min, lod_max, lod_bias, anisotropy, border[4]; };
const struct wonder_nvn_sampler_state *wonder_nvn_sampler_lookup(const void *pool, int id);
int wonder_nvn_implemented_count(void);
// Shared with the other NVN units (nvn_swapchain.c).
struct wonder_nvn_entry { const char *name; void *fn; };
extern const struct wonder_nvn_entry wonder_nvn_swapchain_entries[];
extern const unsigned wonder_nvn_swapchain_count;
extern const struct wonder_nvn_entry wonder_nvn_buffer_entries[];   // nvn_buffer.c: buffers, command buffers
extern const unsigned wonder_nvn_buffer_count;
extern const struct wonder_nvn_entry wonder_nvn_texture_entries[];   // nvn_texture.c
extern const unsigned wonder_nvn_texture_count;
extern const struct wonder_nvn_entry wonder_nvn_state_entries[];     // nvn_state.c
extern const unsigned wonder_nvn_state_count;
size_t wonder_nvn_texture_registration_size(void);
struct wonder_nvn_pool_view { uint8_t *memory; uint64_t size; uint32_t flags; void *host_buffer; uint64_t host_offset; };
int wonder_nvn_info(int what);
const void *wonder_nvn_device_check(const void *device, const char *who);
void wonder_nvn_pool_check(const void *pool, const char *who, struct wonder_nvn_pool_view *view);

// Internal resource access: GPU addresses in virtual pools are tokens, NEVER CPU pointers.
// Resolve one contiguous span, clipped at a mapping boundary. Call again for the remainder.
// Shared physical pages return the same memory/host-buffer offsets, preserving byte aliasing.
// This is a storage view, not permission to map a CPU_NO_ACCESS pool into guest code.
void wonder_nvn_pool_span(const void *pool, uint64_t offset, uint64_t size, struct wonder_nvn_pool_view *view);

extern const struct wonder_nvn_entry wonder_nvn_command_entries[];
extern const unsigned wonder_nvn_command_count;

void wonder_nvn_address_span(uint64_t address, uint64_t size, struct wonder_nvn_pool_view *view);

void wonder_host_resource_label(void *resource, const char *label);

// Lifetime helpers (host_graphics.m on device, the test harness on the host).
void wonder_host_release(void *resource);
void wonder_nvn_frame_summary(unsigned *draws, unsigned *clears, uint64_t *hash);   // since the previous call (nvn_queue.c)
void wonder_host_pool_forget(const void *pool);      // nvnMemoryPoolFinalize: GPU textures stored in this pool
void wonder_host_texture_forget(uint64_t serial);   // nvnTextureFinalize: release host GPU textures made for this serial
void wonder_host_unreserve(void *memory, size_t size);
const void *wonder_nvn_queue_check(const void *queue, const char *who);
// Program shaders keyed by NVNshaderStage (0 vertex, 1 fragment, 2 geometry, 3 tess control, 4 tess eval, 5 compute).
struct wonder_nvn_program_shaders { uint64_t serial; uint32_t mask; uint64_t code[6], extra[6]; const void *control[6]; };
void wonder_nvn_device_modes(int *depth_mode, int *window_origin);   // NVNdepthMode / NVNwindowOriginMode last set, -1 if never
int wonder_nvn_program_snapshot(const void *program, struct wonder_nvn_program_shaders *out);
// Shader evidence for the offline translator (nvn_bootstrap.c on device).
void wonder_nvn_capture_shaders(const void *program, const struct wonder_nvn_program_shaders *shaders);
int wonder_nvn_descriptor_pool_check(const void *pool, int sampler, const char *who);
// Host GPU replay backend (host_graphics.m on device, the test harness on the host). Integer results: 0 = ok.
struct wonder_nvn_texture;
void *wonder_host_render_target(const struct wonder_nvn_texture *texture, uint32_t mtl_pixel_format);
int wonder_host_clear_color(void *texture, int level, int slice, const float rgba[4]);
int wonder_host_clear_depth_stencil(void *texture, int level, int slice, int clear_depth, float depth, int clear_stencil, uint32_t stencil);
int wonder_host_fill_buffer(void *buffer, size_t offset, size_t size, uint8_t value);
int wonder_host_write_buffer(void *buffer, size_t offset, const void *data, size_t size);
void wonder_host_after_gpu(void (*fn)(void *), void *arg);     // run on the CPU after work encoded so far completes
void wonder_host_fence(void (*fn)(void *), void *arg);         // same, and commit now
void wonder_host_flush(void);
void wonder_host_finish(void);
int wonder_host_present(void *texture, const int crop[4], int interval);
extern const struct wonder_nvn_entry wonder_nvn_queue_entries[];
extern const unsigned wonder_nvn_queue_count;
