#import "host_graphics.h"
#include "fps_meter.h"
#include "widescreen.h"
#import <Metal/Metal.h>
#import <QuartzCore/CAMetalLayer.h>
#include "kernel_events.h"
#include "metal3_draw.h"
#include "host_draw.h"
#include "texture_storage.h"
#include <errno.h>
#include <mach/mach.h>
static id<MTLDevice> device;
static id<MTLCommandQueue> queue;
static UIView *surface;
static NSMutableArray<NSNumber *> *vsync_handles;
static CADisplayLink *display_link;
@interface WonderMetalSurface : UIView
@end
@implementation WonderMetalSurface
+ (Class)layerClass{return CAMetalLayer.class;}
- (void)layoutSubviews{[super layoutSubviews];CGFloat scale=self.window.screen.scale?:UIScreen.mainScreen.scale;self.contentScaleFactor=scale;((CAMetalLayer*)self.layer).drawableSize=CGSizeMake(self.bounds.size.width*scale,self.bounds.size.height*scale);}
@end
@interface WonderVsyncTicker : NSObject
- (void)tick:(CADisplayLink *)link;
@end
@implementation WonderVsyncTicker
- (void)tick:(CADisplayLink *)link{(void)link;for(NSNumber *h in [vsync_handles copy])if(wonder_event_signal(h.unsignedIntValue)) [vsync_handles removeObject:h];}
@end
UIView *wonder_graphics_surface(void){NSCAssert(NSThread.isMainThread,@"Graphics view belongs to UIKit thread");if(!surface){surface=[WonderMetalSurface new];surface.backgroundColor=UIColor.blackColor;}return surface;}
int wonder_host_driver_start(void *memory,unsigned long size){
 if(!memory||!size||((uintptr_t)memory&0xfff)||(size&0xfff))return EINVAL;
 if(device&&queue)return 0;
 id<MTLDevice> created=MTLCreateSystemDefaultDevice();id<MTLCommandQueue> createdQueue=[created newCommandQueue];if(!created||!createdQueue)return ENODEV;
 __block int error=0;dispatch_sync(dispatch_get_main_queue(),^{
  if(!surface){error=ENXIO;return;}CAMetalLayer *layer=(CAMetalLayer*)surface.layer;layer.device=created;layer.pixelFormat=MTLPixelFormatBGRA8Unorm;layer.framebufferOnly=YES;
  device=created;queue=createdQueue;
 });
 if(!error){   // translated shaders (tools/wonder_shader_build.py) bundled at dm/shaders by tools/build_loader_probe.py
  wonder_host_draw_set_device((__bridge void*)created);
  wonder_host_shader_set_directory([NSBundle.mainBundle.resourcePath stringByAppendingPathComponent:@"dm/shaders"].fileSystemRepresentation);
 }
 fprintf(stderr,"HOST_GRAPHICS_DRIVER_READY result=%d device=%s queue=%p\n",error,created.name.UTF8String,(__bridge void*)createdQueue);return error;
}
int wonder_host_driver_ready(void){__block int ready=0;dispatch_sync(dispatch_get_main_queue(),^{ready=device&&queue&&surface&&((CAMetalLayer*)surface.layer).device==device;});return ready;}
// Zero-copy MTLBuffer over guest memory. iOS requires the no-copy range to be VM-page aligned, so the
// enclosing pages are wrapped (guest heap pages are host-mapped as a whole) and the offset returned.
void *wonder_host_pool_buffer(void *memory,size_t size,size_t *offset){
 if(!device||!memory||!size||!offset)return NULL;
 uintptr_t page=vm_page_size,base=(uintptr_t)memory&~(page-1),end=((uintptr_t)memory+size+page-1)&~(page-1);
 id<MTLBuffer> buffer=[device newBufferWithBytesNoCopy:(void*)base length:end-base options:MTLResourceStorageModeShared deallocator:nil];
 if(!buffer)return NULL;
 *offset=(uintptr_t)memory-base;return (__bridge_retained void*)buffer;
}
void *wonder_host_display_texture(unsigned width,unsigned height,int format){
 MTLPixelFormat pf=format==0x25?MTLPixelFormatRGBA8Unorm:format==0x38?MTLPixelFormatRGBA8Unorm_sRGB:format==0x9A?MTLPixelFormatBGRA8Unorm:format==0x9C?MTLPixelFormatBGRA8Unorm_sRGB:MTLPixelFormatInvalid;
 if(!device||!width||!height||pf==MTLPixelFormatInvalid)return NULL;
 MTLTextureDescriptor *d=[MTLTextureDescriptor texture2DDescriptorWithPixelFormat:pf width:width height:height mipmapped:NO];
 d.usage=MTLTextureUsageRenderTarget|MTLTextureUsageShaderRead|MTLTextureUsagePixelFormatView;d.storageMode=MTLStorageModePrivate;
 id<MTLTexture> t=[device newTextureWithDescriptor:d];return t?(__bridge_retained void*)t:NULL;
}
int wonder_host_native_window_valid(const void *native){__block int valid=0;dispatch_sync(dispatch_get_main_queue(),^{valid=native&&surface&&native==(__bridge void*)surface.layer;});return valid;}
void *wonder_host_native_layer(void){__block void *native=NULL;dispatch_sync(dispatch_get_main_queue(),^{if(device&&queue&&surface)native=(__bridge void*)surface.layer;});return native;}
int wonder_host_register_vsync(uint32_t handle){
 __block int error=0;dispatch_sync(dispatch_get_main_queue(),^{
  if(!device||!surface){error=ENXIO;return;}
  if(!vsync_handles)vsync_handles=[NSMutableArray new];
  if(vsync_handles.count>=16){error=ENOSPC;return;}
  [vsync_handles addObject:@(handle)];
  if(!display_link){display_link=[CADisplayLink displayLinkWithTarget:[WonderVsyncTicker new] selector:@selector(tick:)];display_link.preferredFramesPerSecond=60;[display_link addToRunLoop:NSRunLoop.mainRunLoop forMode:NSRunLoopCommonModes];}
 });return error;
}

