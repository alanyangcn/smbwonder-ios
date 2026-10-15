#import <Foundation/Foundation.h>
#import <CoreText/CoreText.h>
#import <CoreGraphics/CoreGraphics.h>
#include "shared_font.h"
#include <pthread.h>
#include <errno.h>
static pthread_mutex_t fonts_lock=PTHREAD_MUTEX_INITIALIZER;
static uint8_t *font_bytes[6];static size_t font_sizes[6];
// System font substitution for this native port. These are iOS faces, not Nintendo shared-font data.
// Bytes remain valid for process lifetime, as required by GetSharedFontAddress.
const void *wonder_host_shared_font(unsigned type,size_t *size){
 if(type>=6||!size)return NULL;
 pthread_mutex_lock(&fonts_lock);
 if(!font_bytes[type]){@autoreleasepool{
  const CFStringRef names[]={CFSTR("HiraginoSans-W3"),CFSTR("PingFangSC-Regular"),CFSTR("PingFangSC-Regular"),CFSTR("PingFangTC-Regular"),CFSTR("AppleSDGothicNeo-Regular"),CFSTR("AppleSymbols")};
  CTFontRef font=CTFontCreateWithName(names[type],16,NULL);
  CFArrayRef tags=font?CTFontCopyAvailableTables(font,kCTFontTableOptionNoOptions):NULL;
  CFIndex count=tags?CFArrayGetCount(tags):0;struct wonder_font_table tables[128];CFDataRef data[128]={0};unsigned n=0;int error=0;
  if(count<1||count>128)error=EINVAL;
  for(CFIndex i=0;!error&&i<count;i++){
   CTFontTableTag tag=(CTFontTableTag)(uintptr_t)CFArrayGetValueAtIndex(tags,i);
   CFDataRef bytes=CTFontCopyTable(font,tag,kCTFontTableOptionNoOptions);
   if(!bytes){error=EINVAL;break;}
   data[n]=bytes;tables[n]=(struct wonder_font_table){tag,CFDataGetBytePtr(bytes),(size_t)CFDataGetLength(bytes)};n++;
  }
  uint8_t *result=NULL;size_t length=0;
  if(!error)error=wonder_sfnt_build(tables,n,&result,&length);
  if(!error){
   CGDataProviderRef provider=CGDataProviderCreateWithData(NULL,result,length,NULL);
   CGFontRef check=provider?CGFontCreateWithDataProvider(provider):NULL;
   if(!check||CGFontGetNumberOfGlyphs(check)==0)error=EINVAL;
   if(check)CGFontRelease(check);if(provider)CGDataProviderRelease(provider);
  }
  CFStringRef name=font?CTFontCopyPostScriptName(font):NULL;
  fprintf(stderr,"HOST_SHARED_FONT type=%u face=%s bytes=%zu result=%d source=iOS-system-substitute\n",type,name?[(__bridge NSString*)name UTF8String]:"unavailable",length,error);
  if(error)free(result);else{font_bytes[type]=result;font_sizes[type]=length;}
  if(name)CFRelease(name);for(unsigned i=0;i<n;i++)CFRelease(data[i]);if(tags)CFRelease(tags);if(font)CFRelease(font);
 }}
 const void *result=font_bytes[type];*size=font_sizes[type];pthread_mutex_unlock(&fonts_lock);return result;
}
