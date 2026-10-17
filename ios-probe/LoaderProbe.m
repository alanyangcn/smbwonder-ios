// 真机原生启动诊断：签名模块、SDK、构造函数、nnMain；按首个未实现接口停止。
// Native boot diagnostic: signed modules, SDK, constructors and nnMain; stop at the first missing interface.
#import <UIKit/UIKit.h>
#import <Foundation/Foundation.h>
#import <Metal/Metal.h>
#include "dm.h"
#include "../runtime/romfs.h"
#import "../runtime/metal3_probe.h"
#import "../runtime/host_input.h"
#import "../runtime/metal3_shader_check.h"
#import "../runtime/host_graphics.h"
#include "../runtime/sdk_services.h"
#include "../runtime/gpu_gate.h"
#include "../runtime/widescreen.h"
#if !defined(__aarch64__)
#error This probe requires a physical ARM64 iOS device, not an Intel simulator.
#endif
#include <time.h>
#include <unistd.h>
#include <sys/sysctl.h>
#include <sys/proc.h>
#include <fcntl.h>
#include <errno.h>

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

// stderr goes to Documents/loader-probe.log and, live, to the original stderr (devicectl --console streams it to the
// Mac: copying the growing file off the device fails while the game runs). The console copy never blocks the game:
// when the console is slow or absent, its lines are dropped; the file gets every line.
static int log_file = -1, log_console = -1;
static void *log_pump(void *arg) {
    int in = (int)(intptr_t)arg; char buffer[16384];
    for (;;) {
        ssize_t n = read(in, buffer, sizeof buffer); if (n <= 0) { if (n < 0 && errno == EINTR) continue; break; }
        for (ssize_t done = 0; done < n;) { ssize_t w = write(log_file, buffer + done, (size_t)(n - done)); if (w <= 0) { if (errno == EINTR) continue; break; } done += w; }
        if (log_console >= 0) (void)!write(log_console, buffer, (size_t)n);
    }
    return NULL;
}
static int start_log(const char *path) {
    log_file = open(path, O_WRONLY | O_CREAT | O_TRUNC, 0644); if (log_file < 0) return -1;
    log_console = dup(STDERR_FILENO); if (log_console >= 0) fcntl(log_console, F_SETFL, fcntl(log_console, F_GETFL) | O_NONBLOCK);
    int p[2]; if (pipe(p)) return -1;
    if (dup2(p[1], STDERR_FILENO) < 0) return -1; close(p[1]);
    setvbuf(stderr, NULL, _IOLBF, 0);
    pthread_t t; if (pthread_create(&t, NULL, log_pump, (void *)(intptr_t)p[0])) return -1; pthread_detach(t);
    return 0;
}
static void show_log(BOOL shown) { dispatch_async(dispatch_get_main_queue(), ^{ gText.hidden = !shown; }); }
static void guest_stopped(const char *reason) {
    show_log(YES);
    if(strstr(reason,"GUEST_FAULT")) {
        report([NSString stringWithFormat:@"GUEST_FAULT: %s\n游戏代码访问异常；客体已停止，日志含模块偏移与调用链。",reason]);
        return;
    }
    if(strstr(reason,"NVN_UNIMPLEMENTED")) {
        report([NSString stringWithFormat:@"NVN_GAP: %s\n已推进到首个未实现的 NVN 调用；客体已停止，未返回伪成功。",reason]);
        return;
    }
    if(strstr(reason,"GRAPHICS_DEVICE_UNSUPPORTED")) {
        report([NSString stringWithFormat:@"%s\n此设备的 GPU 不满足渲染器要求（Metal 3 家族 + BC 贴图采样）。",reason]);
        return;
    }
    report([NSString stringWithFormat:@"BOOT_BLOCKED: %s\n客体已停止；此界面保留日志，未跳过缺失接口。",reason]);
}

