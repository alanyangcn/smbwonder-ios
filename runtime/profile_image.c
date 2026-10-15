#include "profile_image.h"
#include <CoreGraphics/CoreGraphics.h>
#include <ImageIO/ImageIO.h>
#include <pthread.h>
#include <stdlib.h>
#include <string.h>
static unsigned char *image; static size_t image_size;
static void generate(void) {
    enum { S = 256 };
    CGColorSpaceRef space = CGColorSpaceCreateDeviceRGB();
    CGContextRef c = CGBitmapContextCreate(NULL, S, S, 8, S * 4, space, (CGBitmapInfo)kCGImageAlphaNoneSkipLast);
    if (!c) { CGColorSpaceRelease(space); return; }
    CGContextSetRGBFillColor(c, 0.20, 0.45, 0.85, 1); CGContextFillRect(c, CGRectMake(0, 0, S, S));          // blue background
    CGContextSetRGBFillColor(c, 0.95, 0.95, 0.95, 1); CGContextFillEllipseInRect(c, CGRectMake(88, 128, 80, 80));   // head
    CGContextFillEllipseInRect(c, CGRectMake(48, -40, 160, 150));                                                     // shoulders
    CGImageRef img = CGBitmapContextCreateImage(c);
    CFMutableDataRef data = CFDataCreateMutable(NULL, 0);
    CGImageDestinationRef dst = img ? CGImageDestinationCreateWithData(data, CFSTR("public.jpeg"), 1, NULL) : NULL;
    if (dst) {
        float quality = 0.9f; CFNumberRef q = CFNumberCreate(NULL, kCFNumberFloatType, &quality);
        const void *keys[] = {kCGImageDestinationLossyCompressionQuality}, *values[] = {q};
        CFDictionaryRef options = CFDictionaryCreate(NULL, keys, values, 1, &kCFTypeDictionaryKeyCallBacks, &kCFTypeDictionaryValueCallBacks);
        CGImageDestinationAddImage(dst, img, options);
        if (CGImageDestinationFinalize(dst) && CFDataGetLength(data) > 0) {
            image_size = (size_t)CFDataGetLength(data); image = malloc(image_size);
            if (image) memcpy(image, CFDataGetBytePtr(data), image_size); else image_size = 0;
        }
        CFRelease(options); CFRelease(q); CFRelease(dst);
    }
    CFRelease(data); if (img) CGImageRelease(img); CGContextRelease(c); CGColorSpaceRelease(space);
}
const unsigned char *wonder_profile_image(size_t *size) {
    static pthread_once_t once = PTHREAD_ONCE_INIT; pthread_once(&once, generate);
    if (size) *size = image_size;
    return image;
}
