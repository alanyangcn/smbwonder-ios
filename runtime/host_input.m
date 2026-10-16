// Host input front end (host_input.h). Two sources, merged on the main thread and published with wonder_hid_publish:
// - on-screen touch controls floating over the landscape game: ZL/L and R/ZR in the top corners, -/+ top centre, left
//   stick above the D-pad on the left, A/B/X/Y (Switch layout) on the right; translucent, light haptic on press;
// - GameController pads (Xbox / PlayStation / MFi), mapped by position: south = B, east = A, west = Y, north = X,
//   as the Switch's own layout (a Nintendo-layout pad therefore keeps its printed labels).
// Buttons are OR-ed; a pad stick outside its dead zone wins over the touch stick.
#import "host_input.h"
#import <GameController/GameController.h>
#include "hid_state.h"
#include <math.h>
#include <stdio.h>

static uint64_t touch_buttons, pad_buttons;
static int32_t touch_lx, touch_ly, pad_lx, pad_ly, pad_rx, pad_ry;
static uint64_t last_buttons = ~0ull;
static void publish(void) {
    uint64_t buttons = touch_buttons | pad_buttons;
    int pad_stick = pad_lx || pad_ly;
    int32_t lx = pad_stick ? pad_lx : touch_lx, ly = pad_stick ? pad_ly : touch_ly;
    wonder_hid_publish(buttons, lx, ly, pad_rx, pad_ry);
    if (buttons != last_buttons) {   // button edges only (stick motion is too frequent to log)
        static unsigned logged; if (logged++ < 400) fprintf(stderr, "HOST_INPUT buttons=%#llx stick=%d,%d\n", (unsigned long long)buttons, lx, ly);
        last_buttons = buttons;
    }
}

// ---------- Touch controls ----------
@interface WonderPadButton : UILabel
@property(nonatomic) uint64_t mask;
@end
@implementation WonderPadButton
@end

