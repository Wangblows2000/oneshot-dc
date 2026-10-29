/* SPDX-License-Identifier: GPL-3.0-or-later */
#include "gfx.h"
#include "../resources/gamedata.h"
#include <stdlib.h>
#include <stdio.h>

#define IMG_MAX_BYTES (2u * 1024 * 1024)
#define TEX_CACHE 96

static int s_tex_failures;
int gfx_tex_failures(void) { return s_tex_failures; }

static uint8_t s_font[256 * 12];
static image_t s_sys;          /* system graphic, CPU copy for text colours */
static bool s_sys_ok;
static char s_sys_name[NAME_LEN];
static const gtex_t *s_sys_tex;

bool image_load(int cat, const char *name, image_t *out) {
    memset(out, 0, sizeof(*out));
    const asset_t *a = asset_find(cat, name);
    if (!a) { os_diag("image missing: cat %d '%s'", cat, name); return false; }
    size_t len = 0;
    uint8_t *b = (uint8_t *)plat_load_file(a->path, &len, IMG_MAX_BYTES);
    if (!b) return false;
    reader_t r;
    rd_init(&r, b, len);
    const uint8_t *m = rd_bytes(&r, 4);
    out->w = rd_u16(&r); out->h = rd_u16(&r);
    if (!m || memcmp(m, "OSI1", 4) || !out->w || !out->h || out->w > 2048 || out->h > 2048) { free(b); return false; }
    for (int i = 0; i < 256; i++) out->pal[i] = rd_u16(&r);
    out->px = rd_bytes(&r, (size_t)out->w * out->h);
    if (r.err) { free(b); memset(out, 0, sizeof(*out)); return false; }
    out->buf = b;
    return true;
}

void image_free(image_t *im) { free(im->buf); memset(im, 0, sizeof(*im)); }

/* ---------------------------------------------------------------- big images
 * The PVR cannot hold a texture wider/taller than 1024 px, but several OneShot
 * panoramas are map-sized (mine 1120x800, maize 1408x1280).  Such images stay
 * indexed in main RAM and only the on-screen part is converted into one fixed
 * 512x512 "window" texture (512 KB VRAM), refreshed when the visible source area
 * leaves it (every ~6-8 tiles of camera movement). */
#define BIG_LIMIT 1024
#define WIN_W 512
#define WIN_H 512
struct big_img {
    image_t im;
    bool keyed;
    int wx, wy, ww, wh;    /* source rectangle currently in the window texture */
    bool filled;
    uint16_t *px;          /* ww*wh conversion buffer */
};
static int s_big_uploads;
int gfx_big_window_uploads(void) { return s_big_uploads; }

static void big_fill(big_img_t *b, tex_t *t, int x0, int y0) {
    b->wx = clampi(x0, 0, b->im.w - b->ww);
    b->wy = clampi(y0, 0, b->im.h - b->wh);
    for (int y = 0; y < b->wh; y++) {
        const uint8_t *src = b->im.px + (size_t)(b->wy + y) * b->im.w + b->wx;
        uint16_t *dst = b->px + (size_t)y * b->ww;
        for (int x = 0; x < b->ww; x++) { uint8_t ix = src[x]; dst[x] = (b->keyed && ix == 0) ? 0 : b->im.pal[ix]; }
    }
    plat_tex_update(t, b->ww, b->wh, b->px);
    b->filled = true;
    s_big_uploads++;
}

static void big_free(gtex_t *g) {
    if (!g->big) return;
    image_free(&g->big->im);
    free(g->big->px);
    free(g->big);
    g->big = NULL;
}

/* Draw part of a big image: clip to the screen, make sure the window holds the
 * visible source pixels, draw from the window. */