void wonder_host_resource_label(void *resource, const char *label) {
 if (!resource) return;
 id<MTLResource> object=(__bridge id<MTLResource>)resource;
 object.label=label?[NSString stringWithUTF8String:label]:nil;
}

void wonder_host_release(void *resource){ if(resource) CFRelease(resource); }

// ---------- Replay backend (nvn_queue.c / nvn_swapchain.c) ----------
// One persistent MTLCommandBuffer collects encoders until a flush, fence or present commits it; completed
// handlers run CPU-side report writes and sync signals in commit order.
#include "nvn_texture.h"
#include <pthread.h>
#include <stdatomic.h>
#include <math.h>
static pthread_mutex_t replay_mutex = PTHREAD_MUTEX_INITIALIZER;
static id<MTLCommandBuffer> pending, last_committed;
static id<MTLRenderPipelineState> present_pipeline;
static id<MTLSamplerState> present_sampler;
// A failed replay command buffer is recorded here and stops the next submission by name (nvn_queue.c).
static char gpu_error_text[256]; static _Atomic int gpu_failed;
void wonder_host_gpu_error_reset(void){ atomic_store(&gpu_failed,0); gpu_error_text[0]=0; }
int wonder_host_gpu_error(char *out,size_t size){ if(!atomic_load(&gpu_failed)) return 0; snprintf(out,size,"%s",gpu_error_text); return 1; }
// The pending command buffer as is (the draw batch flush encodes into it) and, for every other user, after the open
// draw batch was encoded (metal3_draw.m pass batching keeps command order this way).
void wonder_host_draw_flush_locked(void);
// Frame-time accounting (round222, printed with HOST_MEMORY): where the replay thread's wall time goes, and the GPU's
// command buffer time (GPUEndTime - GPUStartTime, overlapping buffers counted once each).
static _Atomic uint64_t acct_finish_ns, acct_finish_n, acct_cbuf_ns, acct_drawable_ns, acct_idle_ns, acct_gpu_ns, acct_cbufs;
static uint64_t now_ns(void){ return clock_gettime_nsec_np(CLOCK_UPTIME_RAW); }
static id<MTLCommandBuffer> raw_pending_buffer(void){
 if(!pending){
  uint64_t t0=now_ns(); pending=[queue commandBuffer]; atomic_fetch_add(&acct_cbuf_ns,now_ns()-t0); atomic_fetch_add(&acct_cbufs,1);
  pending.label=@"Wonder NVN submit";
  [pending addCompletedHandler:^(id<MTLCommandBuffer> b){
   if(b.GPUEndTime>b.GPUStartTime) atomic_fetch_add(&acct_gpu_ns,(uint64_t)((b.GPUEndTime-b.GPUStartTime)*1e9));
   if(b.status==MTLCommandBufferStatusError&&!atomic_exchange(&gpu_failed,1)){
    snprintf(gpu_error_text,sizeof gpu_error_text,"%s in %s",b.error.localizedDescription.UTF8String?:"?",b.label.UTF8String?:"?");
    fprintf(stderr,"HOST_GPU_ERROR %s\n",gpu_error_text);
   }
  }];
 }
 return pending;
}
void *wonder_host_replay_command_buffer_raw(void){ return (__bridge void*)raw_pending_buffer(); }
static id<MTLCommandBuffer> pending_buffer(void){ wonder_host_draw_flush_locked(); return raw_pending_buffer(); }
static unsigned pending_first_submit, pending_last_submit;
static void commit_locked(void){ wonder_host_draw_flush_locked(); if(pending){ [pending commit]; last_committed=pending; pending=nil; pending_first_submit=pending_last_submit=0; } }
// Labels and completion handlers do not encode anything: they take the command buffer without encoding the queued
// operations (metal3_draw.m), which keeps the load/store lookahead across submits.
void wonder_host_note_submit(unsigned submit){
 pthread_mutex_lock(&replay_mutex);
 id<MTLCommandBuffer> b=raw_pending_buffer(); if(!pending_first_submit) pending_first_submit=submit; pending_last_submit=submit;
 b.label=[NSString stringWithFormat:@"Wonder NVN submits %u-%u",pending_first_submit,pending_last_submit];
 pthread_mutex_unlock(&replay_mutex);
}
void wonder_host_flush(void){ pthread_mutex_lock(&replay_mutex); commit_locked(); pthread_mutex_unlock(&replay_mutex); }
// Waits for the GPU and for the completion handlers registered before this point (report writes, sync signals):
// handlers of one command buffer run in registration order, so a final handler signals when they are all done.
void wonder_host_finish(void){
 dispatch_semaphore_t done=dispatch_semaphore_create(0);
 pthread_mutex_lock(&replay_mutex);
 [pending_buffer() addCompletedHandler:^(id<MTLCommandBuffer> b){ (void)b; dispatch_semaphore_signal(done); }];
 commit_locked();
 pthread_mutex_unlock(&replay_mutex);
 uint64_t t0=now_ns(); dispatch_semaphore_wait(done,DISPATCH_TIME_FOREVER); atomic_fetch_add(&acct_finish_ns,now_ns()-t0); atomic_fetch_add(&acct_finish_n,1);
}
void wonder_host_after_gpu(void (*fn)(void *), void *arg){
 pthread_mutex_lock(&replay_mutex); [raw_pending_buffer() addCompletedHandler:^(id<MTLCommandBuffer> b){ (void)b; fn(arg); }]; pthread_mutex_unlock(&replay_mutex);
}
void wonder_host_fence(void (*fn)(void *), void *arg){
 pthread_mutex_lock(&replay_mutex); [pending_buffer() addCompletedHandler:^(id<MTLCommandBuffer> b){ (void)b; fn(arg); }]; commit_locked(); pthread_mutex_unlock(&replay_mutex);
}
void wonder_host_replay_lock(void){ pthread_mutex_lock(&replay_mutex); }
void wonder_host_replay_unlock(void){ pthread_mutex_unlock(&replay_mutex); }
void *wonder_host_replay_command_buffer(void){ return (__bridge void*)pending_buffer(); }
// GPU-written textures live by storage, not by texture object (texture_storage.c): Wonder re-creates its transient
// render targets every frame over the same pool memory and reads what the previous objects rendered (round102:
// identical draw sequences gave the title art or black/blue frames depending on which object was fresh).
static struct wonder_storage_key storage_key(const struct wonder_nvn_texture *t){
 struct wonder_storage_key k; memset(&k,0,sizeof k); k.pool=t->pool; k.offset=t->offset; k.size=t->size; k.desc=t->desc; return k;
}
static void release_texture(void *object){ CFRelease(object); }
void *wonder_host_render_target_existing(const struct wonder_nvn_texture *t){
 if(t->host) return t->host;
 struct wonder_storage_key k=storage_key(t);
 pthread_mutex_lock(&replay_mutex); void *tex=wonder_storage_find(&k); pthread_mutex_unlock(&replay_mutex);
 return tex;
}
void wonder_host_pool_forget(const void *pool){ pthread_mutex_lock(&replay_mutex); wonder_storage_forget_pool(pool); pthread_mutex_unlock(&replay_mutex); }
// nvnTextureFinalize: drops the CPU-uploaded copy (Metal keeps objects alive for command buffers already encoded).
void wonder_host_texture_forget(uint64_t serial){
 wonder_host_draw_forget_sampled(serial);   // GPU-written storage outlives the object (see above)
}
void *wonder_host_render_target(const struct wonder_nvn_texture *t, uint32_t format){
 if(t->host) return t->host;   // display textures own their render target
 if(!device||!format) return NULL;
 pthread_mutex_lock(&replay_mutex);
 struct wonder_storage_key k=storage_key(t);
 id<MTLTexture> tex=(__bridge id<MTLTexture>)wonder_storage_find(&k);
 if(!tex){
  MTLTextureDescriptor *d=wonder_host_render_target_descriptor(&t->desc,(MTLPixelFormat)format);   // 2D/array/rectangle/cube/cube array
  tex=d?[device newTextureWithDescriptor:d]:nil;
  if(tex){ tex.label=[NSString stringWithFormat:@"NVN texture %llu %ux%u fmt %#x",(unsigned long long)t->serial,t->desc.width,t->desc.height,t->desc.format]; wonder_storage_insert(&k,(__bridge_retained void*)tex,release_texture); }
 }
 pthread_mutex_unlock(&replay_mutex);
 return (__bridge void*)tex;
}
// Full clears are queued (metal3_draw.m): folded into the next pass's Clear load when that is provably the same.
static int encode_pass(MTLRenderPassDescriptor *pass){
 pthread_mutex_lock(&replay_mutex); wonder_host_draw_queue_clear_locked((__bridge void*)pass); pthread_mutex_unlock(&replay_mutex); return 0;
}
int wonder_host_clear_color(void *texture,int level,int slice,const float rgba[4]){
 id<MTLTexture> t=(__bridge id<MTLTexture>)texture; if(!t) return EINVAL;
 MTLRenderPassDescriptor *p=[MTLRenderPassDescriptor renderPassDescriptor];
 p.colorAttachments[0].texture=t; p.colorAttachments[0].level=level;
 if(t.textureType==MTLTextureType3D) p.colorAttachments[0].depthPlane=slice; else p.colorAttachments[0].slice=slice;   // 3D: z slice
 p.colorAttachments[0].loadAction=MTLLoadActionClear; p.colorAttachments[0].storeAction=MTLStoreActionStore;
 p.colorAttachments[0].clearColor=MTLClearColorMake(rgba[0],rgba[1],rgba[2],rgba[3]);
 return encode_pass(p);
}
int wonder_host_clear_depth_stencil(void *texture,int level,int slice,int clear_depth,float depth,int clear_stencil,uint32_t stencil){
 id<MTLTexture> t=(__bridge id<MTLTexture>)texture; if(!t) return EINVAL;
 MTLPixelFormat f=t.pixelFormat; int has_depth=f!=MTLPixelFormatStencil8, has_stencil=f==MTLPixelFormatStencil8||f==MTLPixelFormatDepth32Float_Stencil8;
 MTLRenderPassDescriptor *p=[MTLRenderPassDescriptor renderPassDescriptor];
 if(has_depth){ p.depthAttachment.texture=t; p.depthAttachment.level=level; p.depthAttachment.slice=slice;
  p.depthAttachment.loadAction=clear_depth?MTLLoadActionClear:MTLLoadActionLoad; p.depthAttachment.storeAction=MTLStoreActionStore; p.depthAttachment.clearDepth=depth; }
 if(has_stencil){ p.stencilAttachment.texture=t; p.stencilAttachment.level=level; p.stencilAttachment.slice=slice;
  p.stencilAttachment.loadAction=clear_stencil?MTLLoadActionClear:MTLLoadActionLoad; p.stencilAttachment.storeAction=MTLStoreActionStore; p.stencilAttachment.clearStencil=stencil; }
 return encode_pass(p);
}
int wonder_host_fill_buffer(void *buffer,size_t offset,size_t size,uint8_t value){
 id<MTLBuffer> b=(__bridge id<MTLBuffer>)buffer; if(!b||offset+size>b.length) return EINVAL;
 pthread_mutex_lock(&replay_mutex);
 wonder_host_draw_queue_work_locked(^(id<MTLCommandBuffer> cb){ id<MTLBlitCommandEncoder> e=[cb blitCommandEncoder]; [e fillBuffer:b range:NSMakeRange(offset,size) value:value]; [e endEncoding]; });
 pthread_mutex_unlock(&replay_mutex);
 return 0;
}
int wonder_host_write_buffer(void *buffer,size_t offset,const void *data,size_t size){
 id<MTLBuffer> b=(__bridge id<MTLBuffer>)buffer; if(!b||offset+size>b.length) return EINVAL;
 id<MTLBuffer> staging=[device newBufferWithBytes:data length:size options:MTLResourceStorageModeShared]; if(!staging) return ENOMEM;
 pthread_mutex_lock(&replay_mutex);
 wonder_host_draw_queue_work_locked(^(id<MTLCommandBuffer> cb){ id<MTLBlitCommandEncoder> e=[cb blitCommandEncoder];
  [e copyFromBuffer:staging sourceOffset:0 toBuffer:b destinationOffset:offset size:size]; [e endEncoding]; });
 pthread_mutex_unlock(&replay_mutex);
 return 0;
}
static NSString *const present_source=@"#include <metal_stdlib>\nusing namespace metal;\n"
 "struct V{float4 p [[position]];float2 uv;};\nstruct R{float4 dst;float4 src;};\n"
 "vertex V present_vs(uint id [[vertex_id]],constant R &r [[buffer(0)]]){float2 c=float2(id&1,id>>1);V o;"
 "o.p=float4(mix(r.dst.x,r.dst.z,c.x),mix(r.dst.y,r.dst.w,c.y),0,1);o.uv=float2(mix(r.src.x,r.src.z,c.x),mix(r.src.y,r.src.w,c.y));return o;}\n"
 "fragment float4 present_fs(V i [[stage_in]],texture2d<float> t [[texture(0)]],sampler s [[sampler(0)]]){return t.sample(s,i.uv);}\n";
