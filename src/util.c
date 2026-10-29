/* SPDX-License-Identifier: GPL-3.0-or-later */
#include "common.h"
#include "util.h"
#include <stdarg.h>
#include <stdio.h>
#include <ctype.h>

bool g_diag_mode = false;

void os_log(const char *fmt, ...) {
    va_list ap;
    va_start(ap, fmt);
    vprintf(fmt, ap);
    va_end(ap);
    putchar('\n');
    fflush(stdout);
}

void os_diag(const char *fmt, ...) {
    if (!g_diag_mode) return;
    va_list ap;
    va_start(ap, fmt);
    printf("[diag] ");
    vprintf(fmt, ap);
    va_end(ap);
    putchar('\n');
}

int os_stricmp(const char *a, const char *b) {
    for (;; a++, b++) {
        int ca = tolower((unsigned char)*a), cb = tolower((unsigned char)*b);
        if (ca != cb || !ca) return ca - cb;
    }
}

int strncasecmp_portable(const char *a, const char *b, size_t n) {
    for (size_t i = 0; i < n; i++) {
        int ca = tolower((unsigned char)a[i]), cb = tolower((unsigned char)b[i]);
        if (ca != cb || !ca) return ca - cb;
    }
    return 0;
}

void os_strlcpy(char *dst, const char *src, size_t cap) {
    if (!cap) return;
    size_t i = 0;
    for (; i + 1 < cap && src[i]; i++) dst[i] = src[i];
    dst[i] = 0;
}

uint32_t os_crc32(const void *data, size_t len) {
    static uint32_t table[256];
    static bool init;
    if (!init) {
        for (uint32_t i = 0; i < 256; i++) {
            uint32_t c = i;
            for (int k = 0; k < 8; k++) c = (c & 1) ? 0xEDB88320u ^ (c >> 1) : c >> 1;
            table[i] = c;
        }
        init = true;
    }
    uint32_t c = 0xFFFFFFFFu;
    const uint8_t *p = (const uint8_t *)data;
    for (size_t i = 0; i < len; i++) c = table[(c ^ p[i]) & 0xFF] ^ (c >> 8);
    return c ^ 0xFFFFFFFFu;
}

/* Deterministic xorshift PRNG (RPG Maker "random" variable operations). */
static uint32_t s_rng = 0x1234567u;
void os_srand(uint32_t seed) { s_rng = seed ? seed : 0x1234567u; }
uint32_t os_rand(void) {
    uint32_t x = s_rng;
    x ^= x << 13; x ^= x >> 17; x ^= x << 5;
    return s_rng = x;
}
int os_rand_range(int lo, int hi) {
    if (hi < lo) { int t = lo; lo = hi; hi = t; }
    uint32_t span = (uint32_t)(hi - lo) + 1u;
    return lo + (int)(os_rand() % span);
}
