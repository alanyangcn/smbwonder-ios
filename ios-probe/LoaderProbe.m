// 真机原生启动诊断：签名模块、SDK、构造函数、nnMain；按首个未实现接口停止。
// Native boot diagnostic: signed modules, SDK, constructors and nnMain; stop at the first missing interface.
#import <UIKit/UIKit.h>
#import <Foundation/Foundation.h>
#import <Metal/Metal.h>
#include "dm.h"
#include "../runtime/romfs.h"
#import "../runtime/metal2_probe.h"
#import "../runtime/metal2_shader_check.h"
#import "../runtime/host_graphics.h"
#include "../runtime/sdk_services.h"
#if !defined(__aarch64__)
#error This probe requires a physical ARM64 iOS device, not an Intel simulator.
#endif
#include <time.h>
#include <unistd.h>
#include <sys/sysctl.h>
#include <sys/proc.h>

struct dm_flags g_flag;
static UITextView *gText;

uint64_t now_ns(void) {
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (uint64_t)ts.tv_sec * 1000000000ull + ts.tv_nsec;
}
uint64_t guest_ns(void) { return now_ns(); }

// 诊断包关闭调用计数，不生成可执行代码；这些不是平台服务的空实现。
// Call counting is disabled in this diagnostic, generating no executable code; these are not platform-service stubs.
void *hlecount_wrap(const char *kind, const char *name, void *fn) { (void)kind; (void)name; return fn; }
void hlecount_seal(void) {}
void dm_thread_returned(void) { die("PROBE: unexpected guest thread return"); }
void wonder_sdk_startup(void);
void wonder_applet_focus(unsigned);
void wonder_game_boot(const char *stage);
int wonder_game_thread_start(pthread_t *,void *(*)(void *),void *);
static pthread_t gameThread;
void wonder_game_thread_set_reporter(void (*)(const char *));
static void *run_game_thread(void *context) {
    void (^task)(void)=(__bridge_transfer id)context;
    @autoreleasepool { task(); }
    return NULL;
}
static int debugger_attached(void) {
    struct kinfo_proc info={0};size_t size=sizeof info;
    int mib[]={CTL_KERN,KERN_PROC,KERN_PROC_PID,getpid()};
    if(sysctl(mib,4,&info,&size,NULL,0))return -1;
    return (info.kp_proc.p_flag&P_TRACED)!=0;
}

static void report(NSString *message) {
    fprintf(stderr, "%s\n", message.UTF8String);
    dispatch_async(dispatch_get_main_queue(), ^{
        gText.text = [gText.text stringByAppendingFormat:@"\n%@", message];
    });
}

static void guest_stopped(const char *reason) {
    if(strstr(reason,"GAME_RENDERER_PENDING")) {
        report([NSString stringWithFormat:@"%s\nMetal 2 已通过基础 GPU 验证；继续接入游戏 NVN 和实际着色器。",reason]);
        return;
    }
    if(strstr(reason,"GUEST_FAULT")) {
        report([NSString stringWithFormat:@"GUEST_FAULT: %s\n游戏代码访问异常；客体已停止，日志含模块偏移与调用链。",reason]);
        return;
    }
    if(strstr(reason,"NVN_UNIMPLEMENTED")) {
        report([NSString stringWithFormat:@"NVN_GAP: %s\n已推进到首个未实现的 NVN 调用；客体已停止，未返回伪成功。",reason]);
        return;
    }
    if(strstr(reason,"DEVICE_UPGRADE_REQUIRED")) {
        report([NSString stringWithFormat:@"%s\n已到当前图形路径的换机检查点：请使用支持 Metal 3 的较新设备。游戏图形接入仍待完成。",reason]);
        return;
    }
    report([NSString stringWithFormat:@"BOOT_BLOCKED: %s\n客体已停止；此界面保留日志，未跳过缺失接口。",reason]);
}

