#pragma once
#include <stdint.h>
#include <stddef.h>
// Host nn::ngc::detail::IService (the ngc:u profanity-filter service) for the offline host.
// The sdk's own nn::ngc::ProfanityFilter client stays native: argument checks, UTF-8/UTF-16 handling, its work-buffer
// heap, the language option (detail::GetLanguageSetting) and the shared proxy refcount at sdk+0xdc6438. Only the
// service object behind detail::CreateServiceByHipc (sdk+0x387270) is the host's. Its vtable follows the sdk's
// CmifProxy object (relocations at sdk+0xd0f650..0xd0f688): AddReference, Release, GetProxyInfo,
// GetInterfaceTypeInfo, GetContentVersion, Check, Mask, Reload.
// The host has no NG-word database (Nintendo system data, not shipped), so Check reports no matching pattern and
// Mask returns the text unchanged with zero masked words. Every call is counted for the log.
_Noreturn void wonder_ngc_fatal(const char *format, ...) __attribute__((format(printf, 1, 2)));  // supplied by the host
void wonder_ngc_observe(const char *call, uint64_t count, uint32_t language_mask);   // supplied by the host: 1st and every 1000th call
struct wonder_ngc_array { const char *data; uint64_t size; };            // nn::sf::InArray<char> / OutArray<char>
struct wonder_ngc_option { uint32_t word[3]; uint32_t language_mask; };  // nn::ngc::detail::ProfanityFilterOption (sdk+0x386790..0x3867ac)
_Static_assert(sizeof(struct wonder_ngc_option) == 0x10, "ProfanityFilterOption");
struct wonder_ngc_service { const void *const *vtable; int32_t references; uint32_t reserved; };
struct wonder_ngc_counts { uint64_t created, checks, masks, reloads; uint32_t last_language_mask; };
struct wonder_ngc_service *wonder_ngc_create(void);   // one reference; the previous instance must be fully released
int wonder_ngc_live(void);                             // references held on the current instance
struct wonder_ngc_counts wonder_ngc_counts(void);
// Slots (guest calls them through the vtable with the Switch AArch64 ABI; all arguments are pointers or 32-bit).
void wonder_ngc_add_reference(struct wonder_ngc_service *self);
void wonder_ngc_release(struct wonder_ngc_service *self);
uint32_t wonder_ngc_check(struct wonder_ngc_service *self, uint32_t *out_matched, const struct wonder_ngc_array *text,
                          uint32_t patterns, const struct wonder_ngc_option *option);
uint32_t wonder_ngc_mask(struct wonder_ngc_service *self, int32_t *out_count, const struct wonder_ngc_array *out_text,
                         const struct wonder_ngc_array *text, uint32_t patterns, const struct wonder_ngc_option *option);
uint32_t wonder_ngc_reload(struct wonder_ngc_service *self);
