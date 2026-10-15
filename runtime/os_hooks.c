// nn::os functions whose guest code uses EL0 operations iOS traps.
// os::FlushDataCache (sdk+0x12a960) reads CTR_EL0 to size its `dc civac` loop; iOS raises SIGILL on that read
// (round50, called from audio::EffectManager::Add). sdk callers reach it through the PLT (jmprel sdk+0xd3c5b0), so the
// host performs the same clean+invalidate to the point of coherency with sys_dcache_flush. The two remaining CTR_EL0
// readers (VammManager, __clear_cache) are emulated in the SIGILL handler (sysreg_emul.h).
#include "hle.h"
#include <libkern/OSCacheControl.h>
#include <stdatomic.h>
HLE_FORCE("_ZN2nn2os14FlushDataCacheEPKvm", "Data cache clean+invalidate by the host (guest loop reads CTR_EL0, which iOS traps)",
          void, h_os_flush_data_cache, const void *address, size_t size) {
    if (!size) return;   // the guest returns before touching the cache for size 0 (sdk+0x12a960 cbz x1)
    sys_dcache_flush((void *)address, size);
    static _Atomic int logged;
    if (!atomic_exchange(&logged, 1)) LOG("HOST_OS_FLUSH_DATA_CACHE first address=%p size=%#zx (sys_dcache_flush)", address, size);
}