const char *wonder_graphics_backend(void) {
    NSString *backend=[NSBundle.mainBundle objectForInfoDictionaryKey:@"WonderGraphicsBackend"]?:@"metal2";
    return backend.UTF8String;
}
int wonder_metal2_selftest(void) {
    NSString *documents=NSSearchPathForDirectoriesInDomains(NSDocumentDirectory,NSUserDomainMask,YES).firstObject;
    NSError *error=nil;
    NSDictionary *result=wonder_metal2_run(MTLCreateSystemDefaultDevice(),documents,&error);
    if(!result) {
        NSDictionary *failure=@{@"scope":@"host GPU diagnostic; not game frame",@"passed":@NO,@"error":error.description?:@"unknown"};
        [[NSJSONSerialization dataWithJSONObject:failure options:NSJSONWritingPrettyPrinted error:nil] writeToFile:[documents stringByAppendingPathComponent:@"metal2-probe.json"] atomically:YES];
        report([NSString stringWithFormat:@"METAL2_PROBE_FAILED: %@",error.description]);return 1;
    }
    report(@"METAL2_GPU_PROBE_OK: 4096 像素正确；错误参数负对照已拒绝。此为 GPU 诊断图，并非游戏画面。");
    NSString *sourceDirectory=[documents stringByAppendingPathComponent:@"Metal2ShaderSources"];
    if([NSFileManager.defaultManager fileExistsAtPath:[sourceDirectory stringByAppendingPathComponent:@"sources.json"]]) {
        NSError *sourceError=nil;NSDictionary *checks=wonder_metal2_check_sources(MTLCreateSystemDefaultDevice(),sourceDirectory,&sourceError);
        if(checks)report([NSString stringWithFormat:@"TARGET_SHADER_COMPILE: %@/%@；仅编译检查，实际绘制与反射待验证。",checks[@"compiled"],checks[@"total"]]);
        else report([NSString stringWithFormat:@"TARGET_SHADER_CHECK_FAILED: %@",sourceError.description]);
    } else report(@"TARGET_SHADER_CHECK_PENDING: 尚未提供目标 MSL 输入。");
    return 0;
}

int wonder_host_graphics_probe(void) {
    id<MTLDevice> device=MTLCreateSystemDefaultDevice();
    if(!device){report(@"GPU_CAPABILITY Metal device unavailable");return -1;}
    BOOL metal3=[device supportsFamily:MTLGPUFamilyMetal3];
    BOOL bc=NO;if(@available(iOS 16.4,*))bc=device.supportsBCTextureCompression;
    report([NSString stringWithFormat:@"GPU_CAPABILITY name=%@ Metal3=%d Apple7=%d argumentBuffersTier=%lu BC=%d RAM=%llu OS=%@",device.name,metal3,[device supportsFamily:MTLGPUFamilyApple7],(unsigned long)device.argumentBuffersSupport,bc,NSProcessInfo.processInfo.physicalMemory,UIDevice.currentDevice.systemVersion]);
    return metal3?1:0;
}

extern uint64_t wonder_language_code(const char *);
uint64_t wonder_host_language(void) {
    return wonder_language_code(NSLocale.preferredLanguages.firstObject.UTF8String);
}

@interface ProbeDelegate : UIResponder <UIApplicationDelegate>
@property(nonatomic, strong) UIWindow *window;
@end

