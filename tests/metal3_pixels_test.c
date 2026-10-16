#include "metal3_pixels.h"
#include <assert.h>
#include <string.h>
#include <stdio.h>
int main(void){
    unsigned char rgba[64*256];
    const unsigned char values[4][4]={{128,8,12,255},{16,64,24,255},{8,16,191,255},{128,64,191,255}};
    for(unsigned y=0;y<64;y++)for(unsigned x=0;x<64;x++)memcpy(rgba+y*256+x*4,values[(y>=32)*2+(x>=32)],4);
    unsigned bad=999;assert(!wonder_metal3_check_pixels(rgba,64,64,256,&bad)&&bad==0);
    rgba[0]=0;assert(wonder_metal3_check_pixels(rgba,64,64,256,&bad)&&bad==1);
    memset(rgba,0,sizeof rgba);assert(wonder_metal3_check_pixels(rgba,64,64,256,&bad)&&bad==4096);
    assert(wonder_metal3_check_pixels(rgba,0,64,256,&bad));assert(wonder_metal3_check_pixels(rgba,64,64,1,&bad));
    puts("METAL3_PIXEL_NEGATIVE_CONTROLS_OK");
}
