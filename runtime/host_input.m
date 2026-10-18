// Host input front end (host_input.h). Two sources, merged on the main thread and published with wonder_hid_publish:
// - on-screen touch controls floating over the landscape game: ZL/L and R/ZR in the top corners, -/+ top centre, left
//   floating stick on the left (shown in the bottom-left corner; a touch anywhere in the lower left that is not on a
//   button puts the stick's centre there, as zelda-la-android/android/touchpad.c does) (no touch D-pad: Wonder accepts the stick wherever it accepts the D-pad), A/B/X/Y (Switch
//   layout) on the right; dark glass look (gradient fill, light rim, soft shadow), pressed buttons brighten and shrink,
//   a firm haptic on each newly pressed button;
// - GameController pads (Xbox / PlayStation / MFi), mapped by position: south = B, east = A, west = Y, north = X,
//   as the Switch's own layout (a Nintendo-layout pad therefore keeps its printed labels).
// Buttons are OR-ed; a pad stick outside its dead zone wins over the touch stick.
#import "host_input.h"
#import <GameController/GameController.h>
#include "hid_state.h"
#include "input_script.h"
#include <math.h>
#include <stdio.h>

static uint64_t touch_buttons, pad_buttons, script_buttons;
static int32_t touch_lx, touch_ly, pad_lx, pad_ly, pad_rx, pad_ry;
static uint64_t last_buttons = ~0ull;
static void publish(void) {
    uint64_t buttons = touch_buttons | pad_buttons | script_buttons;
    int pad_stick = pad_lx || pad_ly;
    int32_t lx = pad_stick ? pad_lx : touch_lx, ly = pad_stick ? pad_ly : touch_ly;
    wonder_hid_publish(buttons, lx, ly, pad_rx, pad_ry);
    if (buttons != last_buttons) {   // button edges only (stick motion is too frequent to log)
        static unsigned logged; if (logged++ < 400) fprintf(stderr, "HOST_INPUT buttons=%#llx stick=%d,%d\n", (unsigned long long)buttons, lx, ly);
        last_buttons = buttons;
    }
}

