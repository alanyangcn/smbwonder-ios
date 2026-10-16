#include "log_throttle.h"
#include <assert.h>
#include <stdio.h>
int main(void) {
    unsigned printed = 0; unsigned long c = 0;
    for (unsigned i = 1; i <= 5000; i++) printed += wonder_log_throttle("NVN_CALL nvnQueueSubmit queue=0x1 submit=7", &c);
    assert(c == 5000 && printed == WONDER_LOG_FIRST + 5);   // 200 in full, then 1000, 2000, ..., 5000
    // The key is the first two words: another NVN_CALL name has its own budget, arguments do not matter.
    assert(wonder_log_throttle("NVN_CALL nvnWindowAcquireTexture window=0x2", &c) && c == 1);
    assert(wonder_log_throttle("NVN_CALL nvnQueueSubmit queue=0x9", &c) == 0 && c == 5001);
    assert(wonder_log_throttle("SINGLEWORD", &c) && c == 1 && wonder_log_throttle("SINGLEWORD", &c) && c == 2);
    puts("LOG_THROTTLE_OK");
}