#define IDLE [UIColor colorWithWhite:0.1 alpha:0.28]
#define HELD [UIColor colorWithWhite:0.9 alpha:0.45]
@interface WonderControlsView : UIView
@end
@implementation WonderControlsView {
    NSMutableArray<WonderPadButton *> *buttons;
    UIView *stickBase, *stickKnob;
    UITouch *stickTouch;
    UIImpactFeedbackGenerator *haptic;
}
- (WonderPadButton *)button:(NSString *)title mask:(uint64_t)mask round:(BOOL)round {
    WonderPadButton *b = [WonderPadButton new];
    b.text = title; b.mask = mask; b.textAlignment = NSTextAlignmentCenter; b.textColor = UIColor.whiteColor;
    b.font = [UIFont systemFontOfSize:18 weight:UIFontWeightSemibold];
    b.backgroundColor = IDLE; b.layer.masksToBounds = YES; b.layer.borderWidth = 1; b.layer.borderColor = [UIColor colorWithWhite:1 alpha:0.35].CGColor;
    b.textColor = [UIColor colorWithWhite:1 alpha:0.8];
    b.tag = round; [self addSubview:b]; [buttons addObject:b];
    return b;
}
- (instancetype)initWithFrame:(CGRect)frame {
    if (!(self = [super initWithFrame:frame])) return nil;
    self.multipleTouchEnabled = YES; self.backgroundColor = UIColor.clearColor;
    haptic = [[UIImpactFeedbackGenerator alloc] initWithStyle:UIImpactFeedbackStyleLight];
    buttons = [NSMutableArray array];
    stickBase = [UIView new]; stickBase.backgroundColor = IDLE; stickBase.layer.borderWidth = 1; stickBase.layer.borderColor = [UIColor colorWithWhite:1 alpha:0.35].CGColor; stickBase.userInteractionEnabled = NO; [self addSubview:stickBase];
    stickKnob = [UIView new]; stickKnob.backgroundColor = [UIColor colorWithWhite:0.85 alpha:0.4]; stickKnob.userInteractionEnabled = NO; [self addSubview:stickKnob];
    [self button:@"ZL" mask:WONDER_BTN_ZL round:NO]; [self button:@"L" mask:WONDER_BTN_L round:NO];
    [self button:@"−" mask:WONDER_BTN_MINUS round:YES]; [self button:@"+" mask:WONDER_BTN_PLUS round:YES];
    [self button:@"R" mask:WONDER_BTN_R round:NO]; [self button:@"ZR" mask:WONDER_BTN_ZR round:NO];
    [self button:@"▲" mask:WONDER_BTN_UP round:NO]; [self button:@"◀" mask:WONDER_BTN_LEFT round:NO];
    [self button:@"▶" mask:WONDER_BTN_RIGHT round:NO]; [self button:@"▼" mask:WONDER_BTN_DOWN round:NO];
    [self button:@"X" mask:WONDER_BTN_X round:YES]; [self button:@"Y" mask:WONDER_BTN_Y round:YES];
    [self button:@"A" mask:WONDER_BTN_A round:YES]; [self button:@"B" mask:WONDER_BTN_B round:YES];
    return self;
}
- (WonderPadButton *)buttonFor:(uint64_t)mask { for (WonderPadButton *b in buttons) if (b.mask == mask) return b; return nil; }
- (void)layoutSubviews {
    [super layoutSubviews];
    // Sized from the short side (landscape height); buttons stay inside the safe area (notch / home indicator).
    UIEdgeInsets inset = self.safeAreaInsets;
    CGFloat w = self.bounds.size.width, h = self.bounds.size.height, m = 10, gap = 8;
    CGFloat left = inset.left + m, right = w - inset.right - m, top = inset.top + m, bottom = h - inset.bottom - m;
    CGFloat b = fmin(h / 6.0, 72);
    CGFloat sw = b * 1.15, sh = b * 0.62;   // shoulders
    [self buttonFor:WONDER_BTN_ZL].frame = CGRectMake(left, top, sw, sh);
    [self buttonFor:WONDER_BTN_L].frame = CGRectMake(left, top + sh + gap, sw, sh);
    [self buttonFor:WONDER_BTN_ZR].frame = CGRectMake(right - sw, top, sw, sh);
    [self buttonFor:WONDER_BTN_R].frame = CGRectMake(right - sw, top + sh + gap, sw, sh);
    CGFloat small = sh;
    [self buttonFor:WONDER_BTN_MINUS].frame = CGRectMake(w / 2 - gap - small, top, small, small);
    [self buttonFor:WONDER_BTN_PLUS].frame = CGRectMake(w / 2 + gap, top, small, small);
    // Left: stick (middle height) and D-pad (bottom, further in). Right: face buttons at the bottom.
    CGFloat radius = b * 0.95, d = b * 0.78, f = b * 0.9;
    CGPoint stick = CGPointMake(left + sw / 2 + radius * 0.6, top + 2 * sh + gap * 3 + radius);
    stickBase.frame = CGRectMake(stick.x - radius, stick.y - radius, 2 * radius, 2 * radius); stickBase.layer.cornerRadius = radius;
    CGFloat knob = radius * 0.9; stickKnob.bounds = CGRectMake(0, 0, knob, knob); stickKnob.layer.cornerRadius = knob / 2;
    if (!stickTouch) stickKnob.center = stickBase.center;
    CGPoint dpad = CGPointMake(stick.x + radius + 1.2 * d, bottom - 1.5 * d);
    [self buttonFor:WONDER_BTN_UP].frame = CGRectMake(dpad.x - d / 2, dpad.y - 1.5 * d, d, d);
    [self buttonFor:WONDER_BTN_DOWN].frame = CGRectMake(dpad.x - d / 2, dpad.y + 0.5 * d, d, d);
    [self buttonFor:WONDER_BTN_LEFT].frame = CGRectMake(dpad.x - 1.5 * d, dpad.y - d / 2, d, d);
    [self buttonFor:WONDER_BTN_RIGHT].frame = CGRectMake(dpad.x + 0.5 * d, dpad.y - d / 2, d, d);
    CGPoint face = CGPointMake(right - 1.5 * f, bottom - 1.5 * f);
    [self buttonFor:WONDER_BTN_X].frame = CGRectMake(face.x - f / 2, face.y - 1.5 * f, f, f);
    [self buttonFor:WONDER_BTN_B].frame = CGRectMake(face.x - f / 2, face.y + 0.5 * f, f, f);
    [self buttonFor:WONDER_BTN_Y].frame = CGRectMake(face.x - 1.5 * f, face.y - f / 2, f, f);
    [self buttonFor:WONDER_BTN_A].frame = CGRectMake(face.x + 0.5 * f, face.y - f / 2, f, f);
    for (WonderPadButton *button in buttons) button.layer.cornerRadius = button.tag ? button.bounds.size.width / 2 : 10;
}
- (void)update:(UIEvent *)event {
    uint64_t pressed = 0; int stickAlive = 0;
    for (UITouch *t in [event touchesForView:self]) {
        if (t.phase == UITouchPhaseEnded || t.phase == UITouchPhaseCancelled) { if (t == stickTouch) stickTouch = nil; continue; }
        CGPoint p = [t locationInView:self];
        if (!stickTouch && t.phase == UITouchPhaseBegan && CGRectContainsPoint(CGRectInset(stickBase.frame, -20, -20), p)) stickTouch = t;
        if (t == stickTouch) {
            stickAlive = 1;
            CGFloat r = stickBase.bounds.size.width / 2, dx = p.x - stickBase.center.x, dy = p.y - stickBase.center.y, len = hypot(dx, dy);
            if (len > r) { dx *= r / len; dy *= r / len; }
            stickKnob.center = CGPointMake(stickBase.center.x + dx, stickBase.center.y + dy);
            touch_lx = (int32_t)lround(dx / r * WONDER_STICK_MAX); touch_ly = (int32_t)lround(-dy / r * WONDER_STICK_MAX);   // Switch: up positive
            continue;
        }
        for (WonderPadButton *b in buttons) if (CGRectContainsPoint(CGRectInset(b.frame, -6, -6), p)) pressed |= b.mask;
    }
    if (!stickAlive) { stickTouch = nil; touch_lx = touch_ly = 0; stickKnob.center = stickBase.center; }
    for (WonderPadButton *b in buttons) b.backgroundColor = (pressed & b.mask) ? HELD : IDLE;
    if (pressed & ~touch_buttons) [haptic impactOccurred];   // a newly pressed button
    touch_buttons = pressed; publish();
}
- (void)touchesBegan:(NSSet<UITouch *> *)touches withEvent:(UIEvent *)event { (void)touches; [self update:event]; }
- (void)touchesMoved:(NSSet<UITouch *> *)touches withEvent:(UIEvent *)event { (void)touches; [self update:event]; }
- (void)touchesEnded:(NSSet<UITouch *> *)touches withEvent:(UIEvent *)event { (void)touches; [self update:event]; }
- (void)touchesCancelled:(NSSet<UITouch *> *)touches withEvent:(UIEvent *)event { (void)touches; [self update:event]; }
@end

