// Target startup kernel. Unsupported services are errors, never successful placeholders.
// ABI: switchbrew/libnx nx/include/switch/kernel/svc.h; register wrappers verified in target sdk.
#include "dm.h"
#include "kernel_events.h"
#include <sys/mman.h>
#include <unistd.h>
#include <errno.h>
#include <pthread.h>
static pthread_mutex_t kernel_lock = PTHREAD_MUTEX_INITIALIZER;
static uint64_t AS_SIZE, HEAP_OFFSET, POOL_SIZE, ALIAS_OFFSET, ALIAS_SIZE, STACK_OFFSET, STACK_SIZE;
// Resource limits are an explicit host diagnostic budget, not Switch physical memory.
static uint8_t *space;
static size_t heap_size, image_size;
static uint8_t *alias_bits;     // one bit per 4 KiB Horizon page of the alias region
static uint16_t *alias_host;    // mapped Horizon pages per 16 KiB host page
static uint64_t alias_mapped;   // bytes mapped through MapPhysicalMemory
static uint64_t entropy[4];
#define WONDER_SYSTEM_RESOURCE_SIZE 0x1000000ull
static struct { uint64_t base,size,src; uint32_t state,perm; } regions[256];
static unsigned nregion;
void wonder_kernel_region(uint64_t base, uint64_t size, uint32_t state, uint32_t perm) {
    pthread_mutex_lock(&kernel_lock);
    if (!size || base + size < base || nregion == 256) die("invalid kernel region");
    regions[nregion++] = (typeof(regions[0])){base,size,0,state,perm};
    pthread_mutex_unlock(&kernel_lock);
}
// Address-space layout (offsets in one host reservation, sized from what iOS grants):
//   [0, 1792 MiB)      alias region: SDK VammManager maps on demand (MapPhysicalMemory). Wonder reserves
//                      0x6d600000 (1750 MiB) of virtual-address heap there when VAMM is enabled (main+0x61231c).
//   [1792, +256 MiB)   stack region (MapMemory)
//   [2048 MiB, end)    heap region (SetHeapSize)
// Without the extended-virtual-addressing entitlement iOS rejects a 4 GiB reservation (nvn-round12), so the
// largest grantable size is measured and HOST_VA_HEADROOM is left for Metal and the host itself.
// Memory is committed lazily by iOS: reported sizes are address space; only touched pages cost RAM.
#define CONSOLE_APPLICATION_POOL 0xcd500000ull   /* 3285 MiB application pool of a 4 GiB console */
#define WONDER_VAMM_RESERVATION 0x6d600000ull    /* main+0x61231c */
#define HOST_VA_HEADROOM (512ull << 20)
#define MIN_HEAP_REGION (256ull << 20)
static uint64_t largest_reservation(void) {   // 256 MiB steps between 1 and 8 GiB
    uint64_t lo=0,hi=32;                      // units of 256 MiB
    while(lo<hi){
        uint64_t mid=(lo+hi+1)/2,size=mid<<28;
        void *p=mmap(NULL,size,PROT_NONE,MAP_PRIVATE|MAP_ANON,-1,0);
        if(p!=MAP_FAILED){munmap(p,size);lo=mid;}else hi=mid-1;
    }
    return lo<<28;
}
void wonder_kernel_init(uint64_t bytes) {
    if (space) die("SDK kernel initialized twice");
    image_size = bytes;
    ALIAS_OFFSET=0;ALIAS_SIZE=1792ull<<20;
    STACK_OFFSET=ALIAS_OFFSET+ALIAS_SIZE;STACK_SIZE=256ull<<20;
    HEAP_OFFSET=STACK_OFFSET+STACK_SIZE;
    uint64_t largest=largest_reservation();
    LOG("SDK_KERNEL largest_reservation=%llu MiB",(unsigned long long)(largest>>20));
    if(largest<HEAP_OFFSET+MIN_HEAP_REGION+HOST_VA_HEADROOM)
        die("HOST_ADDRESS_SPACE_TOO_SMALL largest=%llu MiB, need %llu MiB (VAMM alias 1792 + stacks 256 + heap 256 + headroom 512); "
            "the extended-virtual-addressing entitlement is required",(unsigned long long)(largest>>20),
            (unsigned long long)((HEAP_OFFSET+MIN_HEAP_REGION+HOST_VA_HEADROOM)>>20));
    AS_SIZE=largest-HOST_VA_HEADROOM;
    space=mmap(NULL,AS_SIZE,PROT_NONE,MAP_PRIVATE|MAP_ANON,-1,0);
    if (space == MAP_FAILED) die("SDK address reservation failed errno=%d",errno);
    uint64_t heap_capacity=AS_SIZE-HEAP_OFFSET;
    // Report the console pool when the heap region can hold the heap the game derives from it
    // (pool - system resource - image - VAMM reservation); otherwise shrink the pool to fit.
    POOL_SIZE=CONSOLE_APPLICATION_POOL;
    uint64_t fit=image_size+heap_capacity+WONDER_VAMM_RESERVATION+WONDER_SYSTEM_RESOURCE_SIZE;
    if(POOL_SIZE>fit)POOL_SIZE=fit;
    alias_bits=calloc(ALIAS_SIZE>>15,1);alias_host=calloc(ALIAS_SIZE>>14,sizeof *alias_host);
    if(!alias_bits||!alias_host)die("alias bookkeeping allocation failed");
    arc4random_buf(entropy,sizeof entropy);
    LOG("SDK_KERNEL AS=%p+%#llx pool=%llu MiB heap_region=%llu MiB alias=%llu MiB (lazy commit)",space,(unsigned long long)AS_SIZE,
        (unsigned long long)(POOL_SIZE>>20),(unsigned long long)(heap_capacity>>20),(unsigned long long)(ALIAS_SIZE>>20));
}
#define TRANSFER_MEMORY_KIND 0x544d454du   /* "TMEM" */
struct transfer_memory { uint64_t src, size, mapped; };
static void free_transfer_memory(void *p) { free(p); }   // a live mapping stays until UnmapTransferMemory, as in Horizon
static int kernel_call_locked(int number, uint64_t *r) {
    if (!space) die("kernel used before initialization");
    switch (number) {
    case 1: {
        uint64_t size=r[1];
        if (size & 0x1fffff) {r[0]=0xca01;return 1;}
        if (size > AS_SIZE-HEAP_OFFSET || size > POOL_SIZE || image_size > POOL_SIZE-size) {r[0]=0x10801;return 1;}
        uint8_t *heap=space+HEAP_OFFSET;
        for (unsigned i=0;i<nregion;i++) if (size>heap_size && regions[i].state==0xe && regions[i].base<(uint64_t)heap+size && regions[i].base+regions[i].size>(uint64_t)heap+heap_size) {r[0]=0xd401;return 1;}
        if (size > heap_size && mprotect(heap+heap_size,size-heap_size,PROT_READ|PROT_WRITE)) {r[0]=0xce01;return 1;}
        if (size < heap_size) {
            // MADV_DONTNEED does not guarantee zero-fill on Darwin; replace the released mapping.
            if (mmap(heap+size,heap_size-size,PROT_NONE,MAP_PRIVATE|MAP_ANON|MAP_FIXED,-1,0)==MAP_FAILED) {r[0]=0xce01;return 1;}
        }
        heap_size=size;r[0]=0;r[1]=(uint64_t)heap;return 1;
    }
    case 0x2c: case 0x2d: {
        // Map/UnmapPhysicalMemory(addr, size) in the alias region, used by the SDK VammManager at 4 KiB granularity.
        // Horizon pages are 4 KiB, iOS pages 16 KiB: each 4 KiB page is tracked in a bitmap; a host page is
        // committed while any of its four Horizon pages is mapped and released (fresh, zero) when none is.
        // Newly mapped Horizon pages read as zero, as physical memory freshly mapped by the kernel does.
        uint64_t a=r[0],size=r[1],base=(uint64_t)space+ALIAS_OFFSET;
        if(!size||(a|size)&0xfff||a<base||size>ALIAS_SIZE||a-base>ALIAS_SIZE-size){r[0]=0xd401;return 1;}
        for(uint64_t p=a;p<a+size;p+=0x1000){
            uint64_t i=(p-base)>>12,host=(p-base)>>14;int mapped=alias_bits[i>>3]>>(i&7)&1;
            if(number==0x2c&&!mapped){
                if(!alias_host[host]&&mprotect((void *)(base+(host<<14)),0x4000,PROT_READ|PROT_WRITE)){r[0]=0xce01;return 1;}
                memset((void *)p,0,0x1000);alias_host[host]++;alias_mapped+=0x1000;alias_bits[i>>3]|=(uint8_t)(1u<<(i&7));
            } else if(number==0x2d&&mapped){
                alias_bits[i>>3]&=(uint8_t)~(1u<<(i&7));alias_mapped-=0x1000;
                if(!--alias_host[host]&&mmap((void *)(base+(host<<14)),0x4000,PROT_NONE,MAP_PRIVATE|MAP_ANON|MAP_FIXED,-1,0)==MAP_FAILED){r[0]=0xce01;return 1;}
            }
        }
        r[0]=0;return 1;
    }
    case 4: case 5: {
        // MapMemory / UnmapMemory(dst, src, size): the SDK moves a heap-allocated thread stack into the stack region.
        // Horizon aliases 4 KiB pages; iOS pages are 16 KiB, so a 4 KiB-granular alias is impossible. Host semantics:
        // move the contents in (Map) and back out (Unmap); Horizon makes src inaccessible meanwhile, so a correct
        // guest cannot observe the difference. dst pages are made accessible at 16 KiB granularity.
        uint64_t dst=r[0],src=r[1],size=r[2],sbase=(uint64_t)space+STACK_OFFSET,heap=(uint64_t)space+HEAP_OFFSET;
        if(!size||(dst|src|size)&0xfff||dst<sbase||size>STACK_SIZE||dst-sbase>STACK_SIZE-size||src<heap||src-heap>heap_size||size>heap_size-(src-heap)){r[0]=0xd401;return 1;}
        int found=-1;
        for(unsigned i=0;i<nregion;i++)if(dst<regions[i].base+regions[i].size&&regions[i].base<dst+size){
            if(number==5&&regions[i].base==dst&&regions[i].size==size&&regions[i].src==src&&regions[i].state==0xb)found=(int)i;
            else {r[0]=0xdc01;return 1;}   // invalid current memory state
        }
        if(number==4){
            if(found>=0||nregion==256){r[0]=0xdc01;return 1;}
            uint64_t lo=dst&~0x3fffull,hi=(dst+size+0x3fff)&~0x3fffull;
            if(mprotect((void *)lo,hi-lo,PROT_READ|PROT_WRITE)){r[0]=0xce01;return 1;}
            memcpy((void *)dst,(void *)src,size);
            regions[nregion++]=(typeof(regions[0])){dst,size,src,0xb,3};
            LOG("HOST_MAP_MEMORY dst=%#llx src=%#llx size=%#llx (move semantics; 16 KiB host pages)",(unsigned long long)dst,(unsigned long long)src,(unsigned long long)size);
        } else {
            if(found<0){r[0]=0xd401;return 1;}
            memcpy((void *)src,(void *)dst,size);
            regions[found]=regions[--nregion];   // pages stay accessible: a 16 KiB page may be shared with another stack
        }
        r[0]=0;return 1;
    }
    case 6: {
        uint64_t a=r[2],base=a&~0xfffull,size=0x1000;uint32_t state=0,perm=0;
        uint64_t lo=(uint64_t)space,hi=lo+AS_SIZE,h=lo+HEAP_OFFSET,abase=lo+ALIAS_OFFSET,aend=abase+ALIAS_SIZE;
        if (a>=abase && a<aend) {   // alias region: runs of equal 4 KiB state from the bitmap
            uint64_t i=(a-abase)>>12,n=ALIAS_SIZE>>12,b=i,e=i+1;int m=alias_bits[i>>3]>>(i&7)&1;
            while(b>0&&(alias_bits[(b-1)>>3]>>((b-1)&7)&1)==m)b--;
            while(e<n&&(alias_bits[e>>3]>>(e&7)&1)==m)e++;
            if(!r[0]){r[0]=0xd401;return 1;}
            uint32_t *info=(void *)r[0];memset(info,0,0x28);
            ((uint64_t *)info)[0]=abase+(b<<12);((uint64_t *)info)[1]=(e-b)<<12;info[4]=m?5:0;info[6]=m?3:0;
            r[0]=0;r[1]=0;return 1;
        }
        if (a>=h && a<h+heap_size) {base=h;size=heap_size;state=5;perm=3;}
        else if (a>=lo && a<hi) {
            if (a<h) hi=h; else lo=h+heap_size;
            if (a<abase && hi>abase) hi=abase;          // the alias region is reported separately above
            if (a>=aend && lo<aend && a<h) lo=aend;
            base=lo;size=hi-lo;
        } else {
            uint64_t top=(uint64_t)pthread_get_stackaddr_np(pthread_self()),sz=pthread_get_stacksize_np(pthread_self());
            if (a>=top-sz && a<top) {base=top-sz;size=sz;state=0xb;perm=3;}
            for (unsigned i=0;i<nregion;i++) if(a>=regions[i].base && a-regions[i].base<regions[i].size) {
                base=regions[i].base;size=regions[i].size;state=regions[i].state;perm=regions[i].perm;break;
            }
        }
        if (!state) {
            for(unsigned i=0;i<nregion;i++) {
                if(a>=regions[i].base && a-regions[i].base<regions[i].size) {
                    base=regions[i].base;size=regions[i].size;state=regions[i].state;perm=regions[i].perm;break;
                }
                if(a>=lo && a<hi) {
                    if(regions[i].base>a && regions[i].base<base+size)size=regions[i].base-base;
                    uint64_t end=regions[i].base+regions[i].size;
                    if(end<=a && end>base){size-=end-base;base=end;}
                }
            }
        }
        if (!r[0]) {r[0]=0xd401;return 1;}
        uint32_t *info=(void *)r[0];memset(info,0,0x28);
        ((uint64_t *)info)[0]=base;((uint64_t *)info)[1]=size;info[4]=state;info[6]=perm;
        r[0]=0;r[1]=0;return 1;
    }
    // Transfer memory (nn::os::CreateTransferMemory / MapTransferMemory; the in-process audio server maps the
    // renderer work buffer this way, sdk+0x136130). Horizon aliases the owner's pages at a new address while the
    // owner keeps the permission it chose; only MemoryPermission_None is supported, so the owner cannot observe
    // the pages and the host moves the contents (as MapMemory does, 16 KiB host pages). The SDK picks random
    // ASLR addresses and retries on 0xd401; Horizon forbids the heap and alias regions. On this host the heap
    // region is most of the address space, so its unused tail is also accepted (SetHeapSize then refuses to grow
    // over a transfer mapping).
    case 0x15: {
        uint64_t src=r[1],size=r[2],heap=(uint64_t)space+HEAP_OFFSET; uint32_t perm=(uint32_t)r[3];
        if(!size||(src|size)&0xfff){r[0]=0xca01;return 1;}
        if(src<heap||src-heap>heap_size||size>heap_size-(src-heap)){r[0]=0xd401;return 1;}
        if(perm!=0) die("SVC CreateTransferMemory owner permission %u unsupported (only None: host moves the pages)",perm);
        struct transfer_memory *t=calloc(1,sizeof *t);
        if(!t){r[0]=0xd201;return 1;}
        *t=(struct transfer_memory){src,size,0};
        uint32_t h=wonder_handle_new_object(TRANSFER_MEMORY_KIND,t,free_transfer_memory);
        if(!h){free(t);r[0]=0xd201;return 1;}
        LOG("HOST_TRANSFER_MEMORY create handle=%#x src=%#llx size=%#llx perm=%u",h,(unsigned long long)src,(unsigned long long)size,perm);
        r[0]=0;r[1]=h;return 1;
    }
    case 0x51: case 0x52: {
        uint32_t h=(uint32_t)r[0]; uint64_t dst=r[1],size=r[2];
        struct transfer_memory *t=wonder_handle_object(h,TRANSFER_MEMORY_KIND);
        if(!t){r[0]=0xe401;return 1;}
        if(size!=t->size||dst&0xfff){r[0]=0xca01;return 1;}
        uint64_t lo=(uint64_t)space,hi=lo+AS_SIZE,abase=lo+ALIAS_OFFSET,aend=abase+ALIAS_SIZE,heap=lo+HEAP_OFFSET;
        if(number==0x51){
            uint32_t perm=(uint32_t)r[3];
            if(perm!=1&&perm!=3){r[0]=0xd801;return 1;}
            if(t->mapped){r[0]=0xd401;return 1;}
            int bad=dst<lo||dst>=hi||size>hi-dst||(dst<aend&&dst+size>abase)||(dst<heap+heap_size&&dst+size>heap);
            for(unsigned i=0;!bad&&i<nregion;i++) if(dst<regions[i].base+regions[i].size&&regions[i].base<dst+size) bad=1;
            if(bad||nregion==256){r[0]=0xd401;return 1;}
            uint64_t plo=dst&~0x3fffull,phi=(dst+size+0x3fff)&~0x3fffull;
            if(mprotect((void *)plo,phi-plo,PROT_READ|PROT_WRITE)){r[0]=0xce01;return 1;}
            memcpy((void *)dst,(void *)t->src,size);
            regions[nregion++]=(typeof(regions[0])){dst,size,t->src,0xe,perm};   // MemoryState TransferMem
            t->mapped=dst;
            LOG("HOST_TRANSFER_MEMORY map handle=%#x dst=%#llx src=%#llx size=%#llx (move semantics)",h,(unsigned long long)dst,(unsigned long long)t->src,(unsigned long long)size);
        } else {
            int found=-1;
            for(unsigned i=0;i<nregion;i++) if(regions[i].base==dst&&regions[i].size==size&&regions[i].state==0xe&&regions[i].src==t->src){found=(int)i;break;}
            if(found<0||t->mapped!=dst){r[0]=0xd401;return 1;}
            memcpy((void *)t->src,(void *)dst,size);
            regions[found]=regions[--nregion]; t->mapped=0;
            // Release whole host pages no other region touches (fresh zero pages next time).
            for(uint64_t p=(dst+0x3fff)&~0x3fffull;p+0x4000<=dst+size;p+=0x4000){
                int shared=0; for(unsigned i=0;i<nregion;i++) if(p<regions[i].base+regions[i].size&&regions[i].base<p+0x4000) shared=1;
                if(!shared&&mmap((void *)p,0x4000,PROT_NONE,MAP_PRIVATE|MAP_ANON|MAP_FIXED,-1,0)==MAP_FAILED){r[0]=0xce01;return 1;}
            }
            LOG("HOST_TRANSFER_MEMORY unmap handle=%#x dst=%#llx",h,(unsigned long long)dst);
        }
        r[0]=0;return 1;
    }
    // Thread SVCs (0x08-0x10, 0x25, synchronization) live in kernel_threads.c.
    case 0x29: {
        uint64_t id=r[1],v=0;
        switch(id) {
        case 0:v=7;break; // process core mask: application cores 0-2 (kernel_threads.c)
        case 1:v=UINT64_MAX;break;
        case 2:v=(uint64_t)space+ALIAS_OFFSET;break;
        case 3:v=ALIAS_SIZE;break;
        case 4:v=(uint64_t)space+HEAP_OFFSET;break;
        case 5:v=AS_SIZE-HEAP_OFFSET;break;
        case 6:v=POOL_SIZE;break;
        case 21:v=POOL_SIZE-WONDER_SYSTEM_RESOURCE_SIZE;break;          // non-system: excludes the system resource
        case 7:case 22:v=image_size+heap_size+alias_mapped;break;        // VAMM-mapped pages count as used memory
        case 8:v=0;break;
        case 11:if(r[3]>3) {r[0]=0xf001;return 1;}v=entropy[r[3]];break;
        case 12:v=(uint64_t)space;break;
        case 13:v=AS_SIZE;break;
        case 14:v=(uint64_t)space+STACK_OFFSET;break;
        case 15:v=STACK_SIZE;break;
        // NPDM system_resource_size (META+0x14) = 16 MiB, checked by build_loader_probe.py. With 0 the SDK leaves
        // VammManager uninitialized and nn::os::AllocateAddressRegion dereferences NULL (nvn-round9, sdk+0x134894).
        case 16:v=WONDER_SYSTEM_RESOURCE_SIZE;break;
        case 17:v=0;break; // nothing charged yet: page tables live in host memory
        case 18:v=0x010015100b514000ull;break;
        case 20:r[0]=0xf001;return 1; // User-exception context not implemented.
        case 23:v=1;break;
        default:r[0]=0xf001;return 1;
        }
        r[0]=0;r[1]=v;return 1;
    }
    default:return 0;
    }
}

// Serialize metadata and the host VM operation as one transaction. In particular, a new 4 KiB
// mapping cannot race the final 16 KiB reset for its neighbour and lose its physical backing.
int wonder_kernel_call(int number, uint64_t *r) {
    pthread_mutex_lock(&kernel_lock);
    int handled = kernel_call_locked(number, r);
    pthread_mutex_unlock(&kernel_lock);
    return handled;
}
