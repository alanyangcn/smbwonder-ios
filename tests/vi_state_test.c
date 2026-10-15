#include "vi_state.h"
#include <assert.h>
#include <stdio.h>
int main(void){void *display=NULL,*layer=NULL,*native=NULL;
 assert(wonder_vi_open(&display));assert(wonder_vi_initialize(NULL,100,100));
 assert(!wonder_vi_initialize((void*)0x123456,1280,720));assert(!wonder_vi_open(&display));assert(wonder_vi_valid_display(display));
 assert(wonder_vi_create(&layer,(void*)0x111));assert(!wonder_vi_create(&layer,display));assert(wonder_vi_create(&native,display));
 assert(wonder_vi_native(&native,(void*)0x111));assert(!wonder_vi_native(&native,layer));assert(native==(void*)0x123456);
 puts("VI_OBJECT_LIFECYCLE_OK");}
