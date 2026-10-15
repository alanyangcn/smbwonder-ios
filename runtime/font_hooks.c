// sdk+0x3a2920..0x3a2b30: Wonder polls state==1 then retrieves address and size.
#include "hle.h"
extern const void *wonder_host_shared_font(unsigned,size_t *);
_Noreturn void wonder_game_thread_stop(const char *);
static const void *font_data(unsigned type,size_t *size){
 const void *bytes=wonder_host_shared_font(type,size);
 if(!bytes||!*size)wonder_game_thread_stop("HOST_SHARED_FONT_UNAVAILABLE: iOS font could not be reconstructed");
 return bytes;
}
HLE_FORCE("_ZN2nn2pl22GetSharedFontLoadStateENS0_14SharedFontTypeE","Host system font is loaded only after valid SFNT bytes exist; target loaded enum=1",int,wonder_font_state,unsigned type){size_t size;font_data(type,&size);return 1;}
HLE_FORCE("_ZN2nn2pl20GetSharedFontAddressENS0_14SharedFontTypeE","Stable real SFNT font data, not a dummy address",const void *,wonder_font_address,unsigned type){size_t size;return font_data(type,&size);}
HLE_FORCE("_ZN2nn2pl17GetSharedFontSizeENS0_14SharedFontTypeE","Exact host font byte count",size_t,wonder_font_size,unsigned type){size_t size;font_data(type,&size);return size;}
