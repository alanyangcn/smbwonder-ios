#import <UIKit/UIKit.h>
UIView *wonder_graphics_surface(void);
int wonder_host_driver_start(void *,unsigned long);
int wonder_host_driver_ready(void);
void *wonder_host_native_layer(void);
int wonder_host_register_vsync(uint32_t);