static void big_draw(const gtex_t *t, int sx, int sy, int sw, int sh, float dx, float dy, float dw, float dh, uint32_t color, uint32_t add) {
    big_img_t *b = t->big;
    if (sw <= 0 || sh <= 0 || dw <= 0 || dh <= 0) return;
    float fx = dw / (float)sw, fy = dh / (float)sh;
    float cx0 = dx < 0 ? 0 : dx, cy0 = dy < 0 ? 0 : dy;
    float cx1 = dx + dw > SCREEN_W ? SCREEN_W : dx + dw, cy1 = dy + dh > SCREEN_H ? SCREEN_H : dy + dh;
    if (cx1 <= cx0 || cy1 <= cy0) return;
    int ux0 = sx + (int)((cx0 - dx) / fx), uy0 = sy + (int)((cy0 - dy) / fy);
    int ux1 = sx + (int)((cx1 - dx) / fx + 0.999f), uy1 = sy + (int)((cy1 - dy) / fy + 0.999f);
    ux1 = clampi(ux1, ux0 + 1, b->im.w); uy1 = clampi(uy1, uy0 + 1, b->im.h);
    if (ux1 - ux0 > b->ww) ux1 = ux0 + b->ww;
    if (uy1 - uy0 > b->wh) uy1 = uy0 + b->wh;
    if (!b->filled || ux0 < b->wx || uy0 < b->wy || ux1 > b->wx + b->ww || uy1 > b->wy + b->wh)
        big_fill(b, t->tex, ux0 - (b->ww - (ux1 - ux0)) / 2, uy0 - (b->wh - (uy1 - uy0)) / 2);
    plat_draw(t->tex, ux0 - b->wx, uy0 - b->wy, ux1 - ux0, uy1 - uy0,
              dx + (ux0 - sx) * fx, dy + (uy0 - sy) * fy, (ux1 - ux0) * fx, (uy1 - uy0) * fy, color, add);
}

/* ---------------------------------------------------------------- cache */
typedef struct {
    bool used;
    int cat;
    char name[NAME_LEN];
    bool keyed;
    tex_life_t life;
    gtex_t g;
} cache_ent_t;
static cache_ent_t s_cache[TEX_CACHE];

static tex_t *upload_image(const image_t *im, bool keyed) {
    size_t n = (size_t)im->w * im->h;
    uint16_t *px = (uint16_t *)malloc(n * 2);
    if (!px) return NULL;
    for (size_t i = 0; i < n; i++) {
        uint8_t ix = im->px[i];
        px[i] = (keyed && ix == 0) ? 0 : im->pal[ix];
    }
    tex_t *t = plat_tex_create(im->w, im->h, px);
    free(px);
    return t;
}

const gtex_t *gfx_tex(int cat, const char *name, bool keyed, tex_life_t life) {
    static gtex_t invalid;
    if (!name || !name[0]) return &invalid;
    int free_slot = -1;
    for (int i = 0; i < TEX_CACHE; i++) {
        cache_ent_t *e = &s_cache[i];
        if (!e->used) { if (free_slot < 0) free_slot = i; continue; }
        if (e->cat == cat && e->keyed == keyed && os_stricmp(e->name, name) == 0) {
            if (life == LIFE_PERSISTENT) e->life = LIFE_PERSISTENT;
            return &e->g;
        }
    }
    if (free_slot < 0) {
        /* evict a room texture (cache is bounded) */
        for (int i = 0; i < TEX_CACHE; i++)
            if (s_cache[i].life == LIFE_ROOM) { plat_tex_destroy(s_cache[i].g.tex); big_free(&s_cache[i].g); s_cache[i].used = false; free_slot = i; break; }
        if (free_slot < 0) return &invalid;
    }
    cache_ent_t *e = &s_cache[free_slot];
    memset(e, 0, sizeof(*e));
    e->used = true; e->cat = cat; e->keyed = keyed; e->life = life;
    os_strlcpy(e->name, name, sizeof(e->name));
    image_t im;
    if (image_load(cat, name, &im)) {
        e->g.w = im.w; e->g.h = im.h;
        if (im.w > BIG_LIMIT || im.h > BIG_LIMIT) {
            big_img_t *b = (big_img_t *)calloc(1, sizeof(big_img_t));
            int ww = im.w < WIN_W ? im.w : WIN_W, wh = im.h < WIN_H ? im.h : WIN_H;
            uint16_t *px = b ? (uint16_t *)calloc((size_t)ww * wh, 2) : NULL;
            tex_t *t = px ? plat_tex_create(ww, wh, px) : NULL;
            if (t) {
                b->im = im; b->keyed = keyed; b->ww = ww; b->wh = wh; b->px = px;
                e->g.tex = t; e->g.big = b; e->g.valid = true;
                os_diag("big image %dx%d '%s': streamed through a %dx%d window", im.w, im.h, name, ww, wh);
            } else { free(px); free(b); image_free(&im); }
        } else {
            e->g.tex = upload_image(&im, keyed);
            e->g.valid = e->g.tex != NULL;
            image_free(&im);
        }
    }
    if (!e->g.valid) { s_tex_failures++; os_diag("texture unavailable: cat %d '%s'", cat, name); }
    return &e->g;
}

