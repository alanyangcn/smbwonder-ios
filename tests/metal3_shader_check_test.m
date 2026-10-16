#import <Foundation/Foundation.h>
#import <Metal/Metal.h>
#import <CommonCrypto/CommonDigest.h>
#import "../runtime/metal3_shader_check.h"
#include <assert.h>
static NSString *hash(NSData *d){unsigned char h[CC_SHA256_DIGEST_LENGTH];CC_SHA256(d.bytes,(CC_LONG)d.length,h);NSMutableString *s=[NSMutableString new];for(unsigned i=0;i<sizeof h;i++)[s appendFormat:@"%02x",h[i]];return s;}
int main(int argc,char **argv){(void)argc;@autoreleasepool{
    id<MTLDevice> device=MTLCreateSystemDefaultDevice();if(!device)return 77;
    NSString *dir=[NSString stringWithUTF8String:argv[1]];
    NSString *source=@"#include <metal_stdlib>\nusing namespace metal; fragment float4 main0(){return float4(1);}";
    NSData *data=[source dataUsingEncoding:NSUTF8StringEncoding];[data writeToFile:[dir stringByAppendingPathComponent:@"fixture.metal"] atomically:YES];
    NSMutableDictionary *stage=[@{@"key":@"fixture",@"stage":@"ps",@"msl_sha256":hash(data)} mutableCopy];
    NSMutableDictionary *manifest=[@{@"runtime_eligible":@NO,@"stages":@[stage]} mutableCopy];NSString *path=[dir stringByAppendingPathComponent:@"sources.json"];
    [[NSJSONSerialization dataWithJSONObject:manifest options:0 error:nil] writeToFile:path atomically:YES];
    NSError *error=nil;NSDictionary *result=wonder_metal3_check_sources(device,dir,&error);assert(result&&[result[@"compiled"] intValue]==1&&[result[@"failed"] intValue]==0);
    stage[@"msl_sha256"]=@"invalid";[[NSJSONSerialization dataWithJSONObject:manifest options:0 error:nil] writeToFile:path atomically:YES];
    result=wonder_metal3_check_sources(device,dir,&error);assert(result&&[result[@"compiled"] intValue]==0&&[result[@"failed"] intValue]==1);
    stage[@"key"]=@"../escape";[[NSJSONSerialization dataWithJSONObject:manifest options:0 error:nil] writeToFile:path atomically:YES];
    assert(!wonder_metal3_check_sources(device,dir,&error));puts("METAL3_SOURCE_HASH_AND_COMPILE_CHECK_OK");return 0;
}}