static int present_setup(void){
 if(present_pipeline) return 0;
 NSError *error=nil; id<MTLLibrary> lib=[device newLibraryWithSource:present_source options:nil error:&error];
 if(!lib){ fprintf(stderr,"HOST_PRESENT_SHADER_FAILED %s\n",error.localizedDescription.UTF8String); return EIO; }
 MTLRenderPipelineDescriptor *d=[MTLRenderPipelineDescriptor new]; d.vertexFunction=[lib newFunctionWithName:@"present_vs"]; d.fragmentFunction=[lib newFunctionWithName:@"present_fs"];
 d.colorAttachments[0].pixelFormat=MTLPixelFormatBGRA8Unorm;
 present_pipeline=[device newRenderPipelineStateWithDescriptor:d error:&error];
 MTLSamplerDescriptor *s=[MTLSamplerDescriptor new]; s.minFilter=s.magFilter=MTLSamplerMinMagFilterLinear; present_sampler=[device newSamplerStateWithDescriptor:s];
 if(!present_pipeline) fprintf(stderr,"HOST_PRESENT_PIPELINE_FAILED %s\n",error.localizedDescription.UTF8String);
 return present_pipeline?0:EIO;
}
// The game's display texture bytes go to the screen unchanged: sRGB textures are read through a UNORM view
// so the BGRA8Unorm drawable receives the encoded values. Aspect-fit with black bars; row 0 is the top
// (the game sets NVN_WINDOW_ORIGIN_MODE_UPPER_LEFT).
// Evidence of what reaches the screen: the presented texture is saved as Documents/Present/present-<n>.png for the first
// 5 presents, then every 60th (at most 20 files), before it is drawn to the drawable.
// WONDER_PRESENT_EVERY=<n> (diagnostic) changes the period and allows 60 files, for animations and loops.
// Requested captures (scripted input asks for one 1.5 s after each press, host_input.m): the next present is saved.
static _Atomic int capture_requested;
void wonder_host_request_present_capture(void){ atomic_store(&capture_requested,1); }
static void capture_present(id<MTLTexture> t,const int crop[4]){   // saved as the crop rectangle (what the screen shows)
 static unsigned presents, saved, every, limit; unsigned n=++presents;
 if(atomic_exchange(&capture_requested,0)){
  NSString *dir=[NSHomeDirectory() stringByAppendingPathComponent:@"Documents/Present"];
  [NSFileManager.defaultManager createDirectoryAtPath:dir withIntermediateDirectories:YES attributes:nil error:nil];
  NSString *path=[dir stringByAppendingPathComponent:[NSString stringWithFormat:@"present-%u.png",n]];
  int r=wonder_host_readback_png_rect((__bridge void*)t,0,0,crop,path.fileSystemRepresentation);
  fprintf(stderr,"HOST_PRESENT_CAPTURE Documents/Present/present-%u.png requested result=%d\n",n,r);
  return;
 }
 if(!every){ const char *e=getenv("WONDER_PRESENT_EVERY"); every=e&&atoi(e)>0?(unsigned)atoi(e):60; limit=e?60:20; }
 // The first frames and every `every` presents up to the limit, then one every 1800 presents (~30 s) for the rest of
 // the run (a long scripted round shows where it got to).
 if(!(n<=5||(saved<limit&&n%every==0)||n%1800==0)) return;
 NSString *dir=[NSHomeDirectory() stringByAppendingPathComponent:@"Documents/Present"];
 [NSFileManager.defaultManager createDirectoryAtPath:dir withIntermediateDirectories:YES attributes:nil error:nil];
 if(!wonder_host_readback_png_rect((__bridge void*)t,0,0,crop,[dir stringByAppendingPathComponent:[NSString stringWithFormat:@"present-%u.png",n]].fileSystemRepresentation)) saved++;
}
int wonder_host_present(void *texture,const int crop[4],int interval){
 (void)interval;
 id<MTLTexture> t=(__bridge id<MTLTexture>)texture; if(!t||!device||!surface) return EINVAL;
 capture_present(t,crop);
 {  // on-screen frame rate (fps_meter.h; LoaderProbe.m shows it)
  static struct wonder_fps_meter meter; int fps=wonder_fps_meter_present(&meter,CACurrentMediaTime()); if(fps>=0) wonder_fps_publish(fps);
 }
 static unsigned presents; if(++presents%300==0){   // memory evidence: iOS stops the app at the high watermark (round95: 2098 MB)
  task_vm_info_data_t vm; mach_msg_type_number_t n=TASK_VM_INFO_COUNT; uint64_t footprint=task_info(mach_task_self(),TASK_VM_INFO,(task_info_t)&vm,&n)==KERN_SUCCESS?vm.phys_footprint:0;
  pthread_mutex_lock(&replay_mutex); unsigned targets=wonder_storage_count(); pthread_mutex_unlock(&replay_mutex);
  if(presents%1800==0){   // every ~30 s: guest threads blocked >= 5 s (a stalled load names its waiter)
   extern void wonder_guest_frames(uint64_t,uint64_t,char *,size_t);
   wonder_kernel_dump_waits(INT64_C(5000000000),wonder_guest_frames);
   for(int k=0;k<3;k++){ wonder_kernel_sample_running(wonder_guest_frames); usleep(3000); }   // a polling loop shows the same frames
  }
  extern size_t wonder_nvn_glyph_cache_nonzero(uint64_t *serial); uint64_t gserial=0; size_t glyphs=wonder_nvn_glyph_cache_nonzero(&gserial);
  extern void wonder_host_draw_batch_stats(unsigned *draws, unsigned *passes); unsigned bd, bp; wonder_host_draw_batch_stats(&bd,&bp);
  static unsigned last_bd, last_bp, last_hv; static double last_t; extern unsigned wonder_host_draw_sampled_validations(void); extern unsigned wonder_guard_count(void); double now=CACurrentMediaTime();
  char tbdr[224]; wonder_host_tbdr_window(tbdr,sizeof tbdr);
  fprintf(stderr,"HOST_MEMORY present=%u footprint_mb=%llu render_targets=%u sampled=%u glyph_cache(serial %llu) nonzero/64=%zu draws/passes=%u/%u fps=%.1f texture_hashes=%u watched=%u %s\n",presents,(unsigned long long)(footprint>>20),targets,wonder_host_draw_sampled_count(),(unsigned long long)gserial,glyphs,
          bd-last_bd,bp-last_bp,last_t>0?300.0/(now-last_t):0.0,wonder_host_draw_sampled_validations()-last_hv,wonder_guard_count(),tbdr);
  {  // round222 accounting over these 300 presents (ms): replay idle / finish waits / command buffer allocation / drawable, GPU time
   static uint64_t l_fin, l_fn, l_cb, l_dr, l_idle, l_gpu, l_cbs;
   uint64_t fin=atomic_load(&acct_finish_ns), fn=atomic_load(&acct_finish_n), cb=atomic_load(&acct_cbuf_ns), dr=atomic_load(&acct_drawable_ns), idle=atomic_load(&acct_idle_ns), gpu=atomic_load(&acct_gpu_ns), cbs=atomic_load(&acct_cbufs);
   extern _Atomic uint64_t wonder_nvn_acct[6]; static uint64_t l_n[6]; uint64_t n6[6]; for(int k=0;k<6;k++) n6[k]=atomic_load(&wonder_nvn_acct[k]);
   fprintf(stderr,"HOST_FRAME_TIME presents=300 wall_ms=%.0f replay_idle_ms=%.0f finish_ms=%.0f(%llu: queue_finish %llu, replay %llu) cbuf_alloc_ms=%.0f(%llu) drawable_ms=%.0f gpu_ms=%.0f game: acquire_wait_ms=%.0f sync_wait_ms=%.0f(%llu) submit_ms=%.0f\n",last_t>0?(now-last_t)*1e3:0.0,
           (idle-l_idle)/1e6,(fin-l_fin)/1e6,(unsigned long long)(fn-l_fn),(unsigned long long)(n6[0]-l_n[0]),(unsigned long long)(n6[1]-l_n[1]),(cb-l_cb)/1e6,(unsigned long long)(cbs-l_cbs),(dr-l_dr)/1e6,(gpu-l_gpu)/1e6,
           (n6[2]-l_n[2])/1e6,(n6[3]-l_n[3])/1e6,(unsigned long long)(n6[4]-l_n[4]),(n6[5]-l_n[5])/1e6);
   memcpy(l_n,n6,sizeof n6);
   l_fin=fin; l_fn=fn; l_cb=cb; l_dr=dr; l_idle=idle; l_gpu=gpu; l_cbs=cbs;
  }
  last_hv=wonder_host_draw_sampled_validations();
  last_bd=bd; last_bp=bp; last_t=now;
 }
 if(present_setup()) return EIO;
 if(t.pixelFormat==MTLPixelFormatRGBA8Unorm_sRGB) t=[t newTextureViewWithPixelFormat:MTLPixelFormatRGBA8Unorm];
 else if(t.pixelFormat==MTLPixelFormatBGRA8Unorm_sRGB) t=[t newTextureViewWithPixelFormat:MTLPixelFormatBGRA8Unorm];
 CAMetalLayer *layer=(CAMetalLayer*)surface.layer; uint64_t td=now_ns(); id<CAMetalDrawable> drawable=[layer nextDrawable]; atomic_fetch_add(&acct_drawable_ns,now_ns()-td); if(!drawable) return EAGAIN;
 // Widescreen (widescreen.h): the game framed a wider view into this 16:9 image; stretching it to the game's aspect
 // undoes the squeeze. Then it is fitted to the drawable as before (bars only past WONDER_WIDESCREEN_MAX_ASPECT).
 double stretch=wonder_widescreen_get()/((double)crop[2]/crop[3]); if(stretch<1) stretch=1;
 double W=drawable.texture.width,H=drawable.texture.height,w=crop[2]*stretch,h=crop[3],scale=fmin(W/w,H/h),dw=w*scale/W,dh=h*scale/H;
 struct { float dst[4], src[4]; } r={{(float)-dw,(float)dh,(float)dw,(float)-dh},
  {(float)(crop[0]/(double)t.width),(float)(crop[1]/(double)t.height),(float)((crop[0]+w)/t.width),(float)((crop[1]+h)/t.height)}};
 MTLRenderPassDescriptor *p=[MTLRenderPassDescriptor renderPassDescriptor];
 p.colorAttachments[0].texture=drawable.texture; p.colorAttachments[0].loadAction=MTLLoadActionClear; p.colorAttachments[0].storeAction=MTLStoreActionStore;
 p.colorAttachments[0].clearColor=MTLClearColorMake(0,0,0,1);
 pthread_mutex_lock(&replay_mutex);
 id<MTLCommandBuffer> b=pending_buffer(); id<MTLRenderCommandEncoder> e=[b renderCommandEncoderWithDescriptor:p];
 [e setRenderPipelineState:present_pipeline]; [e setVertexBytes:&r length:sizeof r atIndex:0];
 [e setFragmentTexture:t atIndex:0]; [e setFragmentSamplerState:present_sampler atIndex:0];
 [e drawPrimitives:MTLPrimitiveTypeTriangleStrip vertexStart:0 vertexCount:4]; [e endEncoding];
 [b presentDrawable:drawable]; commit_locked();
 pthread_mutex_unlock(&replay_mutex);
 return 0;
}

