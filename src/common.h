/* SPDX-License-Identifier: GPL-3.0-or-later
 * OneShot (2014, RPG Maker 2003) Dreamcast runtime - shared definitions. */
#ifndef OS_COMMON_H
#define OS_COMMON_H

#include <stdint.h>
#include <stddef.h>
#include <stdbool.h>
#include <string.h>

#define SCREEN_W 320
#define SCREEN_H 240
#define TILE 16

/* Hard limits.  Every loader checks data against these before use. */
#define MAX_SWITCHES 512
#define MAX_VARIABLES 512
#define MAX_ITEMS 256
#define MAX_SKILLS 64
#define MAX_ACTORS 16
#define MAX_CHIPSETS 64
#define MAX_COMMON_EVENTS 128
#define MAX_MAP_W 500
#define MAX_MAP_H 500
#define MAX_EVENTS 512
#define MAX_PAGES 16
#define MAX_PICTURES 50
#define MAX_ASSETS 1024
#define MAX_MAPINFO 256
#define MAX_PARAMS 4096
#define MAX_PARTY 4
#define NAME_LEN 64

typedef struct {
    const uint8_t *p, *end;
    bool err;
} reader_t;

static inline void rd_init(reader_t *r, const void *buf, size_t len) {
    r->p = (const uint8_t *)buf; r->end = r->p + len; r->err = false;
}
static inline bool rd_need(reader_t *r, size_t n) {
    if (r->err || (size_t)(r->end - r->p) < n) { r->err = true; return false; }
    return true;
}
static inline uint8_t rd_u8(reader_t *r) { if (!rd_need(r, 1)) return 0; return *r->p++; }
static inline uint16_t rd_u16(reader_t *r) {
    if (!rd_need(r, 2)) return 0;
    uint16_t v = (uint16_t)(r->p[0] | (r->p[1] << 8)); r->p += 2; return v;
}
static inline int16_t rd_i16(reader_t *r) { return (int16_t)rd_u16(r); }
static inline uint32_t rd_u32(reader_t *r) {
    if (!rd_need(r, 4)) return 0;
    uint32_t v = (uint32_t)r->p[0] | ((uint32_t)r->p[1] << 8) | ((uint32_t)r->p[2] << 16) | ((uint32_t)r->p[3] << 24);
    r->p += 4; return v;
}
static inline int32_t rd_i32(reader_t *r) { return (int32_t)rd_u32(r); }
static inline const uint8_t *rd_bytes(reader_t *r, size_t n) {
    if (!rd_need(r, n)) return NULL;
    const uint8_t *q = r->p; r->p += n; return q;
}
/* Length-prefixed (u8) string copied into dst (always NUL-terminated). */
static inline void rd_str(reader_t *r, char *dst, size_t cap) {
    uint8_t n = rd_u8(r);
    const uint8_t *s = rd_bytes(r, n);
    size_t m = (s && n < cap) ? n : 0;
    if (s && n >= cap) r->err = true;
    if (m) memcpy(dst, s, m);
    if (cap) dst[m] = 0;
}
static inline void rd_align4(reader_t *r, const void *base) {
    size_t off = (size_t)(r->p - (const uint8_t *)base);
    size_t pad = (4 - (off & 3)) & 3;
    rd_bytes(r, pad);
}

static inline int clampi(int v, int lo, int hi) { return v < lo ? lo : v > hi ? hi : v; }

/* Logging: diagnostic mode adds event/VFS traces (no spoilers in normal play). */
void os_log(const char *fmt, ...);
void os_diag(const char *fmt, ...);
extern bool g_diag_mode;

/* Simple case-insensitive compare used for asset names (Windows semantics). */
int os_stricmp(const char *a, const char *b);
void os_strlcpy(char *dst, const char *src, size_t cap);

#endif
