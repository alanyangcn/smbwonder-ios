// nn::ngc (profanity filter) and nn::settings language on the offline host. See ngc_service.h.
// ProfanityFilter::Initialize (sdk+0x3863a8) gets the shared service through detail::CreateServiceByHipc, and every
// Check/Mask asks detail::GetLanguageSetting (sdk+0x3878b0), which calls settings::GetLanguageCode and
// oe::GetDesiredLanguage. Both service connections would go to sm:; the host answers them here.
#include "hle.h"
#include "ngc_service.h"
#include <stdarg.h>
#include <stdatomic.h>
#include <stdio.h>
_Noreturn void wonder_ngc_fatal(const char *format, ...) {
    char text[256]; va_list ap; va_start(ap, format); vsnprintf(text, sizeof text, format, ap); va_end(ap);
    die("%s", text);
    __builtin_unreachable();
}
void wonder_ngc_observe(const char *call, uint64_t count, uint32_t language_mask) {
    LOG("HOST_NGC_%s count=%llu language_mask=%#x matched=0 (no NG-word database)", call, (unsigned long long)count, language_mask);
}
// CreateServiceByHipc returns nn::sf::SharedPointer<IService> through x8 (sdk+0x387284 / +0x387304), which a C
// function cannot express, so the forced entry is a trampoline that stores the host object into [x8].
void *wonder_ngc_create_logged(void);
void *wonder_ngc_create_logged(void) {
    struct wonder_ngc_service *service = wonder_ngc_create();
    LOG("HOST_NGC_SERVICE created=%llu (offline: no NG-word database, nothing is matched or masked)",
        (unsigned long long)wonder_ngc_counts().created);
    return service;
}
__attribute__((naked)) static void ngc_create_trampoline(void) {
    __asm__ volatile(
        "stp x29, x30, [sp, #-32]!\n"
        "mov x29, sp\n"
        "str x8, [sp, #16]\n"
        "bl _wonder_ngc_create_logged\n"
        "ldr x8, [sp, #16]\n"
        "str x0, [x8]\n"
        "ldp x29, x30, [sp], #32\n"
        "ret\n");
}
__attribute__((used, section("__DATA,swforce"))) static const hle_force_ent hle_force_ngc_service = {
    "_ZN2nn3ngc6detail19CreateServiceByHipcEv", (void *)ngc_create_trampoline,
    "Host ngc:u IService behind the native ProfanityFilter client (offline host, no NG-word database)"};

extern uint64_t wonder_host_language(void);
HLE_FORCE("_ZN2nn8settings15GetLanguageCodeEPNS0_12LanguageCodeE", "System language = the iOS preferred language (same as oe::GetDesiredLanguage)",
          void, h_settings_language, uint64_t *out) {
    if (!out) die("settings::GetLanguageCode out=NULL");
    *out = wonder_host_language();
    static _Atomic int logged;
    if (!atomic_exchange(&logged, 1)) LOG("HOST_SETTINGS_LANGUAGE code=%.8s", (const char *)out);
}
