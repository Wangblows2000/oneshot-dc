/* SPDX-License-Identifier: GPL-3.0-or-later
 * Sega Dreamcast platform layer (KallistiOS).
 *  - PVR: everything is drawn in the translucent list with autosort disabled,
 *    so submission order = draw order (2D painter's algorithm).
 *    The 320x240 virtual screen is scaled exactly x2 to 640x480.
 *  - Textures: ARGB1555, non-twiddled, padded to power-of-two sizes.
 *  - Input: first controller found on the maple bus.
 *  - Files: read-only from the disc (/cd/data/...).
 *  - Audio: snd_sfx for effects, libwav (kos-ports) streams music.
 *  - Storage: first VMU found; nothing is assumed to exist. */
#include "platform.h"
#include <kos.h>
#include <dc/pvr.h>
#include <dc/maple.h>
#include <dc/maple/controller.h>
#include <dc/vmufs.h>
#include <dc/vmu_pkg.h>
#include <dc/sound/sfxmgr.h>
#include <wav/sndwav.h>
#include <stdio.h>
#include <stdlib.h>

KOS_INIT_FLAGS(INIT_DEFAULT);

#define DATA_ROOT "/cd/data/"

struct tex {
    pvr_ptr_t ptr;
    int w, h, tw, th;
    pvr_poly_hdr_t hdr;
    bool hdr_ok;
};

static int s_tex_bytes;
static tex_t *s_last_tex;
static int s_last_kind;   /* 0 none, 1 textured, 2 colour */
static float s_z;
static pvr_poly_hdr_t s_col_hdr;

bool plat_init(void) {
    pvr_init_params_t p = {
        .opb_sizes = {PVR_BINSIZE_0, PVR_BINSIZE_0, PVR_BINSIZE_16, PVR_BINSIZE_0, PVR_BINSIZE_0},
        .vertex_buf_size = 1024 * 1024,
        .dma_enabled = 0,
        .fsaa_enabled = 0,
        .autosort_disabled = 1,
        .opb_overflow_count = 3,
    };
    if (pvr_init(&p) != 0) { printf("pvr_init failed\n"); return false; }
    pvr_set_bg_color(0, 0, 0);
    pvr_poly_cxt_t cxt;
    pvr_poly_cxt_col(&cxt, PVR_LIST_TR_POLY);
    pvr_poly_compile(&s_col_hdr, &cxt);
    snd_stream_init();
    wav_init();
    printf("OneShot DC: PVR ready, %lu bytes VRAM free\n", (unsigned long)pvr_mem_available());
    return true;
}

void plat_shutdown(void) { wav_shutdown(); snd_stream_shutdown(); }
uint64_t plat_ticks_ms(void) { return timer_ms_gettime64(); }

/* ---------------------------------------------------------------- input */
uint32_t plat_poll_buttons(void) {
    maple_device_t *dev = maple_enum_type(0, MAPLE_FUNC_CONTROLLER);
    if (!dev) return 0;
    cont_state_t *st = (cont_state_t *)maple_dev_status(dev);
    if (!st) return 0;
    uint32_t b = 0, c = st->buttons;
    if ((c & CONT_DPAD_UP) || st->joyy < -64) b |= BTN_UP;
    if ((c & CONT_DPAD_DOWN) || st->joyy > 64) b |= BTN_DOWN;
    if ((c & CONT_DPAD_LEFT) || st->joyx < -64) b |= BTN_LEFT;
    if ((c & CONT_DPAD_RIGHT) || st->joyx > 64) b |= BTN_RIGHT;
    if (c & CONT_A) b |= BTN_A;
    if (c & CONT_B) b |= BTN_B;
    if (c & CONT_X) b |= BTN_X;
    if (c & CONT_Y) b |= BTN_Y;
    if (c & CONT_START) b |= BTN_START;
    if (st->ltrig > 128) b |= BTN_L;
    if (st->rtrig > 128) b |= BTN_R;
    return b;
}
bool plat_controller_present(void) { return maple_enum_type(0, MAPLE_FUNC_CONTROLLER) != NULL; }

