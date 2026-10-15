from pathlib import Path
import subprocess,tempfile,unittest
ROOT=Path(__file__).resolve().parents[1]
class SharedFontTests(unittest.TestCase):
    def test_builds_valid_sfnt_checksums_and_rejects_missing_tables(self):
        with tempfile.TemporaryDirectory() as d:
            p=Path(d);source=p/'test.c'
            source.write_text(r'''
#include "shared_font.h"
#include <assert.h>
#include <stdlib.h>
#include <string.h>
static unsigned be(const unsigned char *p){return (unsigned)p[0]<<24|(unsigned)p[1]<<16|(unsigned)p[2]<<8|p[3];}
int main(void){
 unsigned char head[54]={0}, cmap[5]={0,1,2,3,4}, maxp[6]={0,1};
 struct wonder_font_table t[]={{0x68656164,head,sizeof head},{0x636d6170,cmap,sizeof cmap},{0x6d617870,maxp,sizeof maxp}};
 unsigned char *out=0;size_t size=0;
 assert(wonder_sfnt_build(t,3,&out,&size)==0);
 assert(size%4==0 && be(out)==0x10000 && out[5]==3);
 unsigned sum=0;for(size_t i=0;i<size;i+=4)sum+=be(out+i);assert(sum==0xb1b0afba);
 assert(be(out+12)==0x636d6170 && be(out+28)==0x68656164 && be(out+44)==0x6d617870);
 assert(!memcmp(out+be(out+20),cmap,sizeof cmap));
 unsigned offset=be(out+36);assert(be(out+offset+8)!=0);
 free(out);t[0].size=8;assert(wonder_sfnt_build(t,3,&out,&size)!=0);
 t[0].size=sizeof head;t[1].tag=t[0].tag;assert(wonder_sfnt_build(t,3,&out,&size)!=0);
 assert(wonder_sfnt_build(t,0,&out,&size)!=0);
 return 0;
}
''')
            subprocess.run(['clang','-std=c11','-Wall','-Wextra','-Werror','-I'+str(ROOT/'runtime'),str(source),str(ROOT/'runtime/shared_font.c'),'-o',str(p/'test')],check=True)
            subprocess.run([str(p/'test')],check=True)

    def test_host_faces_are_valid_and_keep_stable_addresses(self):
        with tempfile.TemporaryDirectory() as d:
            p=Path(d);source=p/'host.m'
            source.write_text(r'''#import <CoreGraphics/CoreGraphics.h>
#include <assert.h>
#include <stddef.h>
extern const void *wonder_host_shared_font(unsigned,size_t *);
int main(void){for(unsigned type=0;type<6;type++){
 size_t size=0,again_size=0;const void *bytes=wonder_host_shared_font(type,&size);
 assert(bytes&&size>1024&&wonder_host_shared_font(type,&again_size)==bytes&&size==again_size);
 CGDataProviderRef p=CGDataProviderCreateWithData(NULL,bytes,size,NULL);assert(p);
 CGFontRef f=CGFontCreateWithDataProvider(p);assert(f&&CGFontGetNumberOfGlyphs(f)>0);
 CGFontRelease(f);CGDataProviderRelease(p);
 }size_t size=123;assert(!wonder_host_shared_font(6,&size));return 0;}''')
            subprocess.run(['clang','-fobjc-arc','-Wall','-Wextra','-Werror','-I'+str(ROOT/'runtime'),str(source),str(ROOT/'runtime/shared_font.c'),str(ROOT/'runtime/host_fonts.m'),'-framework','Foundation','-framework','CoreText','-framework','CoreGraphics','-o',str(p/'test')],check=True)
            subprocess.run([str(p/'test')],check=True)
