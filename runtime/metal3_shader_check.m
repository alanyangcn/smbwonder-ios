#import "metal3_shader_check.h"
#import <CommonCrypto/CommonDigest.h>
static NSString *digest(NSData *data){
    unsigned char hash[CC_SHA256_DIGEST_LENGTH];CC_SHA256(data.bytes,(CC_LONG)data.length,hash);
    NSMutableString *text=[NSMutableString new];for(unsigned i=0;i<sizeof hash;i++)[text appendFormat:@"%02x",hash[i]];return text;
}
static NSDictionary *invalid(NSError **error,NSString *message){if(error)*error=[NSError errorWithDomain:@"WonderShaderSources" code:1 userInfo:@{NSLocalizedDescriptionKey:message}];return nil;}
NSDictionary *wonder_metal3_check_sources(id<MTLDevice> device,NSString *directory,NSError **error){
    if(!device)return invalid(error,@"No Metal device");
    NSData *data=[NSData dataWithContentsOfFile:[directory stringByAppendingPathComponent:@"sources.json"]];
    if(!data||data.length>(1u<<20))return invalid(error,@"Missing or oversized source manifest");
    id manifest=[NSJSONSerialization JSONObjectWithData:data options:0 error:error];
    if(![manifest isKindOfClass:NSDictionary.class]||![manifest[@"runtime_eligible"] isKindOfClass:NSNumber.class]||[manifest[@"runtime_eligible"] boolValue])return invalid(error,@"Manifest must explicitly exclude runtime qualification");
    NSArray *stages=manifest[@"stages"];
    if(![stages isKindOfClass:NSArray.class]||!stages.count||stages.count>128)return invalid(error,@"Invalid shader stage list");
    NSCharacterSet *invalidKey=[[NSCharacterSet characterSetWithCharactersInString:@"abcdefghijklmnopqrstuvwxyzABCDEFGHIJKLMNOPQRSTUVWXYZ0123456789_-"] invertedSet];
    NSMutableSet *keys=[NSMutableSet new];
    for(id stage in stages){
        if(![stage isKindOfClass:NSDictionary.class])return invalid(error,@"Invalid stage record");
        NSString *key=stage[@"key"],*kind=stage[@"stage"],*hash=stage[@"msl_sha256"];
        if(![key isKindOfClass:NSString.class]||!key.length||key.length>80||[key rangeOfCharacterFromSet:invalidKey].location!=NSNotFound||[keys containsObject:key])return invalid(error,@"Invalid or duplicate shader key");
        if(![kind isKindOfClass:NSString.class]||(![kind isEqual:@"vs"]&&![kind isEqual:@"ps"])||![hash isKindOfClass:NSString.class])return invalid(error,@"Unsupported stage or source hash");
        [keys addObject:key];
    }
    NSMutableArray *records=[NSMutableArray new];unsigned compiled=0;
    MTLCompileOptions *options=[MTLCompileOptions new];options.languageVersion=MTLLanguageVersion3_2;
    for(NSDictionary *stage in stages){
        NSString *key=stage[@"key"],*path=[directory stringByAppendingPathComponent:[key stringByAppendingString:@".metal"]];
        NSDictionary *attributes=[NSFileManager.defaultManager attributesOfItemAtPath:path error:nil];
        NSData *sourceData=nil;
        if([attributes[NSFileType] isEqual:NSFileTypeRegular]&&[attributes fileSize]<=(16u<<20))sourceData=[NSData dataWithContentsOfFile:path];
        NSString *failure=nil;NSError *compileError=nil;id<MTLLibrary> library=nil;id<MTLFunction> function=nil;
        if(!sourceData||![digest(sourceData) isEqual:stage[@"msl_sha256"]])failure=@"Source missing, oversized or SHA-256 mismatch";
        else {
            NSString *source=[[NSString alloc] initWithData:sourceData encoding:NSUTF8StringEncoding];
            if(!source)failure=@"Invalid UTF-8 MSL source";
            else library=[device newLibraryWithSource:source options:options error:&compileError];
            if(library)function=[library newFunctionWithName:@"main0"];
            MTLFunctionType type=[stage[@"stage"] isEqual:@"vs"]?MTLFunctionTypeVertex:MTLFunctionTypeFragment;
            if(!failure&&(!function||function.functionType!=type))failure=compileError.description?:@"Missing or mismatched main0 stage";
        }
        NSMutableDictionary *record=[stage mutableCopy];record[@"compiled"]=@(!failure);
        if(failure)record[@"error"]=failure;else compiled++;
        [records addObject:record];fprintf(stderr,"[metal3] TARGET_SHADER %s stage=%s result=%s\n",key.UTF8String,[stage[@"stage"] UTF8String],failure?"FAILED":"COMPILED");
    }
    NSDictionary *result=@{@"scope":@"original-game source compilation only; no pipeline/draw/reflection validation",@"device":device.name,@"msl":@"3.2",@"compiled":@(compiled),@"failed":@(stages.count-compiled),@"total":@(stages.count),@"runtime_eligible":@NO,@"reflection_validated":@NO,@"stages":records};
    NSData *json=[NSJSONSerialization dataWithJSONObject:result options:NSJSONWritingPrettyPrinted error:error];
    if(!json||![json writeToFile:[directory stringByAppendingPathComponent:@"device-compile-result.json"] options:NSDataWritingAtomic error:error])return nil;
    fprintf(stderr,"[metal3] TARGET_SHADER_COMPILE_RESULT compiled=%u total=%lu runtime_eligible=0\n",compiled,(unsigned long)stages.count);
    return result;
}
