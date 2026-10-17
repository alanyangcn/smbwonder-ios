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
// Candidates per shared-font type, first usable wins. Usable = the face exists under that PostScript name (CoreText
// silently substitutes another face otherwise) and has TrueType (glyf) or CFF outlines: since iOS/macOS 27 PingFang
// is an hvgl variable font without either, which the game's font engine cannot rasterise.
static int outline_tag(CTFontTableTag tag){return tag==0x676c7966||tag==0x43464620||tag==0x43464632;}   // glyf, CFF , CFF2
static int build_face(CFStringRef want,uint8_t **out,size_t *out_size){
 CTFontRef font=CTFontCreateWithName(want,16,NULL);
 CFStringRef got=font?CTFontCopyPostScriptName(font):NULL;
 int error=!got||CFStringCompare(got,want,0)!=kCFCompareEqualTo?ENOENT:0;
 CFArrayRef tags=!error?CTFontCopyAvailableTables(font,kCTFontTableOptionNoOptions):NULL;
 CFIndex count=tags?CFArrayGetCount(tags):0;struct wonder_font_table tables[128];CFDataRef data[128]={0};unsigned n=0,outlines=0;
 if(!error&&(count<1||count>128))error=EINVAL;
 for(CFIndex i=0;!error&&i<count;i++){
  CTFontTableTag tag=(CTFontTableTag)(uintptr_t)CFArrayGetValueAtIndex(tags,i);
  CFDataRef bytes=CTFontCopyTable(font,tag,kCTFontTableOptionNoOptions);
  if(!bytes){error=EINVAL;break;}
  outlines|=outline_tag(tag);
  data[n]=bytes;tables[n]=(struct wonder_font_table){tag,CFDataGetBytePtr(bytes),(size_t)CFDataGetLength(bytes)};n++;
 }
 if(!error&&!outlines)error=EFTYPE;
 uint8_t *result=NULL;size_t length=0;
 if(!error)error=wonder_sfnt_build(tables,n,&result,&length);
 if(!error){
  CGDataProviderRef provider=CGDataProviderCreateWithData(NULL,result,length,NULL);
  CGFontRef check=provider?CGFontCreateWithDataProvider(provider):NULL;
  if(!check||CGFontGetNumberOfGlyphs(check)==0)error=EINVAL;
  if(check)CGFontRelease(check);if(provider)CGDataProviderRelease(provider);
 }
 if(error){free(result);result=NULL;length=0;}
 for(unsigned i=0;i<n;i++)CFRelease(data[i]);if(tags)CFRelease(tags);if(got)CFRelease(got);if(font)CFRelease(font);
 *out=result;*out_size=length;return error;
}
// Type 5 (Nintendo extension: button icons in the private use area) has no iOS counterpart (AppleSymbols showed "?" for
// every icon, round139): the app bundles a drawn icon face, tools/make_icon_font.py -> dm/fonts/wonder-icons.ttf
// (WONDER_ICON_FONT overrides the path for host tests). Missing or unreadable -> NULL, and the font service stops by name.
static int load_icon_face(uint8_t **out,size_t *out_size,char *path_out,size_t path_size){
 const char *env=getenv("WONDER_ICON_FONT");
 NSString *path=env&&*env?@(env):[NSBundle.mainBundle.resourcePath stringByAppendingPathComponent:@"dm/fonts/wonder-icons.ttf"];
 snprintf(path_out,path_size,"%s",path.fileSystemRepresentation);
 NSData *data=[NSData dataWithContentsOfFile:path];if(!data||data.length<12)return ENOENT;
 uint8_t *bytes=malloc(data.length);if(!bytes)return ENOMEM;memcpy(bytes,data.bytes,data.length);
 CGDataProviderRef provider=CGDataProviderCreateWithData(NULL,bytes,data.length,NULL);
 CGFontRef check=provider?CGFontCreateWithDataProvider(provider):NULL;int error=!check||CGFontGetNumberOfGlyphs(check)<2?EINVAL:0;
 if(check)CGFontRelease(check);if(provider)CGDataProviderRelease(provider);
 if(error){free(bytes);return error;}
 *out=bytes;*out_size=data.length;return 0;
}
const void *wonder_host_shared_font(unsigned type,size_t *size){
 if(type>=6||!size)return NULL;
 pthread_mutex_lock(&fonts_lock);
 if(!font_bytes[type]&&type==5){@autoreleasepool{
  uint8_t *bytes=NULL;size_t length=0;char path[1024];int error=load_icon_face(&bytes,&length,path,sizeof path);
  fprintf(stderr,"HOST_SHARED_FONT type=5 face=WonderPortIcons bytes=%zu result=%d path=%s source=bundled-icon-substitute\n",length,error,path);
  if(!error){font_bytes[5]=bytes;font_sizes[5]=length;}
 }}
 if(!font_bytes[type]&&type<5){@autoreleasepool{
  static const char *const candidates[5][4]={
   {"HiraginoSans-W3",NULL},                                                   // Japanese / standard
   {"PingFangSC-Regular","STHeitiSC-Light","HiraginoSansGB-W3",NULL},          // Chinese simplified
   {"PingFangSC-Regular","STHeitiSC-Light","HiraginoSansGB-W3",NULL},          // Chinese simplified extension
   {"PingFangTC-Regular","STHeitiTC-Light",NULL},                              // Chinese traditional
   {"AppleSDGothicNeo-Regular",NULL}};                                         // Korean
  for(unsigned i=0;candidates[type][i]&&!font_bytes[type];i++){
   CFStringRef want=CFStringCreateWithCString(NULL,candidates[type][i],kCFStringEncodingUTF8);
   uint8_t *bytes=NULL;size_t length=0;int error=build_face(want,&bytes,&length);CFRelease(want);
   fprintf(stderr,"HOST_SHARED_FONT type=%u face=%s bytes=%zu result=%d%s source=iOS-system-substitute\n",type,candidates[type][i],length,error,
           error==ENOENT?" (not installed)":error==EFTYPE?" (no glyf/CFF outlines)":"");
   if(!error){font_bytes[type]=bytes;font_sizes[type]=length;}
  }
 }}
 const void *result=font_bytes[type];*size=font_sizes[type];pthread_mutex_unlock(&fonts_lock);return result;
}