void gfx_purge_room(void) {
    for (int i = 0; i < TEX_CACHE; i++)
        if (s_cache[i].used && s_cache[i].life == LIFE_ROOM) {
            plat_tex_destroy(s_cache[i].g.tex);
            big_free(&s_cache[i].g);
            s_cache[i].used = false;
        }
}
void gfx_purge_all(void) {
    for (int i = 0; i < TEX_CACHE; i++)
        if (s_cache[i].used) { plat_tex_destroy(s_cache[i].g.tex); big_free(&s_cache[i].g); s_cache[i].used = false; }
    s_sys_tex = NULL;
}
int gfx_cache_count(void) { int n = 0; for (int i = 0; i < TEX_CACHE; i++) n += s_cache[i].used; return n; }

void gfx_draw_tex(const gtex_t *t, int sx, int sy, int sw, int sh, float dx, float dy, float dw, float dh, uint32_t color, uint32_t add) {
    if (!t || !t->valid) return;
    if (t->big) { big_draw(t, sx, sy, sw, sh, dx, dy, dw, dh, color, add); return; }
    plat_draw(t->tex, sx, sy, sw, sh, dx, dy, dw, dh, color, add);
}

/* ---------------------------------------------------------------- canvas */
bool canvas_init(canvas_t *c, int w, int h) {
    memset(c, 0, sizeof(*c));
    c->px = (uint16_t *)calloc((size_t)w * h, 2);
    if (!c->px) return false;
    c->w = w; c->h = h; c->dirty = true;
    return true;
}
void canvas_free(canvas_t *c) { if (c->tex) plat_tex_destroy(c->tex); free(c->px); memset(c, 0, sizeof(*c)); }
void canvas_clear(canvas_t *c) { memset(c->px, 0, (size_t)c->w * c->h * 2); c->dirty = true; }
void canvas_fill(canvas_t *c, int x, int y, int w, int h, uint16_t col) {
    for (int j = y; j < y + h; j++) {
        if (j < 0 || j >= c->h) continue;
        for (int i = x; i < x + w; i++) if (i >= 0 && i < c->w) c->px[j * c->w + i] = col;
    }
    c->dirty = true;
}

uint16_t gfx_text_color_sample(int color, int gx, int gy) {
    if (!s_sys_ok) return color == 0 ? 0xFFFF : 0x801F;
    if (color < 0 || color > 19) color = 0;
    int x = (color % 10) * 16 + 2 + gx, y = 48 + (color / 10) * 16 + 4 + gy;
    if (x >= s_sys.w || y >= s_sys.h) return 0xFFFF;
    return s_sys.pal[s_sys.px[y * s_sys.w + x]] | 0x8000;
}

static uint16_t shadow_color(void) {
    if (!s_sys_ok || s_sys.w < 17 || s_sys.h < 33) return 0x8000;
    return s_sys.pal[s_sys.px[32 * s_sys.w + 16]] | 0x8000;
}