// ---------- GameController pads ----------
static int32_t axis(float v, float dead) { return fabsf(v) < dead ? 0 : (int32_t)lroundf(fmaxf(-1.f, fminf(1.f, v)) * WONDER_STICK_MAX); }
static void read_pad(GCExtendedGamepad *g) {
    uint64_t b = 0;
    if (g.buttonA.pressed) b |= WONDER_BTN_B;   // south
    if (g.buttonB.pressed) b |= WONDER_BTN_A;   // east
    if (g.buttonX.pressed) b |= WONDER_BTN_Y;   // west
    if (g.buttonY.pressed) b |= WONDER_BTN_X;   // north
    if (g.leftShoulder.pressed) b |= WONDER_BTN_L; if (g.rightShoulder.pressed) b |= WONDER_BTN_R;
    if (g.leftTrigger.pressed) b |= WONDER_BTN_ZL; if (g.rightTrigger.pressed) b |= WONDER_BTN_ZR;
    if (g.buttonMenu.pressed) b |= WONDER_BTN_PLUS; if (g.buttonOptions.pressed) b |= WONDER_BTN_MINUS;
    if (g.leftThumbstickButton.pressed) b |= WONDER_BTN_STICK_L; if (g.rightThumbstickButton.pressed) b |= WONDER_BTN_STICK_R;
    if (g.dpad.up.pressed) b |= WONDER_BTN_UP; if (g.dpad.down.pressed) b |= WONDER_BTN_DOWN;
    if (g.dpad.left.pressed) b |= WONDER_BTN_LEFT; if (g.dpad.right.pressed) b |= WONDER_BTN_RIGHT;
    pad_buttons = b;
    pad_lx = axis(g.leftThumbstick.xAxis.value, 0.12f); pad_ly = axis(g.leftThumbstick.yAxis.value, 0.12f);   // GC: up positive
    pad_rx = axis(g.rightThumbstick.xAxis.value, 0.12f); pad_ry = axis(g.rightThumbstick.yAxis.value, 0.12f);
    publish();
}
static void attach(GCController *c) {
    GCExtendedGamepad *g = c.extendedGamepad;
    fprintf(stderr, "HOST_INPUT_PAD connected vendor=%s extended=%d\n", c.vendorName.UTF8String ?: "?", g != nil);
    if (!g) return;   // micro gamepads (Siri Remote) are not mapped
    g.valueChangedHandler = ^(GCExtendedGamepad *pad, GCControllerElement *element) { (void)element; read_pad(pad); };
    read_pad(g);
}
static void start_pads(void) {
    static int started; if (started) return; started = 1;
    GCController.shouldMonitorBackgroundEvents = NO;
    [NSNotificationCenter.defaultCenter addObserverForName:GCControllerDidConnectNotification object:nil queue:NSOperationQueue.mainQueue
                                                usingBlock:^(NSNotification *n) { attach(n.object); }];
    [NSNotificationCenter.defaultCenter addObserverForName:GCControllerDidDisconnectNotification object:nil queue:NSOperationQueue.mainQueue
                                                usingBlock:^(NSNotification *n) {
        fprintf(stderr, "HOST_INPUT_PAD disconnected vendor=%s\n", ((GCController *)n.object).vendorName.UTF8String ?: "?");
        pad_buttons = 0; pad_lx = pad_ly = pad_rx = pad_ry = 0; publish();
    }];
    for (GCController *c in GCController.controllers) attach(c);
}

UIView *wonder_input_controls(void) {
    NSCAssert(NSThread.isMainThread, @"input controls belong to the UIKit thread");
    static WonderControlsView *view; if (!view) view = [WonderControlsView new];
    start_pads();
    return view;
}
