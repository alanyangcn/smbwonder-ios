#include <assert.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
extern uint64_t wonder_language_code(const char *);
extern int wonder_applet_enable(unsigned,int);
extern void wonder_applet_focus(unsigned);
extern unsigned wonder_applet_get_focus(void);
extern int wonder_applet_pop(uint32_t *);
int main(void) {
    uint32_t message=123;assert(!wonder_applet_pop(&message));assert(message==123);
    assert(wonder_applet_enable(999,1)!=0);
    wonder_applet_focus(3);assert(wonder_applet_get_focus()==3);
    assert(wonder_applet_pop(&message)&&message==15);assert(!wonder_applet_pop(&message));
    wonder_applet_focus(1);assert(wonder_applet_pop(&message)&&message==15);assert(!wonder_applet_pop(&message));
    assert(!wonder_applet_enable(16,1));
    wonder_applet_focus(3);assert(wonder_applet_pop(&message)&&message==15);
    wonder_applet_focus(1);assert(wonder_applet_pop(&message)&&message==15);
    assert(wonder_applet_pop(&message)&&message==16);assert(!wonder_applet_pop(&message));
    wonder_applet_enable(16,0);wonder_applet_focus(3);wonder_applet_focus(1);
    assert(wonder_applet_pop(&message)&&message==15);assert(!wonder_applet_pop(&message));
    uint64_t code=wonder_language_code("zh-Hans-CN");assert(!memcmp(&code,"zh-Hans",8));
    code=wonder_language_code("en-GB");assert(!memcmp(&code,"en-GB",6));
    code=wonder_language_code("zh-TW");assert(!memcmp(&code,"zh-Hant",8));
    code=wonder_language_code("ja-JP");assert(!memcmp(&code,"ja",3));
    code=wonder_language_code("unknown");assert(!memcmp(&code,"en-US",6));
    puts("APPLET_NOTIFICATION_TEST_OK");
}
