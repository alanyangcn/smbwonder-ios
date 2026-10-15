#include "dm.h"
#include <assert.h>
#include <sys/mman.h>
extern int wonder_kernel_call(int number, uint64_t *regs);
extern void wonder_kernel_init(uint64_t image_bytes);
struct dm_flags g_flag;
module_t g_mod[MAX_MOD]; int g_nmod;   // no modules loaded in this test
// Handle table stand-in (kernel_threads.c on device): one transfer-memory object at a time is enough here.
static void *test_object; static uint32_t test_kind; static void (*test_close)(void *);
uint32_t wonder_handle_new_object(uint32_t kind, void *obj, void (*close)(void *)) { test_object = obj; test_kind = kind; test_close = close; return 0x1234; }
void *wonder_handle_object(uint32_t h, uint32_t kind) { return h == 0x1234 && kind == test_kind ? test_object : NULL; }

int main(void) {
    uint64_t r[31] = {0};
    wonder_kernel_init(0x5000000);
    r[1]=18; r[2]=0xffff8001u;
    assert(wonder_kernel_call(0x29,r)); assert(!r[0]); assert(r[1]==0x010015100b514000ull);
    r[1]=999; assert(wonder_kernel_call(0x29,r)); assert(r[0]!=0);
    r[1]=0x200000; assert(wonder_kernel_call(1,r)); assert(r[0]==0); uint64_t heap=r[1];
    *(volatile uint64_t *)heap=0x123456789abcull;
    uint64_t info[5]={0}; r[0]=(uint64_t)info; r[2]=heap;
    assert(wonder_kernel_call(6,r)); assert(!r[0]); assert(info[0]==heap); assert(info[1]==0x200000);
    assert(((uint32_t *)info)[4]==5); assert(((uint32_t *)info)[6]==3);
    r[1]=0x200001; assert(wonder_kernel_call(1,r)); assert(r[0]!=0);
    r[1]=7; r[2]=0xffff8001u; assert(wonder_kernel_call(0x29,r)); assert(!r[0]); assert(r[1]==0x5200000);
    r[1]=0; assert(wonder_kernel_call(1,r)); assert(!r[0]); assert(r[1]==heap);
    r[0]=(uint64_t)info; r[2]=heap; assert(wonder_kernel_call(6,r)); assert(((uint32_t *)info)[4]==0);
    r[1]=0x200000;assert(wonder_kernel_call(1,r));assert(!r[0]);
    assert(*(volatile uint64_t *)heap==0); // Shrink and re-expand must not expose stale data.
    r[1]=2;assert(wonder_kernel_call(0x29,r));uint64_t alias=r[1];
    r[0]=alias;r[1]=0x4000;assert(wonder_kernel_call(0x2c,r));assert(!r[0]);
    *(volatile uint64_t *)alias=99;
    r[0]=(uint64_t)info;r[2]=alias;assert(wonder_kernel_call(6,r));assert(((uint32_t *)info)[4]==5);
    r[0]=alias;r[1]=0x4000;assert(wonder_kernel_call(0x2d,r));assert(!r[0]);
    r[0]=(uint64_t)info;r[2]=alias;assert(wonder_kernel_call(6,r));assert(((uint32_t *)info)[4]==0);
    // 4 KiB-granular MapPhysicalMemory (VammManager, nvn-round10) on 16 KiB host pages.
    r[0]=alias+0x1000;r[1]=0x1000;assert(wonder_kernel_call(0x2c,r));assert(!r[0]);
    r[0]=(uint64_t)info;r[2]=alias+0x1800;assert(wonder_kernel_call(6,r));assert(info[0]==alias+0x1000&&info[1]==0x1000&&((uint32_t *)info)[4]==5);
    r[0]=(uint64_t)info;r[2]=alias;assert(wonder_kernel_call(6,r));assert(info[0]==alias&&info[1]==0x1000&&((uint32_t *)info)[4]==0);
    *(volatile uint64_t *)(alias+0x1000)=5;
    r[0]=alias+0x1000;r[1]=0x2000;assert(wonder_kernel_call(0x2c,r));assert(!r[0]);   // partially mapped range: maps the rest
    assert(*(volatile uint64_t *)(alias+0x1000)==5 && *(volatile uint64_t *)(alias+0x2000)==0);
    r[0]=(uint64_t)info;r[2]=alias+0x2000;assert(wonder_kernel_call(6,r));assert(info[0]==alias+0x1000&&info[1]==0x2000);
    r[0]=alias+0x1000;r[1]=0x1000;assert(wonder_kernel_call(0x2d,r));assert(!r[0]);
    assert(*(volatile uint64_t *)(alias+0x2000)==0);                                   // neighbour page still accessible
    r[0]=alias+0x1000;r[1]=0x1000;assert(wonder_kernel_call(0x2c,r));assert(!r[0]);
    assert(*(volatile uint64_t *)(alias+0x1000)==0);                                   // remapped page reads zero
    r[0]=alias+0x800;r[1]=0x1000;assert(wonder_kernel_call(0x2c,r));assert(r[0]==0xd401);
    r[0]=alias;r[1]=0x10000;assert(wonder_kernel_call(0x2d,r));assert(!r[0]);
    r[0]=(uint64_t)info;r[2]=alias+0x1000;assert(wonder_kernel_call(6,r));assert(((uint32_t *)info)[4]==0);
    // Thread SVCs moved to kernel_threads.c: this kernel must not claim them.
    const int thread_svcs[]={0x08,0x09,0x0c,0x0d,0x0e,0x0f,0x10,0x25,0x18,0x1a,0x45};
    for(unsigned i=0;i<sizeof thread_svcs/sizeof *thread_svcs;i++)assert(!wonder_kernel_call(thread_svcs[i],r));
    r[1]=0;r[2]=0xffff8001u;assert(wonder_kernel_call(0x29,r));assert(!r[0]&&r[1]==7);   // application cores 0-2
    // MapMemory / UnmapMemory: heap stack moved into the stack region at 4 KiB (not 16 KiB) alignment.
    r[1]=14;assert(wonder_kernel_call(0x29,r));uint64_t stack=r[1];
    r[1]=0x200000;assert(wonder_kernel_call(1,r));assert(!r[0]);heap=r[1];
    for(unsigned i=0;i<0x10000/8;i++)((volatile uint64_t *)heap)[i]=0xabc000+i;
    uint64_t dst=stack+0x3000;
    r[0]=dst;r[1]=heap;r[2]=0x10000;assert(wonder_kernel_call(4,r));assert(!r[0]);
    assert(((volatile uint64_t *)dst)[5]==0xabc005);
    r[0]=(uint64_t)info;r[2]=dst+0x100;assert(wonder_kernel_call(6,r));assert(info[0]==dst&&info[1]==0x10000&&((uint32_t *)info)[4]==0xb);
    r[0]=dst+0x1000;r[1]=heap+0x10000;r[2]=0x1000;assert(wonder_kernel_call(4,r));assert(r[0]==0xdc01);   // overlaps
    r[0]=dst+0x20000;r[1]=(uint64_t)info&~0xfffull;r[2]=0x1000;assert(wonder_kernel_call(4,r));assert(r[0]==0xd401); // src not heap
    r[0]=dst+0x20000;r[1]=heap;r[2]=0x800;assert(wonder_kernel_call(4,r));assert(r[0]==0xd401);                    // unaligned size
    ((volatile uint64_t *)dst)[7]=0x77;
    r[0]=dst;r[1]=heap+0x1000;r[2]=0x10000;assert(wonder_kernel_call(5,r));assert(r[0]==0xdc01);              // wrong src
    r[0]=dst;r[1]=heap;r[2]=0x10000;assert(wonder_kernel_call(5,r));assert(!r[0]);
    assert(((volatile uint64_t *)heap)[7]==0x77);                                                          // moved back
    r[0]=(uint64_t)info;r[2]=dst;assert(wonder_kernel_call(6,r));assert(((uint32_t *)info)[4]==0);
    r[0]=dst;r[1]=heap;r[2]=0x10000;assert(wonder_kernel_call(5,r));assert(r[0]==0xd401);                 // not mapped
    // Transfer memory (audio renderer work buffer): owner None, mapped by move, unmapped back.
    ((volatile uint64_t *)heap)[3]=0x7e57;
    r[1]=heap;r[2]=0x8000;r[3]=1;
    r[1]=heap+0x800;r[2]=0x8000;r[3]=0;assert(wonder_kernel_call(0x15,r));assert(r[0]==0xca01);              // unaligned
    r[1]=heap;r[2]=0x8000;r[3]=0;assert(wonder_kernel_call(0x15,r));assert(!r[0]&&r[1]==0x1234);
    uint64_t sbase;{uint64_t q[31]={0};q[1]=14;assert(wonder_kernel_call(0x29,q));sbase=q[1];}
    uint64_t tdst=sbase+0x4000000;
    r[0]=0x1234;r[1]=heap+0x100000;r[2]=0x8000;r[3]=0;assert(wonder_kernel_call(0x51,r));assert(r[0]==0xd401);  // inside heap
    r[0]=0x1234;r[1]=alias;r[2]=0x8000;r[3]=0;assert(wonder_kernel_call(0x51,r));assert(r[0]==0xd401);         // alias region
    r[0]=0x1234;r[1]=tdst;r[2]=0x4000;r[3]=0;assert(wonder_kernel_call(0x51,r));assert(r[0]==0xca01);          // wrong size
    r[0]=0x1234;r[1]=tdst;r[2]=0x8000;r[3]=3;assert(wonder_kernel_call(0x51,r));assert(r[0]==0xfa01);          // perm != owner perm
    r[0]=0x1234;r[1]=tdst;r[2]=0x8000;r[3]=0;assert(wonder_kernel_call(0x51,r));assert(!r[0]);
    assert(((volatile uint64_t *)tdst)[3]==0x7e57);
    r[0]=(uint64_t)info;r[2]=tdst+8;assert(wonder_kernel_call(6,r));assert(info[0]==tdst&&((uint32_t *)info)[4]==0xe);
    r[0]=0x1234;r[1]=tdst+0x10000;r[2]=0x8000;r[3]=0;assert(wonder_kernel_call(0x51,r));assert(r[0]==0xd401); // already mapped
    ((volatile uint64_t *)tdst)[4]=0x5eed;
    r[0]=0x1234;r[1]=tdst;r[2]=0x8000;assert(wonder_kernel_call(0x52,r));assert(!r[0]);
    assert(((volatile uint64_t *)heap)[4]==0x5eed);                                                          // moved back
    r[0]=0x1234;r[1]=tdst;r[2]=0x8000;assert(wonder_kernel_call(0x52,r));assert(r[0]==0xd401);
    r[0]=0x9999;r[1]=tdst;r[2]=0x8000;r[3]=3;assert(wonder_kernel_call(0x51,r));assert(r[0]==0xe401);          // bad handle
    test_close(test_object);
    assert(!wonder_kernel_call(0x7f,r));
    puts("SDK_KERNEL_TEST_OK");
}
