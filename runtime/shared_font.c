#include "shared_font.h"
#include <stdlib.h>
#include <string.h>
#include <errno.h>
static void put16(uint8_t *p,unsigned v){p[0]=v>>8;p[1]=v;}
static void put32(uint8_t *p,uint32_t v){p[0]=v>>24;p[1]=v>>16;p[2]=v>>8;p[3]=v;}
static uint32_t checksum(const uint8_t *p,size_t n){uint32_t sum=0;for(size_t i=0;i<n;i+=4)sum+=((uint32_t)p[i]<<24)|((uint32_t)p[i+1]<<16)|((uint32_t)p[i+2]<<8)|p[i+3];return sum;}
static int order(const void *a,const void *b){uint32_t x=((const struct wonder_font_table*)a)->tag,y=((const struct wonder_font_table*)b)->tag;return(x>y)-(x<y);}
int wonder_sfnt_build(const struct wonder_font_table *tables,unsigned count,uint8_t **output,size_t *size){
 if(!tables||!count||count>128||!output||!size)return EINVAL;
 struct wonder_font_table sorted[128];memcpy(sorted,tables,count*sizeof *tables);qsort(sorted,count,sizeof *sorted,order);
 size_t total=12+16*count;int has_head=0,cff=0;
 for(unsigned i=0;i<count;i++){
  if((i&&sorted[i].tag==sorted[i-1].tag)||sorted[i].size>64*1024*1024||(!sorted[i].bytes&&sorted[i].size))return EINVAL;
  if(sorted[i].tag==0x68656164){if(sorted[i].size<54)return EINVAL;has_head=1;}
  if(sorted[i].tag==0x43464620||sorted[i].tag==0x43464632)cff=1;
  total+=(sorted[i].size+3)&~(size_t)3;if(total>64*1024*1024)return EINVAL;
 }
 if(!has_head)return EINVAL;
 uint8_t *font=calloc(1,total);if(!font)return ENOMEM;
 unsigned pow=1,selector=0;while(pow*2<=count){pow*=2;selector++;}
 put32(font,cff?0x4f54544f:0x10000);put16(font+4,count);put16(font+6,pow*16);put16(font+8,selector);put16(font+10,count*16-pow*16);
 size_t offset=12+16*count,head_offset=0;
 for(unsigned i=0;i<count;i++){
  uint8_t *record=font+12+16*i;put32(record,sorted[i].tag);put32(record+8,(uint32_t)offset);put32(record+12,(uint32_t)sorted[i].size);
  if(sorted[i].size)memcpy(font+offset,sorted[i].bytes,sorted[i].size);
  if(sorted[i].tag==0x68656164){head_offset=offset;memset(font+offset+8,0,4);}
  size_t padded=(sorted[i].size+3)&~(size_t)3;put32(record+4,checksum(font+offset,padded));offset+=padded;
 }
 put32(font+head_offset+8,0xb1b0afba-checksum(font,total));*output=font;*size=total;return 0;
}
