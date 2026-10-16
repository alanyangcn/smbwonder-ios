#import "host_graphics.h"
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
static id<MTLCommandBuffer> pending_buffer(void){
 if(!pending){
  pending=[queue commandBuffer]; pending.label=@"Wonder NVN submit";
  [pending addCompletedHandler:^(id<MTLCommandBuffer> b){
   if(b.status==MTLCommandBufferStatusError&&!atomic_exchange(&gpu_failed,1)){
    snprintf(gpu_error_text,sizeof gpu_error_text,"%s in %s",b.error.localizedDescription.UTF8String?:"?",b.label.UTF8String?:"?");
    fprintf(stderr,"HOST_GPU_ERROR %s\n",gpu_error_text);
   }
  }];
 }
 return pending;
}
static unsigned pending_first_submit, pending_last_submit;
static void commit_locked(void){ if(pending){ [pending commit]; last_committed=pending; pending=nil; pending_first_submit=pending_last_submit=0; } }
void wonder_host_note_submit(unsigned submit){
 pthread_mutex_lock(&replay_mutex);
 id<MTLCommandBuffer> b=pending_buffer(); if(!pending_first_submit) pending_first_submit=submit; pending_last_submit=submit;
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
 dispatch_semaphore_wait(done,DISPATCH_TIME_FOREVER);
}
void wonder_host_after_gpu(void (*fn)(void *), void *arg){
 pthread_mutex_lock(&replay_mutex); [pending_buffer() addCompletedHandler:^(id<MTLCommandBuffer> b){ (void)b; fn(arg); }]; pthread_mutex_unlock(&replay_mutex);
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
static int encode_pass(MTLRenderPassDescriptor *pass){
 pthread_mutex_lock(&replay_mutex);
 id<MTLRenderCommandEncoder> e=[pending_buffer() renderCommandEncoderWithDescriptor:pass]; [e endEncoding];
 pthread_mutex_unlock(&replay_mutex); return e?0:EIO;
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
 pthread_mutex_lock(&replay_mutex); id<MTLBlitCommandEncoder> e=[pending_buffer() blitCommandEncoder]; [e fillBuffer:b range:NSMakeRange(offset,size) value:value]; [e endEncoding]; pthread_mutex_unlock(&replay_mutex);
 return 0;
}
int wonder_host_write_buffer(void *buffer,size_t offset,const void *data,size_t size){
 id<MTLBuffer> b=(__bridge id<MTLBuffer>)buffer; if(!b||offset+size>b.length) return EINVAL;
 id<MTLBuffer> staging=[device newBufferWithBytes:data length:size options:MTLResourceStorageModeShared]; if(!staging) return ENOMEM;
 pthread_mutex_lock(&replay_mutex); id<MTLBlitCommandEncoder> e=[pending_buffer() blitCommandEncoder];
 [e copyFromBuffer:staging sourceOffset:0 toBuffer:b destinationOffset:offset size:size]; [e endEncoding]; pthread_mutex_unlock(&replay_mutex);
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
static void capture_present(id<MTLTexture> t){
 static unsigned presents, saved, every, limit; unsigned n=++presents;
 if(!every){ const char *e=getenv("WONDER_PRESENT_EVERY"); every=e&&atoi(e)>0?(unsigned)atoi(e):60; limit=e?60:20; }
 if(saved>=limit||!(n<=5||n%every==0)) return;
 NSString *dir=[NSHomeDirectory() stringByAppendingPathComponent:@"Documents/Present"];
 [NSFileManager.defaultManager createDirectoryAtPath:dir withIntermediateDirectories:YES attributes:nil error:nil];
 if(!wonder_host_readback_png((__bridge void*)t,0,0,[dir stringByAppendingPathComponent:[NSString stringWithFormat:@"present-%u.png",n]].fileSystemRepresentation)) saved++;
}
int wonder_host_present(void *texture,const int crop[4],int interval){
 (void)interval;
 id<MTLTexture> t=(__bridge id<MTLTexture>)texture; if(!t||!device||!surface) return EINVAL;
 capture_present(t);
 static unsigned presents; if(++presents%300==0){   // memory evidence: iOS stops the app at the high watermark (round95: 2098 MB)
  task_vm_info_data_t vm; mach_msg_type_number_t n=TASK_VM_INFO_COUNT; uint64_t footprint=task_info(mach_task_self(),TASK_VM_INFO,(task_info_t)&vm,&n)==KERN_SUCCESS?vm.phys_footprint:0;
  pthread_mutex_lock(&replay_mutex); unsigned targets=wonder_storage_count(); pthread_mutex_unlock(&replay_mutex);
  fprintf(stderr,"HOST_MEMORY present=%u footprint_mb=%llu render_targets=%u sampled=%u\n",presents,(unsigned long long)(footprint>>20),targets,wonder_host_draw_sampled_count());
 }
 if(present_setup()) return EIO;
 if(t.pixelFormat==MTLPixelFormatRGBA8Unorm_sRGB) t=[t newTextureViewWithPixelFormat:MTLPixelFormatRGBA8Unorm];
 else if(t.pixelFormat==MTLPixelFormatBGRA8Unorm_sRGB) t=[t newTextureViewWithPixelFormat:MTLPixelFormatBGRA8Unorm];
 CAMetalLayer *layer=(CAMetalLayer*)surface.layer; id<CAMetalDrawable> drawable=[layer nextDrawable]; if(!drawable) return EAGAIN;
 double W=drawable.texture.width,H=drawable.texture.height,w=crop[2],h=crop[3],scale=fmin(W/w,H/h),dw=w*scale/W,dh=h*scale/H;
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

// ---------- Host replay thread ----------
static pthread_mutex_t job_lock=PTHREAD_MUTEX_INITIALIZER; static pthread_cond_t job_ready=PTHREAD_COND_INITIALIZER, job_done=PTHREAD_COND_INITIALIZER;
static void (*job_fn)(void *); static void *job_arg; static int job_pending, job_finished; static pthread_t replay_thread;
static void *replay_thread_main(void *unused){
 (void)unused;
 for(;;){
  pthread_mutex_lock(&job_lock); while(!job_pending) pthread_cond_wait(&job_ready,&job_lock);
  void (*fn)(void *)=job_fn; void *arg=job_arg; job_pending=0; pthread_mutex_unlock(&job_lock);
  @autoreleasepool { fn(arg); }   // a stop inside exits this thread; the waiting guest thread then stays parked
  pthread_mutex_lock(&job_lock); job_finished=1; pthread_cond_broadcast(&job_done); pthread_mutex_unlock(&job_lock);
 }
 return NULL;
}
void wonder_host_run_on_replay_stack(void (*fn)(void *),void *arg){
 static int started; static pthread_mutex_t start_lock=PTHREAD_MUTEX_INITIALIZER;
 pthread_mutex_lock(&start_lock);
 if(!started){ pthread_attr_t a; pthread_attr_init(&a); pthread_attr_setstacksize(&a,16u<<20);
  if(pthread_create(&replay_thread,&a,replay_thread_main,NULL)){ fprintf(stderr,"HOST_REPLAY_THREAD_FAILED\n"); abort(); } started=1; }
 pthread_mutex_unlock(&start_lock);
 if(pthread_equal(pthread_self(),replay_thread)){ fn(arg); return; }
 pthread_mutex_lock(&job_lock);
 job_fn=fn; job_arg=arg; job_pending=1; job_finished=0; pthread_cond_signal(&job_ready);
 while(!job_finished) pthread_cond_wait(&job_done,&job_lock);
 pthread_mutex_unlock(&job_lock);
}