// ---------- Touch controls ----------
// Shared look: a vertical gradient (lighter top) inside a light rim, with a soft drop shadow. Shapes are circles or
// capsules, so the corner radius follows the size.
static NSArray *glass(BOOL lit) {
    return lit ? @[(id)[UIColor colorWithWhite:1 alpha:0.62].CGColor, (id)[UIColor colorWithWhite:0.78 alpha:0.48].CGColor]
               : @[(id)[UIColor colorWithWhite:0.42 alpha:0.42].CGColor, (id)[UIColor colorWithWhite:0.04 alpha:0.42].CGColor];
}
static CAGradientLayer *glass_layer(void) {
    CAGradientLayer *g = [CAGradientLayer layer];
    g.colors = glass(NO); g.borderWidth = 1.5; g.borderColor = [UIColor colorWithWhite:1 alpha:0.42].CGColor;
    return g;
}
static void glass_shadow(UIView *v) {
    v.layer.shadowColor = UIColor.blackColor.CGColor; v.layer.shadowOpacity = 0.35; v.layer.shadowRadius = 4; v.layer.shadowOffset = CGSizeMake(0, 2);
}
@interface WonderPadButton : UIView
@property(nonatomic) uint64_t mask;
@property(nonatomic) BOOL held;
@property(nonatomic, strong) UILabel *label;
@property(nonatomic, strong) CAGradientLayer *fill;
@end
@implementation WonderPadButton
- (void)layoutSubviews {
    [super layoutSubviews];
    CGRect r = self.bounds; CGFloat radius = fmin(r.size.width, r.size.height) / 2;
    [CATransaction begin]; [CATransaction setDisableActions:YES];
    self.fill.frame = r; self.fill.cornerRadius = radius;
    self.layer.shadowPath = [UIBezierPath bezierPathWithRoundedRect:r cornerRadius:radius].CGPath;
    [CATransaction commit];
    self.label.frame = r;
    self.label.font = [UIFont systemFontOfSize:fmax(12, r.size.height * 0.42) weight:UIFontWeightBold];
    UIFontDescriptor *rounded = [self.label.font.fontDescriptor fontDescriptorWithDesign:UIFontDescriptorSystemDesignRounded];
    if (rounded) self.label.font = [UIFont fontWithDescriptor:rounded size:0];
}
- (void)setHeld:(BOOL)held {
    if (held == _held) return;
    _held = held;
    [CATransaction begin]; [CATransaction setAnimationDuration:0.06];
    self.fill.colors = glass(held);
    [CATransaction commit];
    self.label.textColor = held ? [UIColor colorWithWhite:0.1 alpha:0.9] : [UIColor colorWithWhite:1 alpha:0.92];
    [UIView animateWithDuration:0.06 delay:0 options:UIViewAnimationOptionBeginFromCurrentState | UIViewAnimationOptionAllowUserInteraction
                     animations:^{ self.transform = held ? CGAffineTransformMakeScale(0.9, 0.9) : CGAffineTransformIdentity; } completion:nil];
}
@end
@interface WonderControlsView : UIView
@end
@implementation WonderControlsView {
    NSMutableArray<WonderPadButton *> *buttons;
    UIView *stickBase, *stickKnob;
    UITouch *stickTouch;
    CGPoint stickHome;   // where the stick rests while not held
    CGRect stickZone;    // a touch starting here (and not on a button) takes the stick, centred on the touch
    UIImpactFeedbackGenerator *haptic;
    CAGradientLayer *stickFill, *knobFill;
}
- (WonderPadButton *)button:(NSString *)title mask:(uint64_t)mask round:(BOOL)round {
    (void)round;   // every button is a circle or a capsule (corner radius = half the short side)
    WonderPadButton *b = [WonderPadButton new];
    b.mask = mask; b.userInteractionEnabled = NO;
    b.fill = glass_layer(); [b.layer addSublayer:b.fill]; glass_shadow(b);
    b.label = [UILabel new]; b.label.text = title; b.label.textAlignment = NSTextAlignmentCenter;
    b.label.textColor = [UIColor colorWithWhite:1 alpha:0.92]; [b addSubview:b.label];
    [self addSubview:b]; [buttons addObject:b];
    return b;
}
- (instancetype)initWithFrame:(CGRect)frame {
    if (!(self = [super initWithFrame:frame])) return nil;
    self.multipleTouchEnabled = YES; self.backgroundColor = UIColor.clearColor;
    // Heavy impact at full intensity: clearly felt through a held phone (the light style was easy to miss).
    haptic = [[UIImpactFeedbackGenerator alloc] initWithStyle:UIImpactFeedbackStyleHeavy]; [haptic prepare];
    buttons = [NSMutableArray array];
    // Stick: a recessed well (darker towards the rim) and a raised knob with a grip ring.
    stickBase = [UIView new]; stickBase.userInteractionEnabled = NO; [self addSubview:stickBase];
    stickFill = glass_layer(); stickFill.type = kCAGradientLayerRadial; stickFill.startPoint = CGPointMake(0.5, 0.5); stickFill.endPoint = CGPointMake(1, 1);
    stickFill.colors = @[(id)[UIColor colorWithWhite:0.2 alpha:0.22].CGColor, (id)[UIColor colorWithWhite:0.02 alpha:0.48].CGColor];
    stickFill.borderColor = [UIColor colorWithWhite:1 alpha:0.32].CGColor; [stickBase.layer addSublayer:stickFill];
    stickKnob = [UIView new]; stickKnob.userInteractionEnabled = NO; glass_shadow(stickKnob); [self addSubview:stickKnob];
    knobFill = glass_layer(); knobFill.colors = @[(id)[UIColor colorWithWhite:0.92 alpha:0.55].CGColor, (id)[UIColor colorWithWhite:0.55 alpha:0.45].CGColor];
    knobFill.borderColor = [UIColor colorWithWhite:1 alpha:0.6].CGColor; [stickKnob.layer addSublayer:knobFill];
    CALayer *grip = [CALayer layer]; grip.borderWidth = 1.5; grip.borderColor = [UIColor colorWithWhite:0.15 alpha:0.25].CGColor; grip.name = @"grip"; [knobFill addSublayer:grip];
    [self button:@"ZL" mask:WONDER_BTN_ZL round:NO]; [self button:@"L" mask:WONDER_BTN_L round:NO];
    [self button:@"−" mask:WONDER_BTN_MINUS round:YES]; [self button:@"+" mask:WONDER_BTN_PLUS round:YES];
    [self button:@"R" mask:WONDER_BTN_R round:NO]; [self button:@"ZR" mask:WONDER_BTN_ZR round:NO];
    [self button:@"X" mask:WONDER_BTN_X round:YES]; [self button:@"Y" mask:WONDER_BTN_Y round:YES];
    [self button:@"A" mask:WONDER_BTN_A round:YES]; [self button:@"B" mask:WONDER_BTN_B round:YES];
    return self;
}
- (void)place:(uint64_t)mask rect:(CGRect)r { WonderPadButton *b = [self buttonFor:mask]; b.bounds = (CGRect){CGPointZero, r.size}; b.center = CGPointMake(CGRectGetMidX(r), CGRectGetMidY(r)); }
- (WonderPadButton *)buttonFor:(uint64_t)mask { for (WonderPadButton *b in buttons) if (b.mask == mask) return b; return nil; }
- (void)layoutSubviews {
    [super layoutSubviews];
    // Sized from the short side (landscape height); buttons stay inside the safe area (notch / home indicator).
    UIEdgeInsets inset = self.safeAreaInsets;
    CGFloat w = self.bounds.size.width, h = self.bounds.size.height, m = 10, gap = 8;
    CGFloat left = inset.left + m, right = w - inset.right - m, top = inset.top + m, bottom = h - inset.bottom - m;
    CGFloat b = fmin(h / 6.0, 72);
    CGFloat sw = b * 1.15, sh = b * 0.62;   // shoulders
    [self place:WONDER_BTN_ZL rect:CGRectMake(left, top, sw, sh)];
    [self place:WONDER_BTN_L rect:CGRectMake(left, top + sh + gap, sw, sh)];
    [self place:WONDER_BTN_ZR rect:CGRectMake(right - sw, top, sw, sh)];
    [self place:WONDER_BTN_R rect:CGRectMake(right - sw, top + sh + gap, sw, sh)];
    CGFloat small = sh;
    [self place:WONDER_BTN_MINUS rect:CGRectMake(w / 2 - gap - small, top, small, small)];
    [self place:WONDER_BTN_PLUS rect:CGRectMake(w / 2 + gap, top, small, small)];
    // Bottom corners, mirrored: the stick on the left has the same centre height and edge distance as the A/B/X/Y group.
    CGFloat radius = b * 0.95, f = b * 0.9;
    CGPoint face = CGPointMake(right - 1.5 * f, bottom - 1.5 * f), stick = CGPointMake(left + 1.5 * f, face.y);
    stickBase.bounds = CGRectMake(0, 0, 2 * radius, 2 * radius);
    stickHome = stick; if (!stickTouch) stickBase.center = stickHome;
    CGFloat zoneTop = top + 2 * sh + 2 * gap;   // below the L button, left 45% of the screen
    stickZone = CGRectMake(0, zoneTop, w * 0.45, h - zoneTop);
    CGFloat knob = radius * 0.9; stickKnob.bounds = CGRectMake(0, 0, knob, knob);
    [CATransaction begin]; [CATransaction setDisableActions:YES];
    stickFill.frame = stickBase.bounds; stickFill.cornerRadius = radius;
    knobFill.frame = stickKnob.bounds; knobFill.cornerRadius = knob / 2;
    stickKnob.layer.shadowPath = [UIBezierPath bezierPathWithOvalInRect:stickKnob.bounds].CGPath;
    for (CALayer *l in knobFill.sublayers) if ([l.name isEqualToString:@"grip"]) { l.frame = CGRectInset(stickKnob.bounds, knob * 0.2, knob * 0.2); l.cornerRadius = knob * 0.3; }
    [CATransaction commit];
    if (!stickTouch) stickKnob.center = stickBase.center;
    [self place:WONDER_BTN_X rect:CGRectMake(face.x - f / 2, face.y - 1.5 * f, f, f)];
    [self place:WONDER_BTN_B rect:CGRectMake(face.x - f / 2, face.y + 0.5 * f, f, f)];
    [self place:WONDER_BTN_Y rect:CGRectMake(face.x - 1.5 * f, face.y - f / 2, f, f)];
    [self place:WONDER_BTN_A rect:CGRectMake(face.x + 0.5 * f, face.y - f / 2, f, f)];
}
- (uint64_t)buttonsAt:(CGPoint)p {
    uint64_t mask = 0;
    for (WonderPadButton *b in buttons) {   // untransformed rect: a held button shrinks on screen, not its touch area
        CGRect hit = CGRectMake(b.center.x - b.bounds.size.width / 2, b.center.y - b.bounds.size.height / 2, b.bounds.size.width, b.bounds.size.height);
        if (CGRectContainsPoint(CGRectInset(hit, -6, -6), p)) mask |= b.mask;
    }
    return mask;
}
- (void)update:(UIEvent *)event {
    uint64_t pressed = 0; int stickAlive = 0;
    for (UITouch *t in [event touchesForView:self]) {
        if (t.phase == UITouchPhaseEnded || t.phase == UITouchPhaseCancelled) { if (t == stickTouch) stickTouch = nil; continue; }
        CGPoint p = [t locationInView:self];
        uint64_t hit = t == stickTouch ? 0 : [self buttonsAt:p];
        if (!stickTouch && !hit && t.phase == UITouchPhaseBegan && CGRectContainsPoint(stickZone, p)) {
            stickTouch = t;   // the whole base stays on screen
            CGFloat R = stickBase.bounds.size.width / 2, w = self.bounds.size.width, h = self.bounds.size.height;
            stickBase.center = CGPointMake(fmin(fmax(p.x, R), w - R), fmin(fmax(p.y, R), h - R));
        }
        if (t == stickTouch) {
            stickAlive = 1;
            CGFloat r = stickBase.bounds.size.width / 2, dx = p.x - stickBase.center.x, dy = p.y - stickBase.center.y, len = hypot(dx, dy);
            if (len > r) { dx *= r / len; dy *= r / len; }
            stickKnob.center = CGPointMake(stickBase.center.x + dx, stickBase.center.y + dy);
            if (hypot(dx, dy) < 0.12 * r) dx = dy = 0;   // dead zone: the touch-down point is the centre
            touch_lx = (int32_t)lround(dx / r * WONDER_STICK_MAX); touch_ly = (int32_t)lround(-dy / r * WONDER_STICK_MAX);   // Switch: up positive
            continue;
        }
        pressed |= hit;
    }
    if (!stickAlive) { stickTouch = nil; touch_lx = touch_ly = 0; stickBase.center = stickHome; stickKnob.center = stickHome; }
    for (WonderPadButton *b in buttons) b.held = (pressed & b.mask) != 0;
    if (pressed & ~touch_buttons) { [haptic impactOccurredWithIntensity:1.0]; [haptic prepare]; }   // a newly pressed button
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

// Scripted presses for unattended rounds (WONDER_AUTO_INPUT, input_script.h), merged like the other sources.
static void start_script(void) {
    const char *spec = getenv("WONDER_AUTO_INPUT"); if (!spec || !*spec) return;
    static struct wonder_input_script script; char err[96] = "";
    if (wonder_input_script_parse(spec, &script, err, sizeof err)) { fprintf(stderr, "HOST_INPUT_SCRIPT invalid '%s': %s\n", spec, err); return; }
    fprintf(stderr, "HOST_INPUT_SCRIPT '%s' (%d items)\n", spec, script.count);
    static dispatch_source_t timer; static CFAbsoluteTime start;
    start = CFAbsoluteTimeGetCurrent();
    timer = dispatch_source_create(DISPATCH_SOURCE_TYPE_TIMER, 0, 0, dispatch_get_main_queue());
    dispatch_source_set_timer(timer, DISPATCH_TIME_NOW, 50 * NSEC_PER_MSEC, 5 * NSEC_PER_MSEC);
    dispatch_source_set_event_handler(timer, ^{
        uint64_t b = wonder_input_script_buttons(&script, CFAbsoluteTimeGetCurrent() - start);
        if (b & ~script_buttons)   // a scripted press: capture the screen 1.5 s later (what the press led to)
            dispatch_after(dispatch_time(DISPATCH_TIME_NOW, 1500 * NSEC_PER_MSEC), dispatch_get_main_queue(), ^{
                extern void wonder_host_request_present_capture(void); wonder_host_request_present_capture(); });
        if (b != script_buttons) { script_buttons = b; publish(); }
    });
    dispatch_resume(timer);
}
UIView *wonder_input_controls(void) {
    NSCAssert(NSThread.isMainThread, @"input controls belong to the UIKit thread");
    static WonderControlsView *view; if (!view) view = [WonderControlsView new];
    start_pads();
    static int scripted; if (!scripted) { scripted = 1; start_script(); }
    return view;
}
