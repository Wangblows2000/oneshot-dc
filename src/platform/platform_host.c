/* SPDX-License-Identifier: GPL-3.0-or-later
 * Headless Linux platform used for automated tests and screenshots.
 * - software rasterizer into a 320x240 RGBA framebuffer (PPM screenshots)
 * - controller input from a script / test API
 * - audio requests are only logged
 * - "memory card" = a directory, with fault injection for tests */
#include "platform.h"
#include "host.h"
#include <stdio.h>
#include <stdlib.h>
#include <sys/stat.h>
#include <time.h>
#include <errno.h>

struct tex { int w, h; uint16_t *px; };

static char s_data_root[512] = "build/cdroot/data";
static uint32_t s_fb[SCREEN_W * SCREEN_H];
static int s_clip_x, s_clip_y, s_clip_w, s_clip_h;
static uint32_t s_buttons;
static int s_tex_bytes;
static char s_store_dir[512] = "build/host_vmu";
static host_store_mode_t s_store_mode = HOST_STORE_OK;
static int s_store_free = 128 * 1024;
static int s_sfx_count;
static char s_bgm[256];

void host_set_data_root(const char *p) { os_strlcpy(s_data_root, p, sizeof(s_data_root)); }
void host_set_buttons(uint32_t b) { s_buttons = b; }
void host_set_store(const char *dir, host_store_mode_t mode, int free_bytes) {
    if (dir) os_strlcpy(s_store_dir, dir, sizeof(s_store_dir));
    s_store_mode = mode; s_store_free = free_bytes;
}
const uint32_t *host_framebuffer(void) { return s_fb; }
const char *host_bgm(void) { return s_bgm; }

bool plat_init(void) { srand(1); return true; }
void plat_shutdown(void) {}
uint64_t plat_ticks_ms(void) { struct timespec t; clock_gettime(CLOCK_MONOTONIC, &t); return (uint64_t)t.tv_sec * 1000 + t.tv_nsec / 1000000; }
uint32_t plat_poll_buttons(void) { return s_buttons; }
bool plat_controller_present(void) { return true; }

void *plat_load_file(const char *rel, size_t *len_out, size_t max_len) {
    if (!rel || strstr(rel, "..")) return NULL;
    char p[1024];
    snprintf(p, sizeof(p), "%s/%s", s_data_root, rel);
    FILE *f = fopen(p, "rb");
    if (!f) return NULL;
    fseek(f, 0, SEEK_END);
    long n = ftell(f);
    fseek(f, 0, SEEK_SET);
    if (n < 0 || (size_t)n > max_len) { fclose(f); return NULL; }
    void *b = malloc((size_t)n ? (size_t)n : 1);
    if (b && fread(b, 1, (size_t)n, f) != (size_t)n) { free(b); b = NULL; }
    fclose(f);
    if (b && len_out) *len_out = (size_t)n;
    return b;
}

tex_t *plat_tex_create(int w, int h, const uint16_t *px) {
    tex_t *t = (tex_t *)malloc(sizeof(tex_t));
    if (!t) return NULL;
    t->w = w; t->h = h;
    t->px = (uint16_t *)malloc((size_t)w * h * 2);
    if (!t->px) { free(t); return NULL; }
    memcpy(t->px, px, (size_t)w * h * 2);
    s_tex_bytes += w * h * 2;
    return t;
}
bool plat_tex_update(tex_t *t, int w, int h, const uint16_t *px) {
    if (!t || t->w != w || t->h != h) return false;
    memcpy(t->px, px, (size_t)w * h * 2);
    return true;
}
void plat_tex_destroy(tex_t *t) { if (!t) return; s_tex_bytes -= t->w * t->h * 2; free(t->px); free(t); }
int plat_tex_bytes_in_use(void) { return s_tex_bytes; }
int plat_vram_free(void) { return 64 * 1024 * 1024; }

void plat_frame_begin(void) { for (int i = 0; i < SCREEN_W * SCREEN_H; i++) s_fb[i] = 0xFF000000; s_clip_w = 0; }
void plat_frame_end(void) {}
void plat_set_clip(int x, int y, int w, int h) { s_clip_x = x; s_clip_y = y; s_clip_w = w; s_clip_h = h; }

static inline void blend(int x, int y, uint32_t r, uint32_t g, uint32_t b, uint32_t a) {
    if (x < 0 || y < 0 || x >= SCREEN_W || y >= SCREEN_H) return;
    if (s_clip_w && (x < s_clip_x || y < s_clip_y || x >= s_clip_x + s_clip_w || y >= s_clip_y + s_clip_h)) return;
    uint32_t *d = &s_fb[y * SCREEN_W + x];
    uint32_t dr = (*d >> 16) & 255, dg = (*d >> 8) & 255, db = *d & 255;
    dr = (r * a + dr * (255 - a)) / 255; dg = (g * a + dg * (255 - a)) / 255; db = (b * a + db * (255 - a)) / 255;
    *d = 0xFF000000u | dr << 16 | dg << 8 | db;
}