// First executed draw: read its colour target back into Documents/FirstDraw/target-0.png (nvn_queue.c calls this once).
void wonder_host_capture_first_draw(const struct wonder_nvn_texture_snapshot *target){
 void *t=wonder_host_render_target_existing(&target->texture);
 NSString *dir=[NSHomeDirectory() stringByAppendingPathComponent:@"Documents/FirstDraw"];
 [NSFileManager.defaultManager createDirectoryAtPath:dir withIntermediateDirectories:YES attributes:nil error:nil];
 int level=target->has_view&&target->view.set_levels?target->view.base_level:0, slice=target->has_view&&target->view.set_layers?target->view.min_layer:0;
 int r=t?wonder_host_readback_png(t,level,slice,[dir stringByAppendingPathComponent:@"target-0.png"].fileSystemRepresentation):-1;
 fprintf(stderr,"HOST_FIRST_DRAW_CAPTURE serial=%llu format=%#x %ux%u result=%d\n",(unsigned long long)target->texture.serial,target->texture.desc.format,target->texture.desc.width,target->texture.desc.height,r);
}

// Diagnostic capture of a render target into Documents/<name> (after its submit; e.g. Text/text-0.png).
void wonder_host_capture_target(const struct wonder_nvn_texture_snapshot *target,const char *name){
 void *t=wonder_host_render_target_existing(&target->texture);
 NSString *path=[NSHomeDirectory() stringByAppendingPathComponent:[@"Documents" stringByAppendingPathComponent:@(name)]];
 [NSFileManager.defaultManager createDirectoryAtPath:path.stringByDeletingLastPathComponent withIntermediateDirectories:YES attributes:nil error:nil];
 int level=target->has_view&&target->view.set_levels?target->view.base_level:0, slice=target->has_view&&target->view.set_layers?target->view.min_layer:0;
 int r=t?wonder_host_readback_png(t,level,slice,path.fileSystemRepresentation):-1;
 fprintf(stderr,"HOST_TARGET_CAPTURE Documents/%s serial=%llu format=%#x %ux%u result=%d\n",name,(unsigned long long)target->texture.serial,target->texture.desc.format,target->texture.desc.width,target->texture.desc.height,r);
}