int wonder_metal3_selftest(void) {
    NSString *documents=NSSearchPathForDirectoriesInDomains(NSDocumentDirectory,NSUserDomainMask,YES).firstObject;
    NSError *error=nil;
    NSDictionary *result=wonder_metal3_run(MTLCreateSystemDefaultDevice(),documents,&error);
    if(!result) {
        NSDictionary *failure=@{@"scope":@"host GPU diagnostic; not game frame",@"passed":@NO,@"error":error.description?:@"unknown"};
        [[NSJSONSerialization dataWithJSONObject:failure options:NSJSONWritingPrettyPrinted error:nil] writeToFile:[documents stringByAppendingPathComponent:@"metal3-probe.json"] atomically:YES];
        report([NSString stringWithFormat:@"METAL3_PROBE_FAILED: %@",error.description]);return 1;
    }
    report(@"METAL3_GPU_PROBE_OK: 4096 像素正确；错误参数负对照已拒绝。此为 GPU 诊断图，并非游戏画面。");
    NSString *sourceDirectory=[documents stringByAppendingPathComponent:@"Metal3ShaderSources"];
    if([NSFileManager.defaultManager fileExistsAtPath:[sourceDirectory stringByAppendingPathComponent:@"sources.json"]]) {
        NSError *sourceError=nil;NSDictionary *checks=wonder_metal3_check_sources(MTLCreateSystemDefaultDevice(),sourceDirectory,&sourceError);
        if(checks)report([NSString stringWithFormat:@"TARGET_SHADER_COMPILE: %@/%@；仅编译检查，实际绘制与反射待验证。",checks[@"compiled"],checks[@"total"]]);
        else report([NSString stringWithFormat:@"TARGET_SHADER_CHECK_FAILED: %@",sourceError.description]);
    } else report(@"TARGET_SHADER_CHECK_PENDING: 尚未提供目标 MSL 输入。");
    return 0;
}

int wonder_host_graphics_probe(void) {
    id<MTLDevice> device=MTLCreateSystemDefaultDevice();
    if(!device){report(@"GPU_CAPABILITY Metal device unavailable");return -1;}
    BOOL metal3=[device supportsFamily:MTLGPUFamilyMetal3];
    BOOL bc=device.supportsBCTextureCompression;
    report([NSString stringWithFormat:@"GPU_CAPABILITY name=%@ Metal3=%d Apple7=%d argumentBuffersTier=%lu BC=%d RAM=%llu OS=%@",device.name,metal3,[device supportsFamily:MTLGPUFamilyApple7],(unsigned long)device.argumentBuffersSupport,bc,NSProcessInfo.processInfo.physicalMemory,UIDevice.currentDevice.systemVersion]);
    if(metal3&&bc)show_log(NO);   // the game draws from here on; the log overlay returns if the guest stops
    return metal3&&bc?1:0;   // the renderer needs both (graphics_gate.c)
}

extern uint64_t wonder_language_code(const char *);
uint64_t wonder_host_language(void) {
    return wonder_language_code(NSLocale.preferredLanguages.firstObject.UTF8String);
}

// iOS 27 requires the UIScene lifecycle (an app without a scene manifest is stopped in
// _UIApplicationEvaluateRuntimeIssueForNoSceneLifecycleAdoption, round111). One window scene; the probe UI, the log and
// the game thread start when it first connects, so the Metal surface exists before nv::InitializeGraphics.
@interface ProbeDelegate : UIResponder <UIApplicationDelegate>
@end
@implementation ProbeDelegate
- (BOOL)application:(UIApplication *)application didFinishLaunchingWithOptions:(NSDictionary *)options { (void)application; (void)options; return YES; }
- (UISceneConfiguration *)application:(UIApplication *)application configurationForConnectingSceneSession:(UISceneSession *)session options:(UISceneConnectionOptions *)options {
    (void)application; (void)options;
    return [[UISceneConfiguration alloc] initWithName:@"Default" sessionRole:session.role];
}
@end

