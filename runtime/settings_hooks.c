// nn::settings (set:) on the offline host. See settings_service.h.
// Every nn::settings::detail call creates a proxy through CreateSettingsServerProxyForIpc (sdk+0x3b9950), which would
// connect to sm:. nn::settings::operator==(Language, LanguageCode) reaches it through detail::MakeLanguageCode
// (sdk+0x3b7e00), whose first call caches GetAvailableLanguageCodes2 (round48, caller main+0xf9d0c8).
#include "hle.h"
#include "settings_service.h"
#include <stdarg.h>
#include <stdatomic.h>
#include <stdio.h>
_Noreturn void wonder_settings_fatal(const char *format, ...) {
    char text[256]; va_list ap; va_start(ap, format); vsnprintf(text, sizeof text, format, ap); va_end(ap);
    die("%s", text);
    __builtin_unreachable();
}
extern uint64_t wonder_host_language(void);
uint64_t wonder_settings_system_language(void) { return wonder_host_language(); }
HLE_FORCE("_ZN2nn8settings6detail31CreateSettingsServerProxyForIpcEPNS_2sf13SharedPointerINS0_15ISettingsServerEEE",
          "Host set: ISettingsServer behind the native settings client (language tables, iOS system language)",
          uint32_t, h_settings_proxy, void **out) {
    if (!out) die("settings CreateSettingsServerProxyForIpc out=NULL");
    *out = wonder_settings_create();
    static _Atomic int logged;
    if (!atomic_exchange(&logged, 1)) LOG("HOST_SETTINGS_SERVICE created (%d languages; region/key maps/nickname stop by name)", WONDER_SETTINGS_LANGUAGES);
    return 0;
}
