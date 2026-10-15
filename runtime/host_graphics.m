#import "host_graphics.h"
#import <Metal/Metal.h>
#import <QuartzCore/CAMetalLayer.h>
#include "kernel_events.h"
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
 d.usage=MTLTextureUsageRenderTarget|MTLTextureUsageShaderRead;d.storageMode=MTLStorageModePrivate;
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
