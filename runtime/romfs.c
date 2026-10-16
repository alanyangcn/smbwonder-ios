// Case-sensitive indexed RomFS; only pread accesses game bytes, never writable mappings.
#include "romfs.h"
#include <fcntl.h>
#include <unistd.h>
#include <sys/stat.h>
#include <pthread.h>
#include <stdlib.h>
#include <string.h>
#include <limits.h>
#include <errno.h>
#define NOT_FOUND 0x202u
#define INVALID 0x2a2u
#define DENIED 0x6402u
struct wonder_romfs_entry {char *name;uint64_t size,offset;};
static struct wonder_romfs_entry *entries;static size_t count;static int archive_fd=-1;
static char mount_name[32];
static struct {uint32_t generation;const struct wonder_romfs_entry *entry;} handles[256];
static pthread_mutex_t lock=PTHREAD_MUTEX_INITIALIZER;
static int read_exact(int fd,void *out,size_t size,uint64_t offset){
    while(size){ssize_t n=pread(fd,out,size,(off_t)offset);if(n<0&&errno==EINTR)continue;if(n<=0)return -1;out=(char*)out+n;size-=n;offset+=n;}return 0;
}
static int valid(const char *p){
    if(!p||!*p||strlen(p)>0x300)return 0;
    for(const char *s=p;;){const char *e=strchr(s,'/');size_t n=e?(size_t)(e-s):strlen(s);
        if(!n||(n==1&&s[0]=='.')||(n==2&&!memcmp(s,"..",2)))return 0;
        for(size_t i=0;i<n;i++)if(s[i]==':'||s[i]=='\\')return 0;
        if(!e)return 1;s=e+1;
    }
}
int wonder_romfs_configure(const char *path){
    if(archive_fd>=0)return EALREADY;
    int fd=open(path,O_RDONLY|O_NOFOLLOW);if(fd<0)return errno;
    struct stat st;uint64_t h[4];struct wonder_romfs_entry *table=NULL;unsigned char *index=NULL;size_t built=0;int error=EINVAL;
    if(fstat(fd,&st)||!S_ISREG(st.st_mode)||read_exact(fd,h,sizeof h,0)||memcmp(h,"WONDER01",8))goto fail;
    if(h[1]>100000||h[2]>(16u<<20)||st.st_size<32||h[2]>(uint64_t)st.st_size-32||h[3]!=(uint64_t)st.st_size-32-h[2])goto fail;
    table=calloc(h[1]?h[1]:1,sizeof *table);index=malloc(h[2]?h[2]:1);if(!table||!index){error=ENOMEM;goto fail;}
    if(read_exact(fd,index,h[2],32))goto fail;
    uint64_t pos=0,offset=32+h[2];
    for(uint64_t i=0;i<h[1];i++){
        if(h[2]-pos<10)goto fail;
        uint16_t len;uint64_t size;memcpy(&len,index+pos,2);memcpy(&size,index+pos+2,8);pos+=10;
        if(!len||len>0x300||len>h[2]-pos||size>(uint64_t)st.st_size-offset||memchr(index+pos,0,len))goto fail;
        table[i].name=calloc((size_t)len+1,1);if(!table[i].name){error=ENOMEM;goto fail;}built++;
        memcpy(table[i].name,index+pos,len);pos+=len;
        if(!valid(table[i].name)||(i&&strcmp(table[i-1].name,table[i].name)>=0))goto fail;
        table[i].size=size;table[i].offset=offset;offset+=size;
    }
    if(pos!=h[2]||offset!=(uint64_t)st.st_size)goto fail;
    free(index);entries=table;count=h[1];archive_fd=fd;return 0;
fail:
    for(size_t i=0;i<built;i++)free(table[i].name);
    free(table);free(index);close(fd);return error;
}
uint32_t wonder_romfs_mount(const char *name){
    if(!name||!*name||strlen(name)>=sizeof mount_name||strpbrk(name,"/:\\"))return INVALID;
    pthread_mutex_lock(&lock);uint32_t result=0;
    if(archive_fd<0)result=NOT_FOUND;
    else if(mount_name[0]&&strcmp(mount_name,name))result=INVALID;
    else strcpy(mount_name,name);
    pthread_mutex_unlock(&lock);return result;
}
// nn::fs normalizes paths before the file system sees them: runs of separators collapse to one (round141: the sound
// engine opens "content://Sound/Resource/Stream/*.bwav", every stream failed with 0x202). Callers hold the lock.
static char normalized[0x301];
static const char *relative(const char *path){
    if(!path||!mount_name[0])return NULL;size_t n=strlen(mount_name);
    if(strncmp(path,mount_name,n)||strncmp(path+n,":/",2))return NULL;
    const char *s=path+n+1;size_t o=0;
    while(*s){
        if(*s=='/'){while(*s=='/')s++;if(*s&&o)normalized[o++]='/';continue;}
        if(o>=sizeof normalized-1)return NULL;
        normalized[o++]=*s++;
    }
    normalized[o]=0;
    return valid(normalized)?normalized:NULL;
}
static const struct wonder_romfs_entry *find(const char *name){
    size_t lo=0,hi=count;while(lo<hi){size_t m=lo+(hi-lo)/2;int c=strcmp(entries[m].name,name);if(c<0)lo=m+1;else hi=m;}
    return lo<count&&!strcmp(entries[lo].name,name)?&entries[lo]:NULL;
}
uint32_t wonder_romfs_open(uint64_t *out,const char *path,int mode){
    if(!out)return INVALID;if(mode!=1)return DENIED;
    pthread_mutex_lock(&lock);const char *name=relative(path);const struct wonder_romfs_entry *e=name?find(name):NULL;uint32_t result=NOT_FOUND;
    if(e){result=INVALID;for(unsigned i=1;i<256;i++)if(!handles[i].entry){
        if(++handles[i].generation==0)++handles[i].generation;
        handles[i].entry=e;*out=((uint64_t)handles[i].generation<<32)|i;result=0;break;
    }}pthread_mutex_unlock(&lock);return result;
}
static const struct wonder_romfs_entry *handle(uint64_t h){unsigned slot=(uint32_t)h;return slot>0&&slot<256&&handles[slot].generation==(uint32_t)(h>>32)?handles[slot].entry:NULL;}
uint32_t wonder_romfs_size(int64_t *out,uint64_t h){
    if(!out)return INVALID;pthread_mutex_lock(&lock);const struct wonder_romfs_entry *e=handle(h);if(e)*out=e->size;pthread_mutex_unlock(&lock);return e?0:INVALID;
}
uint32_t wonder_romfs_read(size_t *out,uint64_t h,int64_t offset,void *buf,size_t size){
    if(!out||offset<0||(!buf&&size))return INVALID;
    pthread_mutex_lock(&lock);const struct wonder_romfs_entry *e=handle(h);pthread_mutex_unlock(&lock);if(!e)return INVALID;
    uint64_t avail=(uint64_t)offset<e->size?e->size-(uint64_t)offset:0;size_t n=size<avail?size:(size_t)avail;
    if(read_exact(archive_fd,buf,n,e->offset+(uint64_t)offset))return INVALID;*out=n;return 0;
}
uint32_t wonder_romfs_close(uint64_t h){
    pthread_mutex_lock(&lock);const struct wonder_romfs_entry *e=handle(h);if(e)handles[(uint32_t)h].entry=NULL;pthread_mutex_unlock(&lock);return e?0:INVALID;
}
uint32_t wonder_romfs_type(int32_t *out,const char *path){
    if(!out)return INVALID;pthread_mutex_lock(&lock);const char *name=relative(path);uint32_t result=NOT_FOUND;
    if(name){if(find(name)){*out=1;result=0;}else {size_t n=strlen(name);for(size_t i=0;i<count;i++)if(!strncmp(entries[i].name,name,n)&&entries[i].name[n]=='/'){*out=0;result=0;break;}}}
    pthread_mutex_unlock(&lock);return result;
}