// ---------- Host replay thread ----------
// An ordered job queue (round221): submits, fences, presents and flushes from the game thread run here in the order
// they were issued, as the console GPU front end consumes its queue behind the CPU. Asynchronous posts return at once
// (the game thread records the next frame while this one is replayed); synchronous runs wait for their job and with it
// for every job posted before. WONDER_ASYNC_SUBMIT=0 makes every post synchronous (the replay of rounds <= 220).
struct replay_job { void (*fn)(void *); void *arg; int *done; struct replay_job *next; };
static pthread_mutex_t job_lock=PTHREAD_MUTEX_INITIALIZER; static pthread_cond_t job_ready=PTHREAD_COND_INITIALIZER, job_done=PTHREAD_COND_INITIALIZER, job_room=PTHREAD_COND_INITIALIZER;
static struct replay_job *job_head, *job_tail; static unsigned job_count; static pthread_t replay_thread;
static _Atomic mach_port_t replay_port;
#define REPLAY_QUEUE_LIMIT 256   // queued jobs before a post waits (the game paces itself at nvnWindowAcquireTexture)
static void *replay_thread_main(void *unused){
 (void)unused;
 atomic_store(&replay_port,pthread_mach_thread_np(pthread_self()));
 for(;;){
  pthread_mutex_lock(&job_lock); uint64_t ti=now_ns(); while(!job_head) pthread_cond_wait(&job_ready,&job_lock); atomic_fetch_add(&acct_idle_ns,now_ns()-ti);
  struct replay_job *j=job_head; pthread_mutex_unlock(&job_lock);
  @autoreleasepool { j->fn(j->arg); }   // a stop inside exits this thread; the guest is parked by the stop
  pthread_mutex_lock(&job_lock);
  job_head=j->next; if(!job_head) job_tail=NULL; job_count--;
  if(j->done){ *j->done=1; pthread_cond_broadcast(&job_done); }
  pthread_cond_broadcast(&job_room); pthread_mutex_unlock(&job_lock);
  if(!j->done) free(j);
 }
 return NULL;
}
static void start_replay_thread(void){
 static int started; static pthread_mutex_t start_lock=PTHREAD_MUTEX_INITIALIZER;
 pthread_mutex_lock(&start_lock);
 if(!started){ pthread_attr_t a; pthread_attr_init(&a); pthread_attr_setstacksize(&a,16u<<20);
  if(pthread_create(&replay_thread,&a,replay_thread_main,NULL)){ fprintf(stderr,"HOST_REPLAY_THREAD_FAILED\n"); abort(); } started=1; }
 pthread_mutex_unlock(&start_lock);
}
static void enqueue_locked(struct replay_job *j){
 j->next=NULL; if(job_tail) job_tail->next=j; else job_head=j; job_tail=j; job_count++; pthread_cond_signal(&job_ready);
}
void wonder_host_run_on_replay_stack(void (*fn)(void *),void *arg){
 start_replay_thread();
 if(pthread_equal(pthread_self(),replay_thread)){ fn(arg); return; }
 int done=0; struct replay_job j={fn,arg,&done,NULL};
 pthread_mutex_lock(&job_lock); enqueue_locked(&j);
 while(!done) pthread_cond_wait(&job_done,&job_lock);
 pthread_mutex_unlock(&job_lock);
}
static int async_posts(void){ static int on=-1; if(on<0){ const char *e=getenv("WONDER_ASYNC_SUBMIT"); on=!(e&&*e=='0'); fprintf(stderr,"HOST_REPLAY_QUEUE %s\n",on?"async (WONDER_ASYNC_SUBMIT=0 for synchronous)":"synchronous (WONDER_ASYNC_SUBMIT=0)"); } return on; }
void wonder_host_post_to_replay(void (*fn)(void *),void *arg){
 start_replay_thread();
 if(pthread_equal(pthread_self(),replay_thread)||!async_posts()){ wonder_host_run_on_replay_stack(fn,arg); return; }
 struct replay_job *j=malloc(sizeof *j); if(!j){ fprintf(stderr,"HOST_REPLAY_QUEUE out of memory\n"); abort(); }
 *j=(struct replay_job){fn,arg,NULL,NULL};
 pthread_mutex_lock(&job_lock);
 while(job_count>=REPLAY_QUEUE_LIMIT) pthread_cond_wait(&job_room,&job_lock);
 enqueue_locked(j); pthread_mutex_unlock(&job_lock);
}