/* ---------------------------------------------------------------- files */
void *plat_load_file(const char *rel, size_t *len_out, size_t max_len) {
    if (!rel || strstr(rel, "..")) return NULL;
    char path[256];
    snprintf(path, sizeof(path), DATA_ROOT "%s", rel);
    file_t f = fs_open(path, O_RDONLY);
    if (f == FILEHND_INVALID) return NULL;
    size_t n = fs_total(f);
    if (n > max_len) { fs_close(f); return NULL; }
    void *b = malloc(n ? n : 1);
    if (b && (size_t)fs_read(f, b, n) != n) { free(b); b = NULL; }
    fs_close(f);
    if (b && len_out) *len_out = n;
    return b;
}

/* ---------------------------------------------------------------- textures */
static int pow2(int v) { int p = 8; while (p < v) p <<= 1; return p; }

static void upload(tex_t *t, const uint16_t *px) {
    /* copy row by row into the padded power-of-two surface */
    uint16_t *tmp = (uint16_t *)memalign(32, (size_t)t->tw * t->th * 2);
    if (!tmp) return;
    memset(tmp, 0, (size_t)t->tw * t->th * 2);
    for (int y = 0; y < t->h; y++) memcpy(tmp + y * t->tw, px + y * t->w, (size_t)t->w * 2);
    pvr_txr_load(tmp, t->ptr, (size_t)t->tw * t->th * 2);
    free(tmp);
}

tex_t *plat_tex_create(int w, int h, const uint16_t *px) {
    if (w <= 0 || h <= 0 || w > 1024 || h > 1024) return NULL;
    tex_t *t = (tex_t *)calloc(1, sizeof(tex_t));
    if (!t) return NULL;
    t->w = w; t->h = h; t->tw = pow2(w); t->th = pow2(h);
    t->ptr = pvr_mem_malloc((size_t)t->tw * t->th * 2);
    if (!t->ptr) { printf("VRAM exhausted (%dx%d)\n", w, h); free(t); return NULL; }
    upload(t, px);
    pvr_poly_cxt_t cxt;
    pvr_poly_cxt_txr(&cxt, PVR_LIST_TR_POLY, PVR_TXRFMT_ARGB1555 | PVR_TXRFMT_NONTWIDDLED,
                     t->tw, t->th, t->ptr, PVR_FILTER_NONE);
    cxt.gen.specular = 1;  /* offset colour = screen tint brightening */
    pvr_poly_compile(&t->hdr, &cxt);
    t->hdr_ok = true;
    s_tex_bytes += t->tw * t->th * 2;
    return t;
}

bool plat_tex_update(tex_t *t, int w, int h, const uint16_t *px) {
    if (!t || t->w != w || t->h != h) return false;
    upload(t, px);
    return true;
}

void plat_tex_destroy(tex_t *t) {
    if (!t) return;
    if (s_last_tex == t) { s_last_tex = NULL; s_last_kind = 0; }
    pvr_mem_free(t->ptr);
    s_tex_bytes -= t->tw * t->th * 2;
    free(t);
}
int plat_tex_bytes_in_use(void) { return s_tex_bytes; }
int plat_vram_free(void) { return (int)pvr_mem_available(); }

/* ---------------------------------------------------------------- drawing */
void plat_frame_begin(void) {
    pvr_wait_ready();
    pvr_scene_begin();
    pvr_list_begin(PVR_LIST_TR_POLY);
    s_last_tex = NULL; s_last_kind = 0; s_z = 1.0f;
}
void plat_frame_end(void) {
    pvr_list_finish();
    pvr_scene_finish();
}
void plat_set_clip(int x, int y, int w, int h) { (void)x; (void)y; (void)w; (void)h; }

