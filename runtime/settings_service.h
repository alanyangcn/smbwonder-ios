#pragma once
#include <stdint.h>
// Host nn::settings::ISettingsServer (the set: service) for the offline host.
// The sdk's settings client stays native (nn::settings::operator==, detail::MakeLanguageCode and its cached table,
// detail::GetAvailableLanguageCodes...). Only the object behind detail::CreateSettingsServerProxyForIpc (sdk+0x3b9950,
// x0 = SharedPointer<ISettingsServer>* out, returns Result) is the host's. Its vtable follows the sdk's CmifProxy object
// (relocations at sdk+0xd13b80..0xd13c00): AddReference, Release, GetProxyInfo, GetInterfaceTypeInfo, then commands
// 0 GetLanguageCode, 1 GetAvailableLanguageCodes, 2 MakeLanguageCode, 3 GetAvailableLanguageCodeCount, 4 GetRegionCode,
// 5 GetAvailableLanguageCodes2, 6 GetAvailableLanguageCodeCount2, 7 GetKeyCodeMap, 8 GetQuestFlag, 9 GetKeyCodeMap2,
// 11 GetFirmwareVersionForDebug, 77 GetDeviceNickName, and GetCmifBaseObject.
// Language codes are the system's table in nn::settings::Language order (18 entries; the pre-4.0.0 commands 1 and 3
// report at most 15). The system language is the iOS preferred language (same as oe::GetDesiredLanguage).
// Region, key-code maps, quest flag, firmware version and nickname have no host answer yet and stop by name.
_Noreturn void wonder_settings_fatal(const char *format, ...) __attribute__((format(printf, 1, 2)));  // supplied by the host
uint64_t wonder_settings_system_language(void);   // supplied by the host: LanguageCode as 8 bytes
struct wonder_settings_array { uint64_t *data; int64_t count; };   // nn::sf::OutArray<LanguageCode>: {pointer, element count} (sdk+0x3b7d54)
struct wonder_settings_service { const void *const *vtable; int32_t references; uint32_t reserved; };
enum { WONDER_SETTINGS_LANGUAGES = 18, WONDER_SETTINGS_LANGUAGES_PRE_400 = 15 };
struct wonder_settings_service *wonder_settings_create(void);   // adds a reference to the process-wide instance
int wonder_settings_live(void);                                  // references held
uint64_t wonder_settings_language_code(int language);           // 0 when out of range
// Slots (guest calls through the vtable with the Switch AArch64 ABI; Result is 32-bit).
void wonder_settings_add_reference(struct wonder_settings_service *self);
void wonder_settings_release(struct wonder_settings_service *self);
uint32_t wonder_settings_get_language_code(struct wonder_settings_service *self, uint64_t *out);
uint32_t wonder_settings_get_available_language_codes(struct wonder_settings_service *self, int32_t *out_count, const struct wonder_settings_array *codes);
uint32_t wonder_settings_make_language_code(struct wonder_settings_service *self, uint64_t *out, int32_t language);
uint32_t wonder_settings_get_available_language_code_count(struct wonder_settings_service *self, int32_t *out);
uint32_t wonder_settings_get_available_language_codes2(struct wonder_settings_service *self, int32_t *out_count, const struct wonder_settings_array *codes);
uint32_t wonder_settings_get_available_language_code_count2(struct wonder_settings_service *self, int32_t *out);
