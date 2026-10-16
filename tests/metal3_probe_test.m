#import <Foundation/Foundation.h>
#import <Metal/Metal.h>
#import "../runtime/metal3_probe.h"
#include <assert.h>
int main(int argc,char **argv){(void)argc;@autoreleasepool{
    id<MTLDevice> device=MTLCreateSystemDefaultDevice();if(!device){puts("METAL3_HOST_DEVICE_UNAVAILABLE");return 77;}
    NSError *error=nil;NSDictionary *result=wonder_metal3_run(device,[NSString stringWithUTF8String:argv[1]],&error);
    if(!result){fprintf(stderr,"GPU probe failed: %s\n",error.description.UTF8String);return 1;}
    assert([result[@"bad_pixels"] unsignedIntValue]==0);assert([result[@"negative_control_rejected"] boolValue]);
    puts("METAL3_REAL_GPU_TEST_OK");return 0;
}}
