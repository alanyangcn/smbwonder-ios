#pragma once
#include <stdint.h>
// EL0 system-register reads that iOS traps (SIGILL) but Horizon allows. Only exact `mrs Xt, CTR_EL0` is emulated:
// three sdk sites read it to size cache-maintenance loops (os::FlushDataCache sdk+0x12a964, VammManager sdk+0x1359e4,
// __clear_cache sdk+0x57ca54). The value is the Tegra X1 (Cortex-A57) CTR_EL0: 64-byte minimum I/D cache lines,
// PIPT I-cache. A stride no larger than the host's line is always safe for `dc civac` / `ic ivau` loops.
#define WONDER_GUEST_CTR_EL0 0x8444c004ull
// Returns the destination register (0..31; 31 = xzr, write discarded) or -1 when the word is not emulated.
static inline int wonder_sysreg_read_emulated(uint32_t word, uint64_t *value) {
    if ((word & 0xFFFFFFE0u) != 0xD53B0020u) return -1;   // mrs Xt, CTR_EL0 (op0=3 op1=3 CRn=0 CRm=0 op2=1)
    *value = WONDER_GUEST_CTR_EL0;
    return (int)(word & 31);
}
