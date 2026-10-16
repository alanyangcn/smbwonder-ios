#include "romfs.h"
#include <assert.h>
#include <string.h>
#include <stdio.h>
int main(int argc,char **argv){
    int error=wonder_romfs_configure(argv[1]);if(argc>2){assert(error);return 0;}assert(!error);
    uint64_t h=0,old;size_t n=999;int64_t size=-1;char b[20]={0};int32_t type=-1;
    assert(wonder_romfs_open(&h,"rom:/Folder/hello.bin",1)); // Not mounted.
    assert(!wonder_romfs_mount("rom"));assert(wonder_romfs_mount("bad/name"));
    assert(!wonder_romfs_open(&h,"rom:/Folder/hello.bin",1));old=h;
    assert(!wonder_romfs_size(&size,h)&&size==10);
    assert(!wonder_romfs_read(&n,h,3,b,20)&&n==7&&!memcmp(b,"3456789",7));
    assert(!wonder_romfs_read(&n,h,10,b,1)&&n==0);assert(wonder_romfs_read(&n,h,-1,b,1));
    { uint64_t h2; assert(!wonder_romfs_open(&h2,"rom://Folder//hello.bin",1)&&!wonder_romfs_close(h2)); }   // nn::fs collapses separators (round141)
    assert(wonder_romfs_open(&h,"rom://Folder/../empty",1));
    assert(wonder_romfs_open(&h,"rom:/Folder/../empty",1));assert(wonder_romfs_open(&h,"rom:/folder/hello.bin",1));
    assert(wonder_romfs_open(&h,"rom:/empty",2));assert(!wonder_romfs_close(old));
    assert(wonder_romfs_size(&size,old));assert(!wonder_romfs_open(&h,"rom:/empty",1));assert(h!=old);
    assert(!wonder_romfs_size(&size,h)&&size==0);assert(wonder_romfs_close(old));
    assert(!wonder_romfs_type(&type,"rom:/Folder")&&type==0);
    assert(!wonder_romfs_type(&type,"rom:/empty")&&type==1);
    assert(wonder_romfs_type(&type,"rom:/Missing"));assert(!wonder_romfs_close(h));
    puts("ROMFS_TEST_OK");
}
