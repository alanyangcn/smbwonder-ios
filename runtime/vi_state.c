#include "vi_state.h"
#include <pthread.h>
#include <errno.h>
static pthread_mutex_t lock=PTHREAD_MUTEX_INITIALIZER;
static struct {unsigned width,height;} display;
static struct {void *native;int created;} layer;
int wonder_vi_initialize(void *native,unsigned width,unsigned height){
 if(!native||!width||!height)return EINVAL;pthread_mutex_lock(&lock);
 int error=layer.native&&layer.native!=native?EBUSY:0;
 if(!error){layer.native=native;display.width=width;display.height=height;}pthread_mutex_unlock(&lock);return error;
}
int wonder_vi_valid_display(void *p){pthread_mutex_lock(&lock);int valid=p==&display&&layer.native!=0;pthread_mutex_unlock(&lock);return valid;}
uint32_t wonder_vi_open(void **out){if(!out)return 0xd401;pthread_mutex_lock(&lock);uint32_t r=layer.native?0:0xfa01;if(!r)*out=&display;pthread_mutex_unlock(&lock);return r;}
uint32_t wonder_vi_create(void **out,void *p){if(!out)return 0xd401;pthread_mutex_lock(&lock);uint32_t r=p!=&display||!layer.native?0xe401:layer.created?0xce01:0;if(!r){layer.created=1;*out=&layer;}pthread_mutex_unlock(&lock);return r;}
uint32_t wonder_vi_native(void **out,void *p){if(!out)return 0xd401;pthread_mutex_lock(&lock);uint32_t r=p==&layer&&layer.created?0:0xe401;if(!r)*out=layer.native;pthread_mutex_unlock(&lock);return r;}
