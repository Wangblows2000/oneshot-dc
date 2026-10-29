/* SPDX-License-Identifier: GPL-3.0-or-later */
#ifndef OS_GFX_H
#define OS_GFX_H
#include "../common.h"
#include "../platform/platform.h"

/* CPU-side decoded indexed image (OSI1). */
typedef struct {
    uint16_t w, h;
    uint16_t pal[256];     /* RGB555 | 0x8000 */
    const uint8_t *px;     /* w*h indices, points into buf */
    void *buf;
} image_t;

bool image_load(int cat, const char *name, image_t *out);
void image_free(image_t *im);

/* Cached GPU texture for an asset.  `keyed`: palette index 0 is transparent. */
typedef struct big_img big_img_t;   /* see gfx.c: images larger than one PVR texture */
typedef struct {
    tex_t *tex;
    int w, h;
    bool valid;
    big_img_t *big;        /* non-NULL: tex is a streamed 512x512 window into a larger image */
} gtex_t;

typedef enum { LIFE_ROOM = 0, LIFE_PERSISTENT = 1 } tex_life_t;
const gtex_t *gfx_tex(int cat, const char *name, bool keyed, tex_life_t life);
void gfx_purge_room(void);          /* free LIFE_ROOM textures (map change) */
void gfx_purge_all(void);
int gfx_cache_count(void);
int gfx_tex_failures(void);
int gfx_big_window_uploads(void);  /* window refreshes of >1024 px images (diagnostics/tests) */   /* texture loads/uploads that failed (diagnostics) */

/* Text canvases: ARGB1555 CPU buffers uploaded on change. */
typedef struct {
    int w, h;
    uint16_t *px;
    tex_t *tex;
    bool dirty;
} canvas_t;

bool canvas_init(canvas_t *c, int w, int h);
void canvas_free(canvas_t *c);
void canvas_clear(canvas_t *c);
void canvas_fill(canvas_t *c, int x, int y, int w, int h, uint16_t col);
/* Draws one glyph line of text using the system graphic colour swatch `color`
 * (0..19).  Returns the x after the text.  Handles cp1252 bytes. */
int canvas_text(canvas_t *c, int x, int y, const char *s, int color);
int canvas_char(canvas_t *c, int x, int y, uint8_t ch, int color);
/* "Computer" UI text: fixed RGB555 colour, no game window-skin swatches, no shadow.
 * Set to 0 to return to RPG Maker-style text. */
void canvas_set_plain_color(uint16_t argb1555);
void canvas_blit_image(canvas_t *c, int dx, int dy, const image_t *im, int sx, int sy, int sw, int sh, bool keyed);
void canvas_draw(canvas_t *c, int x, int y, uint32_t color);

bool gfx_init(void);
bool gfx_set_system(const char *name);  /* loads system graphic (window skin + text colours) */
const char *gfx_system_name(void);
int text_width(const char *s);

void gfx_window(int x, int y, int w, int h, int opacity /*0..255*/, bool frame);
void gfx_cursor(int x, int y, int w, int h, int blink_frame);
void gfx_draw_tex(const gtex_t *t, int sx, int sy, int sw, int sh, float dx, float dy, float dw, float dh, uint32_t color, uint32_t add);
uint16_t gfx_text_color_sample(int color, int gx, int gy);

#endif
