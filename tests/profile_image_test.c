// Offline profile image: a deterministic 256x256 JPEG within nn::account's 128 KiB ProfileImageBytesMax.
#include "profile_image.h"
#include <ImageIO/ImageIO.h>
#include <assert.h>
#include <stdio.h>
int main(void) {
    size_t n = 0, m = 0; const unsigned char *a = wonder_profile_image(&n), *b = wonder_profile_image(&m);
    assert(a && a == b && n == m && n > 1000 && n <= 0x20000);
    assert(a[0] == 0xff && a[1] == 0xd8 && a[n - 2] == 0xff && a[n - 1] == 0xd9);   // JPEG SOI ... EOI
    CFDataRef data = CFDataCreate(NULL, a, (CFIndex)n); CGImageSourceRef src = CGImageSourceCreateWithData(data, NULL);
    CGImageRef image = CGImageSourceCreateImageAtIndex(src, 0, NULL);
    assert(image && CGImageGetWidth(image) == 256 && CGImageGetHeight(image) == 256);
    CFStringRef type = CGImageSourceGetType(src); assert(type && CFStringCompare(type, CFSTR("public.jpeg"), 0) == kCFCompareEqualTo);
    CGImageRelease(image); CFRelease(src); CFRelease(data);
    puts("profile_image OK");
    return 0;
}