static uint16_t s_plain;
void canvas_set_plain_color(uint16_t c) { s_plain = c; }

int canvas_char(canvas_t *c, int x, int y, uint8_t ch, int color) {
    const uint8_t *g = &s_font[ch * 12];
    if (s_plain) {
        uint16_t col = color == 1 ? 0x8000 | (12 << 10) | (12 << 5) | 16 : color == 2 ? 0x8000 | (22 << 10) | (4 << 5) | 6 : s_plain;
        for (int row = 0; row < 12; row++)
            for (int cx = 0; cx < 6; cx++)
                if ((g[row] & (0x80 >> cx)) && x + cx >= 0 && y + row >= 0 && x + cx < c->w && y + row < c->h)
                    c->px[(y + row) * c->w + x + cx] = col;
        c->dirty = true;
        return x + 6;
    }
    uint16_t sh = shadow_color();
    for (int pass = 0; pass < 2; pass++) {
        int ox = pass == 0 ? 1 : 0, oy = pass == 0 ? 1 : 0;
        for (int row = 0; row < 12; row++) {
            uint8_t bits = g[row];
            for (int col = 0; col < 6; col++) {
                if (!(bits & (0x80 >> col))) continue;
                int px = x + col + ox, py = y + row + oy;
                if (px < 0 || py < 0 || px >= c->w || py >= c->h) continue;
                c->px[py * c->w + px] = pass == 0 ? sh : gfx_text_color_sample(color, col, row);
            }
        }
    }
    c->dirty = true;
    return x + 6;
}

int canvas_text(canvas_t *c, int x, int y, const char *s, int color) {
    for (; *s; s++) x = canvas_char(c, x, y, (uint8_t)*s, color);
    return x;
}

int text_width(const char *s) { return (int)strlen(s) * 6; }

void canvas_blit_image(canvas_t *c, int dx, int dy, const image_t *im, int sx, int sy, int sw, int sh, bool keyed) {
    for (int j = 0; j < sh; j++) {
        int ty = dy + j, iy = sy + j;
        if (ty < 0 || ty >= c->h || iy < 0 || iy >= im->h) continue;
        for (int i = 0; i < sw; i++) {
            int tx = dx + i, ix = sx + i;
            if (tx < 0 || tx >= c->w || ix < 0 || ix >= im->w) continue;
            uint8_t p = im->px[iy * im->w + ix];
            if (keyed && p == 0) continue;
            c->px[ty * c->w + tx] = im->pal[p] | 0x8000;
        }
    }
    c->dirty = true;
}

void canvas_draw(canvas_t *c, int x, int y, uint32_t color) {
    if (c->dirty) {
        if (!c->tex || !plat_tex_update(c->tex, c->w, c->h, c->px)) {
            if (c->tex) plat_tex_destroy(c->tex);
            c->tex = plat_tex_create(c->w, c->h, c->px);
        }
        c->dirty = false;
    }
    if (c->tex) plat_draw(c->tex, 0, 0, c->w, c->h, (float)x, (float)y, (float)c->w, (float)c->h, color, 0);
}

/* ---------------------------------------------------------------- system */
bool gfx_init(void) {
    size_t len = 0;
    uint8_t *f = (uint8_t *)plat_load_file("font.bin", &len, sizeof(s_font));
    if (!f || len != sizeof(s_font)) { os_log("font.bin missing or wrong size"); free(f); return false; }
    memcpy(s_font, f, sizeof(s_font));
    free(f);
    return true;
}

bool gfx_set_system(const char *name) {
    if (s_sys_ok && os_stricmp(name, s_sys_name) == 0) return true;
    image_t im;
    if (!image_load(CAT_SYSTEM, name, &im)) { os_log("system graphic '%s' missing", name); return false; }
    if (s_sys_ok) image_free(&s_sys);
    s_sys = im; s_sys_ok = true;
    os_strlcpy(s_sys_name, name, sizeof(s_sys_name));
    s_sys_tex = gfx_tex(CAT_SYSTEM, name, true, LIFE_PERSISTENT);
    return true;
}
const char *gfx_system_name(void) { return s_sys_name; }