// ---------- Frame thread time constraints (rt_declare.c) ----------
// The presenting game thread and the replay thread (it runs every submit while the game thread waits) are declared
// time-constraint threads: period = constraint = present period (interval x 1/60 s), computation from each thread's own
// measured CPU per frame. WONDER_RT=0 (diagnostic) leaves both at their QoS; WONDER_RT_COMP=<ms> pins the computation.
// Every 600 frames the policy is read back; a thread that lost it (e.g. a guest priority change re-set its QoS) is
// declared again.
#include "rt_declare.h"
#include <mach/thread_policy.h>
#include <mach/mach_time.h>
static uint64_t thread_cpu_ns(mach_port_t port){
 thread_basic_info_data_t info; mach_msg_type_number_t n=THREAD_BASIC_INFO_COUNT;
 if(thread_info(port,THREAD_BASIC_INFO,(thread_info_t)&info,&n)!=KERN_SUCCESS) return 0;
 return ((uint64_t)info.user_time.seconds+info.system_time.seconds)*1000000000ull+((uint64_t)info.user_time.microseconds+info.system_time.microseconds)*1000ull;
}
static int rt_active(mach_port_t port){
 thread_time_constraint_policy_data_t p; mach_msg_type_number_t n=THREAD_TIME_CONSTRAINT_POLICY_COUNT; boolean_t def=FALSE;
 return thread_policy_get(port,THREAD_TIME_CONSTRAINT_POLICY,(thread_policy_t)&p,&n,&def)==KERN_SUCCESS&&!def;
}
static void rt_declare(const char *who,mach_port_t port,uint64_t period_ns,uint64_t comp_ns){
 static mach_timebase_info_data_t tb; if(!tb.denom) mach_timebase_info(&tb);
 thread_time_constraint_policy_data_t p={.period=(uint32_t)(period_ns*tb.denom/tb.numer),.computation=(uint32_t)(comp_ns*tb.denom/tb.numer),
  .constraint=(uint32_t)(period_ns*tb.denom/tb.numer),.preemptible=1};
 kern_return_t kr=thread_policy_set(port,THREAD_TIME_CONSTRAINT_POLICY,(thread_policy_t)&p,THREAD_TIME_CONSTRAINT_POLICY_COUNT);
 static _Atomic unsigned logged; if(atomic_fetch_add(&logged,1)<48)
  fprintf(stderr,"HOST_RT_POLICY thread=%s period_ms=%.3f computation_ms=%.3f result=%d active=%d\n",who,period_ns/1e6,comp_ns/1e6,kr,rt_active(port));
}
struct rt_thread { const char *who; mach_port_t port; struct wonder_rt_decl d; };
static void rt_thread_tick(struct rt_thread *t,mach_port_t port,uint64_t period_ns,uint64_t fixed_ns,int check){
 if(!port) return;
 if(t->port!=port){ memset(&t->d,0,sizeof t->d); t->port=port; }   // another thread presents now
 if(check&&t->d.period_ns&&!rt_active(port)){ fprintf(stderr,"HOST_RT_POLICY thread=%s lost the time constraint, declaring again\n",t->who); t->d.period_ns=0; }
 uint64_t comp=wonder_rt_tick(&t->d,thread_cpu_ns(port),period_ns,fixed_ns);
 if(comp) rt_declare(t->who,port,period_ns,comp);
}
void wonder_host_frame_threads_tick(int interval){
 static int mode=-1; static uint64_t fixed_ns; static unsigned frames; static struct rt_thread game={"present"}, replay={"replay"};
 if(mode<0){ const char *e=getenv("WONDER_RT"); mode=!(e&&!strcmp(e,"0")); const char *c=getenv("WONDER_RT_COMP"); fixed_ns=c&&atoi(c)>0?(uint64_t)atoi(c)*1000000ull:0;
  fprintf(stderr,"HOST_RT_POLICY mode=%s computation=%s\n",mode?"time-constraint":"off (WONDER_RT=0)",fixed_ns?c:"auto"); }
 if(!mode) return;
 uint64_t period_ns=(uint64_t)(interval>0?interval:1)*16666667ull; int check=++frames%600==0;
 rt_thread_tick(&game,pthread_mach_thread_np(pthread_self()),period_ns,fixed_ns,check);
 rt_thread_tick(&replay,atomic_load(&replay_port),period_ns,fixed_ns,check);
}
