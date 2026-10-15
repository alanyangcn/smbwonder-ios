#pragma once
#include <stdint.h>
#include <stddef.h>
int wonder_romfs_configure(const char *archive);
uint32_t wonder_romfs_mount(const char *name);
uint32_t wonder_romfs_open(uint64_t *,const char *,int);
uint32_t wonder_romfs_read(size_t *,uint64_t,int64_t,void *,size_t);
uint32_t wonder_romfs_size(int64_t *,uint64_t);
uint32_t wonder_romfs_close(uint64_t);
uint32_t wonder_romfs_type(int32_t *,const char *);
