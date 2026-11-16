#pragma once
// Host-test shim: glibc < 2.38 has no strlcpy (ESP-IDF's newlib does). Force-included by tests/run.py.
#include <string.h>
#if defined(__GLIBC__) && !(__GLIBC__ > 2 || (__GLIBC__ == 2 && __GLIBC_MINOR__ >= 38))
static inline size_t fg_strlcpy(char *d, const char *s, size_t n) {
    size_t len = strlen(s);
    if (n) {
        size_t c = len < n - 1 ? len : n - 1;
        memcpy(d, s, c);
        d[c] = 0;
    }
    return len;
}
#define strlcpy fg_strlcpy
#endif
