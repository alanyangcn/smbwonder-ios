// mrs CTR_EL0 emulation: decode of the three sdk words, and rejection of every neighbouring system register.
#include "sysreg_emul.h"
#include <assert.h>
#include <stdio.h>
int main(void) {
    uint64_t v = 0;
    assert(wonder_sysreg_read_emulated(0xd53b0029u, &v) == 9 && v == WONDER_GUEST_CTR_EL0);   // sdk+0x12a964 mrs x9, ctr_el0
    assert(((v >> 16) & 15) == 4 && (v & 15) == 4);                                            // DminLine = IminLine = 64 bytes
    assert(wonder_sysreg_read_emulated(0xd53b003fu, &v) == 31);                                // xzr destination
    assert(wonder_sysreg_read_emulated(0xd53b00e9u, &v) == -1);   // DCZID_EL0
    assert(wonder_sysreg_read_emulated(0xd53be029u, &v) == -1);   // CNTPCT_EL0 (patched as a gate)
    assert(wonder_sysreg_read_emulated(0xd53bd049u, &v) == -1);   // TPIDR_EL0
    assert(wonder_sysreg_read_emulated(0xd51b0029u, &v) == -1);   // msr ctr_el0, x9 (write)
    assert(wonder_sysreg_read_emulated(0x00000000u, &v) == -1);   // udf
    puts("sysreg_emul OK");
    return 0;
}
