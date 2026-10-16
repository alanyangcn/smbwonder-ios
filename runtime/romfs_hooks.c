#include "page_guard.h"
#include "hle.h"
#include "romfs.h"
#include "savedata.h"
#include <errno.h>
_Noreturn void wonder_game_thread_stop(const char *);
// nn::fs file APIs shared by the read-only RomFS mount and host save data: paths are routed by mount name and
// handles by slot (save data slots start at WONDER_SAVE_SLOT_BASE; RomFS uses 1..255).
HLE_FORCE("_ZN2nn2fs22QueryMountRomCacheSizeEPm","Host indexed RomFS requires no guest cache",Result,wonder_cache,size_t *out){if(!out)return RES_FAIL;*out=0;LOG("ROMFS_CACHE size=0 (host index)");return 0;}
HLE_FORCE("_ZN2nn2fs8MountRomEPKcPvm","Mount actual indexed read-only game archive",Result,wonder_mount,const char *name,void *cache,size_t size){
    (void)cache;(void)size;if(wonder_save_owns_mount(name))return FS_MOUNT_NAME_ALREADY_EXISTS;
    Result r=wonder_romfs_mount(name);LOG("ROMFS_MOUNT name=%s result=%#x",name,r);
    if(r)wonder_game_thread_stop("RomFS archive missing or mount invalid; no empty resource mount supplied");return r;
}
HLE_FORCE("_ZN2nn2fs8MountRomEPKc","Mount actual indexed read-only game archive",Result,wonder_mount_simple,const char *name){return wonder_mount(name,NULL,0);}
HLE_FORCE("_ZN2nn2fs8OpenFileEPNS0_10FileHandleEPKci","Open host save data files or read-only indexed RomFS; reject unknown paths",Result,wonder_open,uint64_t *out,const char *path,int mode){
    Result r;
    if(wonder_save_owns_path(path)){r=wonder_save_open(out,path,mode);LOG("SAVE_OPEN path=%s mode=%d result=%#x",path,mode,r);return r;}
    r=wonder_romfs_open(out,path,mode);LOG("ROMFS_OPEN path=%s mode=%d result=%#x handle=%#llx",path,mode,r,(unsigned long long)(r?0:*out));return r;
}
static Result read_file(size_t *out,uint64_t h,int64_t offset,void *buf,size_t size){
    wonder_guard_release(buf,size);   // pread into a write-watched page would fail with EFAULT (page_guard.h)
    if(wonder_save_owns_handle(h))return wonder_save_read(out,h,offset,buf,size);
    Result r=wonder_romfs_read(out,h,offset,buf,size);LOG("ROMFS_READ h=%#llx offset=%#llx request=%zu actual=%zu result=%#x",(unsigned long long)h,(unsigned long long)offset,size,r?0:*out,r);return r;
}
HLE_FORCE("_ZN2nn2fs8ReadFileEPmNS0_10FileHandleElPvm","Read actual file bytes with short-read output",Result,wonder_read,size_t *out,uint64_t h,int64_t offset,void *buf,size_t size){return read_file(out,h,offset,buf,size);}
HLE_FORCE("_ZN2nn2fs8ReadFileEPmNS0_10FileHandleElPvmRKNS0_10ReadOptionE","Read actual file bytes with validated option",Result,wonder_read_option,size_t *out,uint64_t h,int64_t offset,void *buf,size_t size,const uint32_t *option){if(!option||*option)return RES_FAIL;return read_file(out,h,offset,buf,size);}
HLE_FORCE("_ZN2nn2fs8ReadFileENS0_10FileHandleElPvm","Read exact requested bytes or report short read",Result,wonder_read_exact,uint64_t h,int64_t offset,void *buf,size_t size){size_t got=0;Result r=read_file(&got,h,offset,buf,size);return r?r:got==size?0:RES_FAIL;}
HLE_FORCE("_ZN2nn2fs8ReadFileENS0_10FileHandleElPvmRKNS0_10ReadOptionE","Read exact requested bytes with validated option",Result,wonder_read_exact_option,uint64_t h,int64_t offset,void *buf,size_t size,const uint32_t *option){if(!option||*option)return RES_FAIL;return wonder_read_exact(h,offset,buf,size);}
HLE_FORCE("_ZN2nn2fs11GetFileSizeEPlNS0_10FileHandleE","Report actual file size",Result,wonder_size,int64_t *out,uint64_t h){return wonder_save_owns_handle(h)?wonder_save_size(out,h):wonder_romfs_size(out,h);}
HLE_FORCE("_ZN2nn2fs9CloseFileENS0_10FileHandleE","Invalidate generation-bound file handle",void,wonder_close,uint64_t h){
    if(wonder_save_owns_handle(h)){wonder_save_close(h);return;}
    if(wonder_romfs_close(h))die("Invalid RomFS close %#llx",h);
}
HLE_FORCE("_ZN2nn2fs12GetEntryTypeEPNS0_18DirectoryEntryTypeEPKc","Case-sensitive file/directory query in save data or archive",Result,wonder_type,int32_t *out,const char *path){
    return wonder_save_owns_path(path)?wonder_save_entry_type(out,path):wonder_romfs_type(out,path);
}
