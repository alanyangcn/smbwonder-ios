#import "host_graphics.h"
#import <Metal/Metal.h>
#import <QuartzCore/CAMetalLayer.h>
#include "kernel_events.h"
#include "metal2_draw.h"
#include "host_draw.h"
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
#include <math.h>
static pthread_mutex_t replay_mutex = PTHREAD_MUTEX_INITIALIZER;
static id<MTLCommandBuffer> pending, last_committed;
static NSMutableDictionary<NSNumber *, id<MTLTexture>> *render_targets;
static id<MTLRenderPipelineState> present_pipeline;
static id<MTLSamplerState> present_sampler;
static id<MTLCommandBuffer> pending_buffer(void){ if(!pending){ pending=[queue commandBuffer]; pending.label=@"Wonder NVN submit"; } return pending; }
static void commit_locked(void){ if(pending){ [pending commit]; last_committed=pending; pending=nil; } }
void wonder_host_flush(void){ pthread_mutex_lock(&replay_mutex); commit_locked(); pthread_mutex_unlock(&replay_mutex); }
void wonder_host_finish(void){
 pthread_mutex_lock(&replay_mutex); commit_locked(); id<MTLCommandBuffer> last=last_committed; pthread_mutex_unlock(&replay_mutex);
 [last waitUntilCompleted];
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
void *wonder_host_render_target_existing(const struct wonder_nvn_texture *t){
 if(t->host) return t->host;
 pthread_mutex_lock(&replay_mutex); id<MTLTexture> tex=render_targets[@(t->serial)]; pthread_mutex_unlock(&replay_mutex);
 return (__bridge void*)tex;
}
void *wonder_host_render_target(const struct wonder_nvn_texture *t, uint32_t format){
 if(t->host) return t->host;   // display textures own their render target
 if(!device||!format) return NULL;
 pthread_mutex_lock(&replay_mutex);
 if(!render_targets) render_targets=[NSMutableDictionary new];
 id<MTLTexture> tex=render_targets[@(t->serial)];
 if(!tex){
  MTLTextureDescriptor *d=[MTLTextureDescriptor new];
  d.textureType=t->desc.target==4?MTLTextureType2DArray:MTLTextureType2D; d.pixelFormat=(MTLPixelFormat)format;
  d.width=t->desc.width; d.height=t->desc.height; d.mipmapLevelCount=t->desc.levels?t->desc.levels:1;
  if(t->desc.target==4) d.arrayLength=t->desc.depth?t->desc.depth:1;
  d.usage=MTLTextureUsageRenderTarget|MTLTextureUsageShaderRead|MTLTextureUsagePixelFormatView; d.storageMode=MTLStorageModePrivate;
  tex=[device newTextureWithDescriptor:d];
  if(tex){ tex.label=[NSString stringWithFormat:@"NVN texture %llu %ux%u fmt %#x",(unsigned long long)t->serial,t->desc.width,t->desc.height,t->desc.format]; render_targets[@(t->serial)]=tex; }
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
 p.colorAttachments[0].texture=t; p.colorAttachments[0].level=level; p.colorAttachments[0].slice=slice;
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
int wonder_host_present(void *texture,const int crop[4],int interval){
 (void)interval;
 id<MTLTexture> t=(__bridge id<MTLTexture>)texture; if(!t||!device||!surface) return EINVAL;
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
