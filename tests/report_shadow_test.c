#include "report_shadow.h"
#include <assert.h>
#include <stdio.h>
int main(void) {
    static struct wonder_report_shadow s; uint32_t v = 0;
    wonder_report_shadow_clear(&s);
    assert(wonder_report_shadow_lookup(&s, 0x1000, &v) == 0);                                   // nothing pending
    wonder_report_shadow_value(&s, 0x1000, 7); wonder_report_shadow_value(&s, 0x1010, 9);
    assert(wonder_report_shadow_lookup(&s, 0x1000, &v) == 1 && v == 7);
    assert(wonder_report_shadow_lookup(&s, 0x1010, &v) == 1 && v == 9);
    wonder_report_shadow_value(&s, 0x1000, 8); assert(wonder_report_shadow_lookup(&s, 0x1000, &v) == 1 && v == 8);   // latest wins
    assert(wonder_report_shadow_lookup(&s, 0x1004, &v) == 0);                                   // the high word is not written
    wonder_report_shadow_unknown(&s, 0x1008, 16);                                              // a timestamp record over 0x1010
    assert(wonder_report_shadow_lookup(&s, 0x1010, &v) == -1 && wonder_report_shadow_lookup(&s, 0x1000, &v) == 1);
    wonder_report_shadow_value(&s, 0x1010, 3); assert(wonder_report_shadow_lookup(&s, 0x1010, &v) == 1 && v == 3);   // rewritten after
    wonder_report_shadow_value(&s, 0x1002, 1); assert(wonder_report_shadow_lookup(&s, 0x1000, &v) == -1);            // partial overlap
    wonder_report_shadow_clear(&s); assert(wonder_report_shadow_lookup(&s, 0x1000, &v) == 0);
    for (int i = 0; i < WONDER_REPORT_SHADOW_SLOTS + 1; i++) wonder_report_shadow_value(&s, 0x2000 + 16 * (uint64_t)i, (uint32_t)i);
    assert(wonder_report_shadow_lookup(&s, 0x9000000, &v) == -1);                               // overflow: nothing is trusted
    puts("report_shadow OK");
    return 0;
}
