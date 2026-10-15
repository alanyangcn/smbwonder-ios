#pragma once
#include <stddef.h>
enum wonder_service { WONDER_LOG, WONDER_OE, WONDER_AOC, WONDER_ERROR, WONDER_FS, WONDER_PARENTAL, WONDER_ABORT, WONDER_SERVICE_COUNT };
int wonder_services_configure(const char *root);
int wonder_service_initialize(enum wonder_service service);
int wonder_service_ready(enum wonder_service service);
int wonder_service_log(const char *message);
void wonder_services_shutdown(void);
const char *wonder_services_root(void);
