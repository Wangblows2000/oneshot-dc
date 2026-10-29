/* SPDX-License-Identifier: GPL-3.0-or-later
 * Platform abstraction.  Implemented by platform_dc.c (KallistiOS) and
 * platform_host.c (headless Linux build used by automated tests). */
#ifndef OS_PLATFORM_H
#define OS_PLATFORM_H
#include "../common.h"

/* ---- lifecycle / time ---- */
bool plat_init(void);
void plat_shutdown(void);
uint64_t plat_ticks_ms(void);

/* ---- input (logical buttons, mapped from the Dreamcast controller) ---- */
enum {
    BTN_UP = 1 << 0, BTN_DOWN = 1 << 1, BTN_LEFT = 1 << 2, BTN_RIGHT = 1 << 3,
    BTN_A = 1 << 4,      /* decision */
    BTN_B = 1 << 5,      /* cancel / menu */
    BTN_X = 1 << 6,      /* "number key" (fast-travel in OneShot) */
    BTN_Y = 1 << 7,      /* shift (deselect item) */
    BTN_START = 1 << 8,  /* open the virtual computer */
    BTN_L = 1 << 9, BTN_R = 1 << 10,
};
uint32_t plat_poll_buttons(void);   /* currently held buttons */
bool plat_controller_present(void);

/* ---- read-only asset filesystem (the disc) ---- */
/* Loads a whole file under the data root; returns malloc'd buffer or NULL. */
void *plat_load_file(const char *rel, size_t *len_out, size_t max_len);

/* ---- graphics (320x240 virtual screen; DC scales x2 to 640x480) ---- */
typedef struct tex tex_t;
tex_t *plat_tex_create(int w, int h, const uint16_t *argb1555); /* data is w*h, row-major */
void plat_tex_destroy(tex_t *t);
bool plat_tex_update(tex_t *t, int w, int h, const uint16_t *argb1555); /* same size only */
int plat_tex_bytes_in_use(void);
int plat_vram_free(void);          /* bytes of texture memory still available (diagnostics) */
void plat_frame_begin(void);
void plat_frame_end(void);
/* Draw sub-rect (sx,sy,sw,sh) of tex at (dx,dy) scaled to (dw,dh).
 * color: ARGB8888 modulation (alpha = opacity).  add: RGB offset (0 = none). */
void plat_draw(tex_t *t, int sx, int sy, int sw, int sh, float dx, float dy, float dw, float dh,
               uint32_t color, uint32_t add);
void plat_fill(int x, int y, int w, int h, uint32_t argb);
void plat_set_clip(int x, int y, int w, int h); /* w==0 disables */

/* ---- audio ---- */
int plat_sfx_load(const char *rel);           /* returns handle >=0 or -1 */
void plat_sfx_unload_all(void);
void plat_sfx_play(int h, int vol, int pan);  /* vol 0..255, pan 0..255 */
bool plat_bgm_play(const char *rel, int vol, bool loop);
void plat_bgm_stop(void);
void plat_bgm_volume(int vol);
void plat_audio_update(void);

/* ---- writable persistent storage (VMU on Dreamcast, file on host) ---- */
typedef enum {
    STORE_OK = 0, STORE_NO_DEVICE, STORE_FULL, STORE_IO_ERROR, STORE_NOT_FOUND, STORE_CORRUPT
} store_result_t;
store_result_t plat_store_probe(int *free_bytes_out);
store_result_t plat_store_write(const char *name, const void *buf, size_t len);
store_result_t plat_store_read(const char *name, void *buf, size_t cap, size_t *len_out);
store_result_t plat_store_delete(const char *name);
const char *plat_store_describe(void);

/* ---- debug ---- */
void plat_screenshot(const char *path); /* host only; no-op on DC */

#endif
