#include "report_shadow.h"
#include <string.h>
void wonder_report_shadow_clear(struct wonder_report_shadow *s) { s->count = 0; s->overflow = 0; }
static void add(struct wonder_report_shadow *s, uint64_t address, uint64_t size, uint32_t value, int known) {
    if (s->count == WONDER_REPORT_SHADOW_SLOTS) { s->overflow = 1; return; }
    s->entry[s->count].address = address; s->entry[s->count].size = size; s->entry[s->count].value = value; s->entry[s->count].known = known; s->count++;
}
void wonder_report_shadow_value(struct wonder_report_shadow *s, uint64_t address, uint32_t value) { add(s, address, 4, value, 1); }
void wonder_report_shadow_unknown(struct wonder_report_shadow *s, uint64_t address, uint64_t size) { add(s, address, size, 0, 0); }
int wonder_report_shadow_lookup(const struct wonder_report_shadow *s, uint64_t address, uint32_t *value) {
    if (s->overflow) return -1;
    for (unsigned i = s->count; i-- > 0; ) {   // latest write first
        const uint64_t a = s->entry[i].address, n = s->entry[i].size;
        if (a + n <= address || address + 4 <= a) continue;
        if (!s->entry[i].known || a != address) return -1;   // unknown bytes, or a value write only partly over these 4
        *value = s->entry[i].value; return 1;
    }
    return 0;
}