void gfx_window(int x, int y, int w, int h, int opacity, bool frame) {
    if (!s_sys_tex || !s_sys_tex->valid || w < 16 || h < 16) { plat_fill(x, y, w, h, (uint32_t)opacity << 24); return; }
    const gtex_t *t = s_sys_tex;
    uint32_t col = ((uint32_t)opacity << 24) | 0xFFFFFF;
    /* background (stretched, RPG Maker default) */
    gfx_draw_tex(t, 0, 0, 32, 32, (float)x + 2, (float)y + 2, (float)w - 4, (float)h - 4, col, 0);
    if (!frame) return;
    uint32_t fc = 0xFFFFFFFF;
    /* corners */
    gfx_draw_tex(t, 32, 0, 8, 8, (float)x, (float)y, 8, 8, fc, 0);
    gfx_draw_tex(t, 56, 0, 8, 8, (float)(x + w - 8), (float)y, 8, 8, fc, 0);
    gfx_draw_tex(t, 32, 24, 8, 8, (float)x, (float)(y + h - 8), 8, 8, fc, 0);
    gfx_draw_tex(t, 56, 24, 8, 8, (float)(x + w - 8), (float)(y + h - 8), 8, 8, fc, 0);
    /* edges (stretched) */
    gfx_draw_tex(t, 40, 0, 16, 8, (float)(x + 8), (float)y, (float)(w - 16), 8, fc, 0);
    gfx_draw_tex(t, 40, 24, 16, 8, (float)(x + 8), (float)(y + h - 8), (float)(w - 16), 8, fc, 0);
    gfx_draw_tex(t, 32, 8, 8, 16, (float)x, (float)(y + 8), 8, (float)(h - 16), fc, 0);
    gfx_draw_tex(t, 56, 8, 8, 16, (float)(x + w - 8), (float)(y + 8), 8, (float)(h - 16), fc, 0);
}

void gfx_cursor(int x, int y, int w, int h, int blink_frame) {
    if (!s_sys_tex || !s_sys_tex->valid || w < 16 || h < 16) { plat_fill(x, y, w, h, 0x60FFFFFF); return; }
    const gtex_t *t = s_sys_tex;
    int bx = (blink_frame & 32) ? 96 : 64;
    uint32_t c = 0xFFFFFFFF;
    gfx_draw_tex(t, bx + 8, 8, 16, 16, (float)(x + 8), (float)(y + 8), (float)(w - 16), (float)(h - 16), c, 0);
    gfx_draw_tex(t, bx, 0, 8, 8, (float)x, (float)y, 8, 8, c, 0);
    gfx_draw_tex(t, bx + 24, 0, 8, 8, (float)(x + w - 8), (float)y, 8, 8, c, 0);
    gfx_draw_tex(t, bx, 24, 8, 8, (float)x, (float)(y + h - 8), 8, 8, c, 0);
    gfx_draw_tex(t, bx + 24, 24, 8, 8, (float)(x + w - 8), (float)(y + h - 8), 8, 8, c, 0);
    gfx_draw_tex(t, bx + 8, 0, 16, 8, (float)(x + 8), (float)y, (float)(w - 16), 8, c, 0);
    gfx_draw_tex(t, bx + 8, 24, 16, 8, (float)(x + 8), (float)(y + h - 8), (float)(w - 16), 8, c, 0);
    gfx_draw_tex(t, bx, 8, 8, 16, (float)x, (float)(y + 8), 8, (float)(h - 16), c, 0);
    gfx_draw_tex(t, bx + 24, 8, 8, 16, (float)(x + w - 8), (float)(y + 8), 8, (float)(h - 16), c, 0);
}
