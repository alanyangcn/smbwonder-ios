#include "metal2_pixels.h"
#include <stdlib.h>
int wonder_metal2_check_pixels(const unsigned char *pixels,unsigned width,unsigned height,size_t stride,unsigned *bad){
    if(!pixels||!bad||width!=64||height!=64||stride<width*4)return -1;
    // Independent CPU oracle for 2x2 nearest sampling multiplied by (.5,.25,.75,1).
    const unsigned char expected[4][4]={{128,8,12,255},{16,64,24,255},{8,16,191,255},{128,64,191,255}};
    *bad=0;
    for(unsigned y=0;y<height;y++)for(unsigned x=0;x<width;x++){
        const unsigned char *p=pixels+y*stride+x*4,*e=expected[(y>=height/2)*2+(x>=width/2)];
        int wrong=0;for(unsigned c=0;c<4;c++)if(abs((int)p[c]-(int)e[c])>1)wrong=1;
        *bad+=wrong;
    }
    return *bad?1:0;
}
