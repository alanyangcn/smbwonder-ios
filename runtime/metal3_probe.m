#import "metal3_probe.h"
#import <CoreGraphics/CoreGraphics.h>
#import <ImageIO/ImageIO.h>
#include "metal3_pixels.h"
static NSDictionary *failed(NSError **error,NSString *stage,NSError *underlying){
    fprintf(stderr,"[metal3] FAILED stage=%s detail=%s\n",stage.UTF8String,underlying.description.UTF8String?:"none");
    if(error)*error=[NSError errorWithDomain:@"WonderMetal3" code:1 userInfo:@{NSLocalizedDescriptionKey:stage,NSUnderlyingErrorKey:underlying?:[NSError errorWithDomain:@"WonderMetal3" code:2 userInfo:nil]}];
    return nil;
}
static BOOL save_png(NSData *pixels,NSString *path){
    CGColorSpaceRef space=CGColorSpaceCreateDeviceRGB();
    CGDataProviderRef provider=CGDataProviderCreateWithCFData((__bridge CFDataRef)pixels);
    CGImageRef image=CGImageCreate(64,64,8,32,256,space,kCGImageAlphaLast|kCGBitmapByteOrderDefault,provider,NULL,false,kCGRenderingIntentDefault);
    CGImageDestinationRef destination=CGImageDestinationCreateWithURL((__bridge CFURLRef)[NSURL fileURLWithPath:path],CFSTR("public.png"),1,NULL);
    BOOL ok=NO;
    if(image&&destination){CGImageDestinationAddImage(destination,image,NULL);ok=CGImageDestinationFinalize(destination);}
    if(destination)CFRelease(destination);if(image)CGImageRelease(image);CGDataProviderRelease(provider);CGColorSpaceRelease(space);return ok;
}
NSDictionary *wonder_metal3_run(id<MTLDevice> device,NSString *directory,NSError **error){
    if(!device)return failed(error,@"Metal device unavailable",nil);
    fprintf(stderr,"[metal3] BEGIN device=%s MSL=3.2 explicit_slots=1\n",device.name.UTF8String);
    NSString *source=@"#include <metal_stdlib>\nusing namespace metal;\n"
    "struct V { float2 p; float2 uv; };\nstruct O { float4 p [[position]]; float2 uv; };\n"
    "vertex O probe_v(uint i [[vertex_id]], const device V* v [[buffer(0)]]) { O o; o.p=float4(v[i].p,0,1); o.uv=v[i].uv; return o; }\n"
    "fragment float4 probe_f(O i [[stage_in]], texture2d<float> tex [[texture(0)]], sampler smp [[sampler(0)]], constant float4& tint [[buffer(1)]]) { return tex.sample(smp,i.uv)*tint; }\n";
    MTLCompileOptions *options=[MTLCompileOptions new];options.languageVersion=MTLLanguageVersion3_2;
    NSError *underlying=nil;
    id<MTLLibrary> library=[device newLibraryWithSource:source options:options error:&underlying];
    if(!library)return failed(error,@"MSL 2.1 compilation",underlying);
    MTLRenderPipelineDescriptor *pipeline=[MTLRenderPipelineDescriptor new];
    pipeline.vertexFunction=[library newFunctionWithName:@"probe_v"];pipeline.fragmentFunction=[library newFunctionWithName:@"probe_f"];
    pipeline.colorAttachments[0].pixelFormat=MTLPixelFormatRGBA8Unorm;
    id<MTLRenderPipelineState> state=[device newRenderPipelineStateWithDescriptor:pipeline error:&underlying];
    if(!state)return failed(error,@"render pipeline creation",underlying);
    const float vertices[6][4]={{-1,1,0,0},{-1,-1,0,1},{1,1,1,0},{1,1,1,0},{-1,-1,0,1},{1,-1,1,1}};
    id<MTLBuffer> vertex=[device newBufferWithBytes:vertices length:sizeof vertices options:MTLResourceStorageModeShared];
    id<MTLBuffer> output=[device newBufferWithLength:64*256 options:MTLResourceStorageModeShared];
    MTLTextureDescriptor *td=[MTLTextureDescriptor texture2DDescriptorWithPixelFormat:MTLPixelFormatRGBA8Unorm width:2 height:2 mipmapped:NO];
    td.usage=MTLTextureUsageShaderRead;td.storageMode=MTLStorageModeShared;
    id<MTLTexture> texture=[device newTextureWithDescriptor:td];
    const unsigned char colors[16]={255,32,16,255,32,255,32,255,16,64,255,255,255,255,255,255};
    if(!vertex||!output||!texture)return failed(error,@"vertex/texture/readback allocation",nil);
    [texture replaceRegion:MTLRegionMake2D(0,0,2,2) mipmapLevel:0 withBytes:colors bytesPerRow:8];
    MTLSamplerDescriptor *sd=[MTLSamplerDescriptor new];sd.minFilter=MTLSamplerMinMagFilterNearest;sd.magFilter=MTLSamplerMinMagFilterNearest;
    sd.sAddressMode=MTLSamplerAddressModeClampToEdge;sd.tAddressMode=MTLSamplerAddressModeClampToEdge;
    id<MTLSamplerState> sampler=[device newSamplerStateWithDescriptor:sd];
    td=[MTLTextureDescriptor texture2DDescriptorWithPixelFormat:MTLPixelFormatRGBA8Unorm width:64 height:64 mipmapped:NO];
    td.usage=MTLTextureUsageRenderTarget;td.storageMode=MTLStorageModePrivate;
    id<MTLTexture> target=[device newTextureWithDescriptor:td];id<MTLCommandQueue> queue=[device newCommandQueue];
    if(!sampler||!target||!queue)return failed(error,@"sampler/target/queue creation",nil);
    NSData *positive=nil;unsigned bad=0,negativeBad=0;
    for(unsigned trial=0;trial<2;trial++){
        const float good[4]={.5f,.25f,.75f,1},wrong[4]={1,1,1,1};
        id<MTLBuffer> tint=[device newBufferWithBytes:trial?wrong:good length:sizeof good options:MTLResourceStorageModeShared];
        MTLRenderPassDescriptor *pass=[MTLRenderPassDescriptor renderPassDescriptor];
        pass.colorAttachments[0].texture=target;pass.colorAttachments[0].loadAction=MTLLoadActionClear;pass.colorAttachments[0].storeAction=MTLStoreActionStore;
        pass.colorAttachments[0].clearColor=MTLClearColorMake(0,0,0,1);
        id<MTLCommandBuffer> command=[queue commandBuffer];id<MTLRenderCommandEncoder> render=[command renderCommandEncoderWithDescriptor:pass];
        if(!tint||!command||!render)return failed(error,@"render encoding",nil);
        [render setRenderPipelineState:state];[render setVertexBuffer:vertex offset:0 atIndex:0];
        [render setFragmentBuffer:tint offset:0 atIndex:1];[render setFragmentTexture:texture atIndex:0];[render setFragmentSamplerState:sampler atIndex:0];
        [render drawPrimitives:MTLPrimitiveTypeTriangle vertexStart:0 vertexCount:6];[render endEncoding];
        id<MTLBlitCommandEncoder> blit=[command blitCommandEncoder];if(!blit)return failed(error,@"blit encoder",nil);
        [blit copyFromTexture:target sourceSlice:0 sourceLevel:0 sourceOrigin:MTLOriginMake(0,0,0) sourceSize:MTLSizeMake(64,64,1) toBuffer:output destinationOffset:0 destinationBytesPerRow:256 destinationBytesPerImage:64*256];
        [blit endEncoding];[command commit];[command waitUntilCompleted];
        if(command.status!=MTLCommandBufferStatusCompleted)return failed(error,@"GPU command completion",command.error);
        unsigned mismatch=0;int check=wonder_metal3_check_pixels(output.contents,64,64,256,&mismatch);
        fprintf(stderr,"[metal3] GPU_READBACK trial=%u bad_pixels=%u status=%ld\n",trial,mismatch,(long)command.status);
        if(!trial){if(check)return failed(error,@"positive pixel readback mismatch",nil);bad=mismatch;positive=[NSData dataWithBytes:output.contents length:64*256];}
        else {if(check!=1||mismatch!=4096)return failed(error,@"wrong-uniform negative control was not rejected",nil);negativeBad=mismatch;}
    }
    if(![NSFileManager.defaultManager createDirectoryAtPath:directory withIntermediateDirectories:YES attributes:nil error:&underlying])return failed(error,@"diagnostic output directory",underlying);
    NSString *png=[directory stringByAppendingPathComponent:@"metal3-probe.png"];
    if(!save_png(positive,png))return failed(error,@"PNG export",nil);
    NSDictionary *result=@{@"schema":@1,@"scope":@"host GPU diagnostic; not Wonder game frame",@"backend":@"metal3",@"msl":@"3.2",@"device":device.name,@"width":@64,@"height":@64,@"pixels_checked":@4096,@"bad_pixels":@(bad),@"negative_control_bad_pixels":@(negativeBad),@"negative_control_rejected":@YES,@"explicit_binding":@YES,@"gpu_completed":@YES,@"png":png.lastPathComponent};
    NSData *json=[NSJSONSerialization dataWithJSONObject:result options:NSJSONWritingPrettyPrinted error:&underlying];
    if(!json||![json writeToFile:[directory stringByAppendingPathComponent:@"metal3-probe.json"] options:NSDataWritingAtomic error:&underlying])return failed(error,@"JSON export",underlying);
    fprintf(stderr,"[metal3] METAL3_GPU_PROBE_OK verified_pixels=4096 negative_bad_pixels=%u\n",negativeBad);
    return result;
}
