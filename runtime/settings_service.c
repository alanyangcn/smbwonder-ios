#include "settings_service.h"
#include <pthread.h>
#include <string.h>
static pthread_mutex_t lock = PTHREAD_MUTEX_INITIALIZER;
static struct wonder_settings_service instance;
// nn::settings::Language order (system settings LanguageCode table, 18 entries since 10.1.0).
static const char languages[WONDER_SETTINGS_LANGUAGES][8] = {
    "ja", "en-US", "fr", "de", "it", "es", "zh-CN", "ko", "nl", "pt", "ru", "zh-TW",
    "en-GB", "fr-CA", "es-419", "zh-Hans", "zh-Hant", "pt-BR",
};
uint64_t wonder_settings_language_code(int language) {
    if (language < 0 || language >= WONDER_SETTINGS_LANGUAGES) return 0;
    uint64_t v; memcpy(&v, languages[language], 8); return v;
}
static void stop_proxy_info(void) { wonder_settings_fatal("set: ISettingsServer::GetProxyInfo is not provided by the host service"); }
static void stop_type_info(void) { wonder_settings_fatal("set: ISettingsServer::GetInterfaceTypeInfo is not provided by the host service"); }
static void stop_region(void) { wonder_settings_fatal("set: ISettingsServer::GetRegionCode has no host answer"); }
static void stop_key_code_map(void) { wonder_settings_fatal("set: ISettingsServer::GetKeyCodeMap has no host answer"); }
static void stop_quest_flag(void) { wonder_settings_fatal("set: ISettingsServer::GetQuestFlag has no host answer"); }
static void stop_key_code_map2(void) { wonder_settings_fatal("set: ISettingsServer::GetKeyCodeMap2 has no host answer"); }
static void stop_firmware_version(void) { wonder_settings_fatal("set: ISettingsServer::GetFirmwareVersionForDebug has no host answer"); }
static void stop_nickname(void) { wonder_settings_fatal("set: ISettingsServer::GetDeviceNickName has no host answer"); }
static void stop_base_object(void) { wonder_settings_fatal("set: ISettingsServer::GetCmifBaseObject: the host object is not a CMIF proxy"); }
static const void *const vtable[17] = {
    (const void *)wonder_settings_add_reference, (const void *)wonder_settings_release,
    (const void *)stop_proxy_info, (const void *)stop_type_info,
    (const void *)wonder_settings_get_language_code, (const void *)wonder_settings_get_available_language_codes,
    (const void *)wonder_settings_make_language_code, (const void *)wonder_settings_get_available_language_code_count,
    (const void *)stop_region, (const void *)wonder_settings_get_available_language_codes2,
    (const void *)wonder_settings_get_available_language_code_count2, (const void *)stop_key_code_map,
    (const void *)stop_quest_flag, (const void *)stop_key_code_map2, (const void *)stop_firmware_version,
    (const void *)stop_nickname, (const void *)stop_base_object,
};
static void require(struct wonder_settings_service *self, const char *who) {
    if (self != &instance || self->vtable != vtable) wonder_settings_fatal("set: %s on an object the host did not create (%p)", who, (void *)self);
    if (self->references <= 0) wonder_settings_fatal("set: %s after the last Release", who);
}
struct wonder_settings_service *wonder_settings_create(void) {
    pthread_mutex_lock(&lock); instance.vtable = vtable; instance.references++; pthread_mutex_unlock(&lock);
    return &instance;
}
int wonder_settings_live(void) { pthread_mutex_lock(&lock); int r = instance.references; pthread_mutex_unlock(&lock); return r; }
void wonder_settings_add_reference(struct wonder_settings_service *self) {
    pthread_mutex_lock(&lock); require(self, "AddReference"); self->references++; pthread_mutex_unlock(&lock);
}
void wonder_settings_release(struct wonder_settings_service *self) {
    pthread_mutex_lock(&lock); require(self, "Release"); self->references--; pthread_mutex_unlock(&lock);
}
static void enter(struct wonder_settings_service *self, const void *out, const char *who) {
    pthread_mutex_lock(&lock); require(self, who); pthread_mutex_unlock(&lock);
    if (!out) wonder_settings_fatal("set: %s out=NULL", who);
}
uint32_t wonder_settings_get_language_code(struct wonder_settings_service *self, uint64_t *out) {
    enter(self, out, "GetLanguageCode"); *out = wonder_settings_system_language(); return 0;
}
static uint32_t codes(struct wonder_settings_service *self, int32_t *out_count, const struct wonder_settings_array *array, int limit, const char *who) {
    enter(self, out_count, who);
    if (!array || array->count < 0 || (array->count > 0 && !array->data)) wonder_settings_fatal("set: %s: invalid output array", who);
    int n = array->count < limit ? (int)array->count : limit;
    for (int i = 0; i < n; i++) array->data[i] = wonder_settings_language_code(i);
    *out_count = n; return 0;
}
uint32_t wonder_settings_get_available_language_codes(struct wonder_settings_service *self, int32_t *out_count, const struct wonder_settings_array *array) {
    return codes(self, out_count, array, WONDER_SETTINGS_LANGUAGES_PRE_400, "GetAvailableLanguageCodes");
}
uint32_t wonder_settings_get_available_language_codes2(struct wonder_settings_service *self, int32_t *out_count, const struct wonder_settings_array *array) {
    return codes(self, out_count, array, WONDER_SETTINGS_LANGUAGES, "GetAvailableLanguageCodes2");
}
uint32_t wonder_settings_make_language_code(struct wonder_settings_service *self, uint64_t *out, int32_t language) {
    enter(self, out, "MakeLanguageCode");
    // The client only asks the service for languages outside its cached table (sdk+0x3b7f30); the system's
    // out-of-range result is not modelled, so it stops with the value.
    if (language < 0 || language >= WONDER_SETTINGS_LANGUAGES) wonder_settings_fatal("set: MakeLanguageCode language=%d is outside the %d-entry table", language, WONDER_SETTINGS_LANGUAGES);
    *out = wonder_settings_language_code(language); return 0;
}
uint32_t wonder_settings_get_available_language_code_count(struct wonder_settings_service *self, int32_t *out) {
    enter(self, out, "GetAvailableLanguageCodeCount"); *out = WONDER_SETTINGS_LANGUAGES_PRE_400; return 0;
}
uint32_t wonder_settings_get_available_language_code_count2(struct wonder_settings_service *self, int32_t *out) {
    enter(self, out, "GetAvailableLanguageCodeCount2"); *out = WONDER_SETTINGS_LANGUAGES; return 0;
}