static void quad(float x0, float y0, float x1, float y1, float u0, float v0, float u1, float v1, uint32_t argb, uint32_t oargb) {
    pvr_vertex_t v;
    s_z += 0.0001f;
    v.z = s_z; v.argb = argb; v.oargb = oargb;
    v.flags = PVR_CMD_VERTEX; v.x = x0; v.y = y0; v.u = u0; v.v = v0; pvr_prim(&v, sizeof(v));
    v.flags = PVR_CMD_VERTEX; v.x = x1; v.y = y0; v.u = u1; v.v = v0; pvr_prim(&v, sizeof(v));
    v.flags = PVR_CMD_VERTEX; v.x = x0; v.y = y1; v.u = u0; v.v = v1; pvr_prim(&v, sizeof(v));
    v.flags = PVR_CMD_VERTEX_EOL; v.x = x1; v.y = y1; v.u = u1; v.v = v1; pvr_prim(&v, sizeof(v));
}

void plat_draw(tex_t *t, int sx, int sy, int sw, int sh, float dx, float dy, float dw, float dh, uint32_t color, uint32_t add) {
    if (!t || !t->hdr_ok || (color >> 24) == 0) return;
    if (s_last_kind != 1 || s_last_tex != t) { pvr_prim(&t->hdr, sizeof(t->hdr)); s_last_tex = t; s_last_kind = 1; }
    float u0 = (float)sx / t->tw, v0 = (float)sy / t->th, u1 = (float)(sx + sw) / t->tw, v1 = (float)(sy + sh) / t->th;
    quad(dx * 2.0f, dy * 2.0f, (dx + dw) * 2.0f, (dy + dh) * 2.0f, u0, v0, u1, v1, color, add);
}

void plat_fill(int x, int y, int w, int h, uint32_t argb) {
    if ((argb >> 24) == 0 || w <= 0 || h <= 0) return;
    if (s_last_kind != 2) { pvr_prim(&s_col_hdr, sizeof(s_col_hdr)); s_last_kind = 2; s_last_tex = NULL; }
    quad(x * 2.0f, y * 2.0f, (x + w) * 2.0f, (y + h) * 2.0f, 0, 0, 0, 0, argb, 0);
}

/* ---------------------------------------------------------------- audio */
#define MAX_SFX 64
static sfxhnd_t s_sfx[MAX_SFX];
static int s_nsfx;
static wav_stream_hnd_t s_bgm = SND_STREAM_INVALID;

int plat_sfx_load(const char *rel) {
    if (s_nsfx >= MAX_SFX) return -1;
    char path[256];
    snprintf(path, sizeof(path), DATA_ROOT "%s", rel);
    sfxhnd_t h = snd_sfx_load(path);
    if (!h) return -1;
    s_sfx[s_nsfx] = h;
    return s_nsfx++;
}
void plat_sfx_unload_all(void) { snd_sfx_unload_all(); s_nsfx = 0; }
void plat_sfx_play(int h, int vol, int pan) { if (h >= 0 && h < s_nsfx) snd_sfx_play(s_sfx[h], vol, pan); }

bool plat_bgm_play(const char *rel, int vol, bool loop) {
    plat_bgm_stop();
    char path[256];
    snprintf(path, sizeof(path), DATA_ROOT "%s", rel);
    s_bgm = wav_create(path, loop ? 1 : 0);
    if (s_bgm == SND_STREAM_INVALID) { printf("bgm: cannot open %s\n", path); return false; }
    wav_volume(s_bgm, vol);
    wav_play(s_bgm);
    return true;
}
void plat_bgm_stop(void) {
    if (s_bgm != SND_STREAM_INVALID) { wav_stop(s_bgm); wav_destroy(s_bgm); s_bgm = SND_STREAM_INVALID; }
}
void plat_bgm_volume(int vol) { if (s_bgm != SND_STREAM_INVALID) wav_volume(s_bgm, vol); }
void plat_audio_update(void) {}

