#pragma once
// Rate limit for high-frequency diagnostic lines (NVN reports run per call, every frame: round112 wrote 112 MB in
// 10 minutes, enough to stall devicectl's file transfer). Lines are keyed by their first two words; each key is
// printed in full WONDER_LOG_FIRST times, then once every WONDER_LOG_EVERY occurrences. Thread-safe.
#define WONDER_LOG_FIRST 200u
#define WONDER_LOG_EVERY 1000u
// 1 = print this line; *count (optional) receives the occurrence number of its key (1-based). Keys beyond the table
// capacity are always printed.
int wonder_log_throttle(const char *line, unsigned long *count);