// Landscape only, no status bar, home indicator hidden and edge gestures deferred (the controls sit at the edges).
@interface ProbeGameController : UIViewController
@end
@implementation ProbeGameController
- (BOOL)prefersStatusBarHidden { return YES; }
- (BOOL)prefersHomeIndicatorAutoHidden { return YES; }
- (UIRectEdge)preferredScreenEdgesDeferringSystemGestures { return UIRectEdgeAll; }
- (UIInterfaceOrientationMask)supportedInterfaceOrientations { return UIInterfaceOrientationMaskLandscape; }
@end

@interface ProbeSceneDelegate : UIResponder <UIWindowSceneDelegate>
@property(nonatomic, strong) UIWindow *window;
@end

@implementation ProbeSceneDelegate
// Lifecycle is logged so a round can tell whether iOS backgrounded the probe.
// GPU submissions wait from resign to active (gpu_gate.h): iOS refuses GPU work once the app is in the background.
- (void)sceneDidBecomeActive:(UIScene *)scene { (void)scene;fprintf(stderr,"HOST_LIFECYCLE active t=%.3f\n",now_ns()/1e9);wonder_applet_focus(1);wonder_gpu_gate_set(1); }
- (void)sceneWillResignActive:(UIScene *)scene { (void)scene;fprintf(stderr,"HOST_LIFECYCLE resign t=%.3f\n",now_ns()/1e9);wonder_gpu_gate_set(0);wonder_applet_focus(2); }
- (void)sceneDidEnterBackground:(UIScene *)scene { (void)scene;fprintf(stderr,"HOST_LIFECYCLE background t=%.3f\n",now_ns()/1e9);wonder_applet_focus(3); }
- (void)scene:(UIScene *)scene willConnectToSession:(UISceneSession *)session options:(UISceneConnectionOptions *)options {
    (void)session; (void)options;
    if (![scene isKindOfClass:UIWindowScene.class] || self.window) return;
    static int started; if (started) return; started = 1;   // one game per process
    self.window = [[UIWindow alloc] initWithWindowScene:(UIWindowScene *)scene];
    {   // Widescreen (runtime/widescreen.h): the game frames the screen's aspect; WONDER_WIDESCREEN=0 keeps 16:9 with bars.
        const char *e = getenv("WONDER_WIDESCREEN"); CGRect b = ((UIWindowScene *)scene).screen.bounds;
        wonder_widescreen_set(e && *e == '0' ? 16.0f / 9.0f : wonder_widescreen_aspect(b.size.width, b.size.height));
    }
    ProbeGameController *controller = [ProbeGameController new];
    controller.view.backgroundColor = UIColor.blackColor;
    gText = [[UITextView alloc] initWithFrame:CGRectZero];
    gText.editable = NO; gText.userInteractionEnabled = NO;
    gText.font = [UIFont monospacedSystemFontOfSize:10 weight:UIFontWeightRegular];
    gText.textColor = UIColor.whiteColor; gText.backgroundColor = [UIColor colorWithWhite:0 alpha:0.6];
    gText.text = @"Wonder Native Loader Probe\n";
    gText.translatesAutoresizingMaskIntoConstraints = NO;
    // Persistent CAMetalLayer that VI exposes as the native window: 16:9, as tall as the screen allows, centred
    // (the 19.5:9 iPhone gets bars at the sides). Black until NVN presents.
    UIView *surface = wonder_graphics_surface();
    surface.translatesAutoresizingMaskIntoConstraints = NO;
    // Floating touch controls over the whole screen (GameController pads feed nn::hid too). The probe log is an
    // overlay shown while booting and again when the guest stops.
    UIView *controls = wonder_input_controls();
    controls.translatesAutoresizingMaskIntoConstraints = NO;
    UIView *root = controller.view;
    [root addSubview:surface]; [root addSubview:controls]; [root addSubview:gText];
    NSLayoutConstraint *fillHeight = [surface.heightAnchor constraintEqualToAnchor:root.heightAnchor]; fillHeight.priority = UILayoutPriorityDefaultHigh;
    [NSLayoutConstraint activateConstraints:@[
        [surface.centerXAnchor constraintEqualToAnchor:root.centerXAnchor],
        [surface.centerYAnchor constraintEqualToAnchor:root.centerYAnchor],
        [surface.widthAnchor constraintEqualToAnchor:surface.heightAnchor multiplier:16.0/9.0],
        [surface.widthAnchor constraintLessThanOrEqualToAnchor:root.widthAnchor],
        [surface.heightAnchor constraintLessThanOrEqualToAnchor:root.heightAnchor], fillHeight,
        [controls.leadingAnchor constraintEqualToAnchor:root.leadingAnchor],
        [controls.trailingAnchor constraintEqualToAnchor:root.trailingAnchor],
        [controls.topAnchor constraintEqualToAnchor:root.topAnchor],
        [controls.bottomAnchor constraintEqualToAnchor:root.bottomAnchor],
        [gText.leadingAnchor constraintEqualToAnchor:root.safeAreaLayoutGuide.leadingAnchor constant:8],
        [gText.trailingAnchor constraintEqualToAnchor:root.safeAreaLayoutGuide.trailingAnchor constant:-8],
        [gText.topAnchor constraintEqualToAnchor:root.topAnchor constant:8],
        [gText.heightAnchor constraintEqualToAnchor:root.heightAnchor multiplier:0.45]
    ]];
    self.window.rootViewController = controller;
    [self.window makeKeyAndVisible];
    NSString *documents = NSSearchPathForDirectoriesInDomains(NSDocumentDirectory, NSUserDomainMask, YES).firstObject;
    NSString *log = [documents stringByAppendingPathComponent:@"loader-probe.log"];
    NSString *previous=[documents stringByAppendingPathComponent:@"loader-probe.previous.log"];
    [NSFileManager.defaultManager removeItemAtPath:previous error:nil];
    if([NSFileManager.defaultManager fileExistsAtPath:log])[NSFileManager.defaultManager moveItemAtPath:log toPath:previous error:nil];
    if (start_log(log.fileSystemRepresentation)) return;
    report([NSString stringWithFormat:@"PROBE_RUN id=%@ pid=%d debugger=%d stage=%@",NSUUID.UUID.UUIDString,getpid(),debugger_attached(),[NSBundle.mainBundle objectForInfoDictionaryKey:@"WonderBootStage"]]);
    for(NSString *k in [NSProcessInfo.processInfo.environment.allKeys sortedArrayUsingSelector:@selector(compare:)])
        if([k hasPrefix:@"WONDER_"]) report([NSString stringWithFormat:@"PROBE_ENV %@=%@",k,NSProcessInfo.processInfo.environment[k]]);
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
}
@end