void plat_draw(tex_t *t, int sx, int sy, int sw, int sh, float dx, float dy, float dw, float dh, uint32_t color, uint32_t add) {
    if (!t || dw <= 0 || dh <= 0 || sw <= 0 || sh <= 0) return;
    int x0 = (int)(dx < 0 ? dx - 0.999f : dx), y0 = (int)(dy < 0 ? dy - 0.999f : dy);
    int w = (int)(dw + 0.5f), h = (int)(dh + 0.5f);
    uint32_t ma = color >> 24, mr = (color >> 16) & 255, mg = (color >> 8) & 255, mb = color & 255;
    uint32_t ar = (add >> 16) & 255, ag = (add >> 8) & 255, ab = add & 255;
    for (int j = 0; j < h; j++) {
        int y = y0 + j;
        if (y < 0 || y >= SCREEN_H) continue;
        int ty = sy + j * sh / h;
        if (ty < 0 || ty >= t->h) continue;
        for (int i = 0; i < w; i++) {
            int x = x0 + i;
            if (x < 0 || x >= SCREEN_W) continue;
            int tx = sx + i * sw / w;
            if (tx < 0 || tx >= t->w) continue;
            uint16_t p = t->px[ty * t->w + tx];
            if (!(p & 0x8000)) continue;
            uint32_t r = ((p >> 10) & 31) * 255 / 31, g = ((p >> 5) & 31) * 255 / 31, b = (p & 31) * 255 / 31;
            r = r * mr / 255 + ar; g = g * mg / 255 + ag; b = b * mb / 255 + ab;
            blend(x, y, r > 255 ? 255 : r, g > 255 ? 255 : g, b > 255 ? 255 : b, ma);
        }
    }
}

void plat_fill(int x, int y, int w, int h, uint32_t argb) {
    uint32_t a = argb >> 24;
    if (!a) return;
    for (int j = y; j < y + h; j++)
        for (int i = x; i < x + w; i++) blend(i, j, (argb >> 16) & 255, (argb >> 8) & 255, argb & 255, a);
}

int plat_sfx_load(const char *rel) {
    size_t n = 0;
    void *b = plat_load_file(rel, &n, 8 * 1024 * 1024);
    if (!b) return -1;
    free(b);
    return s_sfx_count++;
}
void plat_sfx_unload_all(void) { s_sfx_count = 0; }
void plat_sfx_play(int h, int vol, int pan) { (void)h; (void)vol; (void)pan; }
bool plat_bgm_play(const char *rel, int vol, bool loop) { (void)vol; (void)loop; os_strlcpy(s_bgm, rel, sizeof(s_bgm)); return true; }
void plat_bgm_stop(void) { s_bgm[0] = 0; }
void plat_bgm_volume(int vol) { (void)vol; }
void plat_audio_update(void) {}

/* ---- storage ---- */
static void store_path(const char *name, char *out, size_t cap) { snprintf(out, cap, "%s/%s", s_store_dir, name); }

store_result_t plat_store_probe(int *free_bytes) {
    if (s_store_mode == HOST_STORE_NONE) return STORE_NO_DEVICE;
    mkdir(s_store_dir, 0755);
    if (free_bytes) *free_bytes = s_store_mode == HOST_STORE_FULL ? 0 : s_store_free;
    return STORE_OK;
}
store_result_t plat_store_write(const char *name, const void *buf, size_t len) {
    if (s_store_mode == HOST_STORE_NONE) return STORE_NO_DEVICE;
    if (s_store_mode == HOST_STORE_FULL) return STORE_FULL;
    char p[1024];
    store_path(name, p, sizeof(p));
    FILE *f = fopen(p, "wb");
    if (!f) return STORE_IO_ERROR;
    size_t n = s_store_mode == HOST_STORE_TORN ? len / 2 : len; /* simulated power loss mid-write */
    size_t w = fwrite(buf, 1, n, f);
    fclose(f);
    if (s_store_mode == HOST_STORE_TORN) return STORE_OK;  /* device claims success: verify must catch it */
    return w == len ? STORE_OK : STORE_IO_ERROR;
}
store_result_t plat_store_read(const char *name, void *buf, size_t cap, size_t *len) {
    if (s_store_mode == HOST_STORE_NONE) return STORE_NO_DEVICE;
    char p[1024];
    store_path(name, p, sizeof(p));
    FILE *f = fopen(p, "rb");
    if (!f) return STORE_NOT_FOUND;
    size_t n = fread(buf, 1, cap, f);
    fclose(f);
    if (len) *len = n;
    return STORE_OK;
}
store_result_t plat_store_delete(const char *name) {
    if (s_store_mode == HOST_STORE_NONE) return STORE_NO_DEVICE;
    char p[1024];
    store_path(name, p, sizeof(p));
    if (remove(p) != 0) return errno == ENOENT ? STORE_NOT_FOUND : STORE_IO_ERROR;
    return STORE_OK;
}
const char *plat_store_describe(void) { return "host directory (emulated VMU)"; }

void plat_screenshot(const char *path) {
    FILE *f = fopen(path, "wb");
    if (!f) return;
    fprintf(f, "P6\n%d %d\n255\n", SCREEN_W, SCREEN_H);
    for (int i = 0; i < SCREEN_W * SCREEN_H; i++) {
        uint8_t px[3] = {(uint8_t)(s_fb[i] >> 16), (uint8_t)(s_fb[i] >> 8), (uint8_t)s_fb[i]};
        fwrite(px, 1, 3, f);
    }
    fclose(f);
}
