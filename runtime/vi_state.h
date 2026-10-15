#pragma once
#include <stdint.h>
int wonder_vi_initialize(void *,unsigned,unsigned);
uint32_t wonder_vi_open(void **);
uint32_t wonder_vi_create(void **,void *);
uint32_t wonder_vi_native(void **,void *);
int wonder_vi_valid_display(void *);