@implementation ProbeDelegate
// Lifecycle is logged so a round can tell whether iOS backgrounded the probe.
- (void)applicationDidBecomeActive:(UIApplication *)application { (void)application;fprintf(stderr,"HOST_LIFECYCLE active t=%.3f\n",now_ns()/1e9);wonder_applet_focus(1); }
- (void)applicationWillResignActive:(UIApplication *)application { (void)application;fprintf(stderr,"HOST_LIFECYCLE resign t=%.3f\n",now_ns()/1e9);wonder_applet_focus(2); }
- (void)applicationDidEnterBackground:(UIApplication *)application { (void)application;fprintf(stderr,"HOST_LIFECYCLE background t=%.3f\n",now_ns()/1e9);wonder_applet_focus(3); }
- (BOOL)application:(UIApplication *)application didFinishLaunchingWithOptions:(NSDictionary *)options {
    (void)application; (void)options;
    self.window = [[UIWindow alloc] initWithFrame:UIScreen.mainScreen.bounds];
    UIViewController *controller = [UIViewController new];
    controller.view.backgroundColor = UIColor.systemBackgroundColor;
    gText = [[UITextView alloc] initWithFrame:CGRectZero];
    gText.editable = NO;
    gText.font = [UIFont monospacedSystemFontOfSize:14 weight:UIFontWeightRegular];
    gText.text = @"Wonder Native Loader Probe\n\n原生装载诊断包：用于验证签名代码映射、重定位和原生返回。\n游戏启动接口诊断；尚未接入图形，不能游玩。\n";
    gText.translatesAutoresizingMaskIntoConstraints = NO;
    // Persistent CAMetalLayer that VI exposes as the native window; black until NVN presents.
    UIView *surface = wonder_graphics_surface();
    surface.translatesAutoresizingMaskIntoConstraints = NO;
    [controller.view addSubview:surface];
    [controller.view addSubview:gText];
    [NSLayoutConstraint activateConstraints:@[
        [surface.leadingAnchor constraintEqualToAnchor:controller.view.safeAreaLayoutGuide.leadingAnchor],
        [surface.trailingAnchor constraintEqualToAnchor:controller.view.safeAreaLayoutGuide.trailingAnchor],
        [surface.topAnchor constraintEqualToAnchor:controller.view.safeAreaLayoutGuide.topAnchor],
        [surface.heightAnchor constraintEqualToAnchor:surface.widthAnchor multiplier:9.0/16.0],
        [gText.leadingAnchor constraintEqualToAnchor:controller.view.safeAreaLayoutGuide.leadingAnchor constant:12],
        [gText.trailingAnchor constraintEqualToAnchor:controller.view.safeAreaLayoutGuide.trailingAnchor constant:-12],
        [gText.topAnchor constraintEqualToAnchor:surface.bottomAnchor constant:8],
        [gText.bottomAnchor constraintEqualToAnchor:controller.view.safeAreaLayoutGuide.bottomAnchor constant:-12]
    ]];
    self.window.rootViewController = controller;
    [self.window makeKeyAndVisible];
    NSString *documents = NSSearchPathForDirectoriesInDomains(NSDocumentDirectory, NSUserDomainMask, YES).firstObject;
    NSString *log = [documents stringByAppendingPathComponent:@"loader-probe.log"];
    NSString *previous=[documents stringByAppendingPathComponent:@"loader-probe.previous.log"];
    [NSFileManager.defaultManager removeItemAtPath:previous error:nil];
    if([NSFileManager.defaultManager fileExistsAtPath:log])[NSFileManager.defaultManager moveItemAtPath:log toPath:previous error:nil];
    if (!freopen(log.fileSystemRepresentation, "w", stderr)) return YES;
    setvbuf(stderr, NULL, _IOLBF, 0);
    report([NSString stringWithFormat:@"PROBE_RUN id=%@ pid=%d debugger=%d stage=%@",NSUUID.UUID.UUIDString,getpid(),debugger_attached(),[NSBundle.mainBundle objectForInfoDictionaryKey:@"WonderBootStage"]]);
    UIApplication.sharedApplication.idleTimerDisabled=YES;
    report(@"PROBE_START: no existing game saves or source assets will be modified");
    void (^gameTask)(void)=^{
        @autoreleasepool {
            size_t stackSize=pthread_get_stacksize_np(pthread_self());
            if(pthread_main_np()||stackSize!=0x100000)die("Incorrect game thread stack %#zx",stackSize);
            report([NSString stringWithFormat:@"GAME_THREAD_OK stack=%#zx dedicated=1",stackSize]);
            NSString *directory = [NSBundle.mainBundle.resourcePath stringByAppendingPathComponent:@"dm"];
            const char *names[] = {"main", "sdk"};
            load_modules(directory.fileSystemRepresentation, names, 2);
            report([NSString stringWithFormat:@"MAPPED_OK: %d modules; signed text + writable relocations", g_nmod]);
            // 只执行事先静态核对过的 ret；任何不同实现都拒绝调用。
            // Execute only the statically verified ret; refuse to call any different implementation.
            void *function = guest_sym("_init_libc0");
            uint32_t instruction = 0;
            if (!function) die("PROBE: _init_libc0 not found");
            memcpy(&instruction, function, sizeof instruction);
            if (instruction != 0xd65f03c0u) die("PROBE: _init_libc0 is not the expected ret instruction");
            ((void (*)(void))function)();
            report(@"GUEST_RET_OK: executed the signed, remapped ARM64 SDK ret instruction");
            report(@"SDK_START: validating target thread environment and initializing SDK");
            NSString *root=[documents stringByAppendingPathComponent:@"WonderSDK"];
            NSError *error=nil;
            if(![NSFileManager.defaultManager createDirectoryAtPath:root withIntermediateDirectories:YES attributes:nil error:&error])die("SDK root creation failed %s",error.description.UTF8String);
            if(wonder_services_configure(root.fileSystemRepresentation))die("SDK root configuration failed");
            NSString *archive=[documents stringByAppendingPathComponent:@"wonder-romfs.wrom"];
            int romError=wonder_romfs_configure(archive.fileSystemRepresentation);
            report([NSString stringWithFormat:@"ROMFS_ARCHIVE configure=%d",romError]);
            wonder_sdk_startup();
            report(@"SDK_INIT_COMPLETE");
            NSString *stage=[NSBundle.mainBundle objectForInfoDictionaryKey:@"WonderBootStage"]?:@"sdk";
            if(![stage isEqualToString:@"sdk"]) {
                report([NSString stringWithFormat:@"BOOT_STAGE %@",stage]);
                wonder_game_boot(stage.UTF8String);
                report([stage isEqualToString:@"constructors"]?@"CONSTRUCTORS_COMPLETE":@"NNMAIN_RETURNED");
            }
        }
    };
    wonder_game_thread_set_reporter(guest_stopped);
    extern void wonder_crash_install(void); wonder_crash_install();
    int error=wonder_game_thread_start(&gameThread,run_game_thread,(__bridge_retained void *)gameTask);
    if(error)die("Game main thread creation failed %d",error);
    return YES;
}
@end

int main(int argc, char **argv) {
    @autoreleasepool { return UIApplicationMain(argc, argv, nil, NSStringFromClass(ProbeDelegate.class)); }
}