/* ---------------------------------------------------------------- VMU storage */
static maple_device_t *vmu(void) { return maple_enum_type(0, MAPLE_FUNC_MEMCARD); }

store_result_t plat_store_probe(int *free_bytes) {
    maple_device_t *d = vmu();
    if (!d) return STORE_NO_DEVICE;
    int fb = vmufs_free_blocks(d);
    if (fb < 0) return STORE_IO_ERROR;
    if (free_bytes) *free_bytes = fb * 512;
    return STORE_OK;
}

static uint8_t s_icon[512];
static uint16_t s_icon_pal[16] = {0xF000, 0xFFD5, 0xF6CF, 0xFA48};

store_result_t plat_store_write(const char *name, const void *buf, size_t len) {
    maple_device_t *d = vmu();
    if (!d) return STORE_NO_DEVICE;
    /* simple original 32x32 icon: a lit bulb shape in palette index 1 */
    for (int y = 0; y < 32; y++)
        for (int x = 0; x < 32; x += 2) {
            int dx = x - 16, dy = y - 13, dx2 = x + 1 - 16;
            int a = (dx * dx + dy * dy < 90) ? 1 : (y > 22 && y < 29 && x > 11 && x < 20) ? 3 : 0;
            int b2 = (dx2 * dx2 + dy * dy < 90) ? 1 : (y > 22 && y < 29 && x + 1 > 11 && x + 1 < 20) ? 3 : 0;
            s_icon[y * 16 + x / 2] = (uint8_t)(a << 4 | b2);
        }
    vmu_pkg_t pkg;
    memset(&pkg, 0, sizeof(pkg));
    strcpy(pkg.desc_short, "OneShot");
    strcpy(pkg.desc_long, "OneShot (2014) DC fan port");
    strcpy(pkg.app_id, "ONESHOTDC");
    pkg.icon_cnt = 1;
    pkg.icon_anim_speed = 0;
    memcpy(pkg.icon_pal, s_icon_pal, sizeof(s_icon_pal));
    pkg.icon_data = s_icon;
    pkg.eyecatch_type = VMUPKG_EC_NONE;
    pkg.data_len = (int)len;
    pkg.data = (const uint8_t *)buf;
    uint8_t *out = NULL;
    int out_len = 0;
    if (vmu_pkg_build(&pkg, &out, &out_len) < 0 || !out) return STORE_IO_ERROR;
    int need = (out_len + 511) / 512, have = vmufs_free_blocks(d);
    int r = vmufs_write(d, name, out, out_len, VMUFS_OVERWRITE);
    free(out);
    if (r < 0) return (have >= 0 && have < need) ? STORE_FULL : STORE_IO_ERROR;
    return STORE_OK;
}

store_result_t plat_store_read(const char *name, void *buf, size_t cap, size_t *len_out) {
    maple_device_t *d = vmu();
    if (!d) return STORE_NO_DEVICE;
    void *raw = NULL;
    int raw_len = 0;
    if (vmufs_read(d, name, &raw, &raw_len) < 0 || !raw) return STORE_NOT_FOUND;
    vmu_pkg_t pkg;
    if (vmu_pkg_parse((uint8_t *)raw, (size_t)raw_len, &pkg) < 0 || pkg.data_len < 0 || (size_t)pkg.data_len > cap) {
        free(raw);
        return STORE_CORRUPT;
    }
    memcpy(buf, pkg.data, (size_t)pkg.data_len);
    if (len_out) *len_out = (size_t)pkg.data_len;
    free(raw);
    return STORE_OK;
}

store_result_t plat_store_delete(const char *name) {
    maple_device_t *d = vmu();
    if (!d) return STORE_NO_DEVICE;
    return vmufs_delete(d, name) < 0 ? STORE_NOT_FOUND : STORE_OK;
}
const char *plat_store_describe(void) { return "VMU in port A"; }
void plat_screenshot(const char *path) { (void)path; }
