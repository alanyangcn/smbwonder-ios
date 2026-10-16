#include "log_throttle.h"
#include <pthread.h>
#include <string.h>
#define KEYS 1024
static struct { char key[64]; unsigned long count; } table[KEYS];
static pthread_mutex_t lock = PTHREAD_MUTEX_INITIALIZER;
int wonder_log_throttle(const char *line, unsigned long *count) {
    char key[64]; size_t n = 0; int words = 0;
    for (; line[n] && n < sizeof key - 1; n++) { if (line[n] == ' ' && ++words == 2) break; key[n] = line[n]; }
    key[n] = 0;
    unsigned long h = 5381; for (size_t i = 0; i < n; i++) h = h * 33 + (unsigned char)key[i];
    pthread_mutex_lock(&lock);
    unsigned long c = 0;
    for (unsigned probe = 0; probe < KEYS; probe++) {
        unsigned slot = (unsigned)((h + probe) % KEYS);
        if (!table[slot].key[0]) { memcpy(table[slot].key, key, n + 1); c = table[slot].count = 1; break; }
        if (!strcmp(table[slot].key, key)) { c = ++table[slot].count; break; }
    }
    pthread_mutex_unlock(&lock);
    if (count) *count = c;
    return !c || c <= WONDER_LOG_FIRST || c % WONDER_LOG_EVERY == 0;
}
