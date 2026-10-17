#include "command_store.h"
#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
int main(void) {
    uint32_t s1, s2, s3; unsigned op; size_t n;
    assert(!wonder_cmdstore_put(0x1000, 7, "abcdef", 6, &s1));
    char *p = wonder_cmdstore_get(0x1000, s1, &op, &n); assert(p && op == 7 && n == 6 && !memcmp(p, "abcdef", 6)); free(p);
    // Re-recording the same address replaces the record; the old sequence number no longer resolves.
    assert(!wonder_cmdstore_put(0x1000, 9, "xy", 2, &s2) && s2 != s1);
    assert(!wonder_cmdstore_get(0x1000, s1, &op, &n));
    p = wonder_cmdstore_get(0x1000, s2, &op, &n); assert(p && op == 9 && n == 2); free(p);
    assert(!wonder_cmdstore_get(0x2000, s2, &op, &n));               // no record there
    assert(wonder_cmdstore_put(0, 1, "", 0, &s3) == -1);            // address 0 is never a stub
    // Many records (growth), then a pool range is forgotten and the rest survive.
    for (uint64_t a = 0x100000; a < 0x100000 + 8 * 5000; a += 8) assert(!wonder_cmdstore_put(a, 1, &a, 8, &s3));
    assert(wonder_cmdstore_count() == 5001);
    wonder_cmdstore_forget_range(0x100000, 8 * 2500);
    assert(wonder_cmdstore_count() == 2501);
    assert(!wonder_cmdstore_get(0x100000, s3, &op, &n));
    p = wonder_cmdstore_get(0x100000 + 8 * 4999, s3, &op, &n); assert(p); free(p);   // last record kept
    p = wonder_cmdstore_get(0x1000, s2, &op, &n); assert(p); free(p);
    {   // round232: copy into the caller's buffer when it fits, else a malloc'd copy
        uint32_t seq2; char big[3000]; memset(big, 7, sizeof big);
        assert(!wonder_cmdstore_put(0x9000, 5, "abc", 3, &seq2));
        char buf[16]; unsigned op = 0; size_t n = 0; int alloc = -1;
        void *p = wonder_cmdstore_get_into(0x9000, seq2, &op, &n, buf, sizeof buf, &alloc);
        assert(p == buf && !alloc && op == 5 && n == 3 && !memcmp(buf, "abc", 3));
        assert(!wonder_cmdstore_put(0x9008, 6, big, sizeof big, &seq2));
        p = wonder_cmdstore_get_into(0x9008, seq2, &op, &n, buf, sizeof buf, &alloc);
        assert(p && p != buf && alloc == 1 && n == sizeof big && !memcmp(p, big, sizeof big)); free(p);
        assert(!wonder_cmdstore_get_into(0x9008, seq2 + 1, &op, &n, buf, sizeof buf, &alloc));   // stale sequence
    }
    puts("COMMAND_STORE_OK");
}