// Documents/launch-env.txt (written by tools/device_round.sh from WONDER_ENVS): KEY=VALUE lines applied when the app is
// opened from the home screen too, so it runs with the round's settings (round157: a home-screen launch lacked
// WONDER_RT=0 and the census sweep). Variables already set by the launcher win.
static void apply_launch_env(void) {
    NSString *path = [NSHomeDirectory() stringByAppendingPathComponent:@"Documents/launch-env.txt"];
    NSString *text = [NSString stringWithContentsOfFile:path encoding:NSUTF8StringEncoding error:nil]; if (!text) return;
    for (NSString *line in [text componentsSeparatedByCharactersInSet:NSCharacterSet.whitespaceAndNewlineCharacterSet]) {
        NSRange eq = [line rangeOfString:@"="]; if (!line.length || eq.location == NSNotFound || !eq.location) continue;
        NSString *key = [line substringToIndex:eq.location], *value = [line substringFromIndex:eq.location + 1];
        if (![key hasPrefix:@"WONDER_"]) continue;
        if (setenv(key.UTF8String, value.UTF8String, 0) == 0) fprintf(stderr, "HOST_LAUNCH_ENV %s=%s%s\n", key.UTF8String, getenv(key.UTF8String), strcmp(getenv(key.UTF8String), value.UTF8String) ? " (launcher value kept)" : "");
    }
}
int main(int argc, char **argv) {
    @autoreleasepool { apply_launch_env(); return UIApplicationMain(argc, argv, nil, NSStringFromClass(ProbeDelegate.class)); }
}
