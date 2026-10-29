/* SPDX-License-Identifier: GPL-3.0-or-later */
#include "banim.h"
#include "../renderer/gfx.h"
#include "../audio/audio.h"
#include "../resources/gamedata.h"
#include "../platform/platform.h"
#include <stdlib.h>

#define BA_MAX 64
#define BA_MAX_CELLS 16

typedef struct { uint8_t valid, id; int16_t x, y; uint16_t zoom; uint8_t tr, tg, tb, tgray, transp; } ba_cell_t;
typedef struct { uint8_t n; ba_cell_t c[BA_MAX_CELLS]; } ba_frame_t;
typedef struct { uint16_t frame; char se[NAME_LEN]; uint8_t vol, tempo, pan, flash_scope, r, g, b, power, shake; } ba_timing_t;
typedef struct {
    uint16_t id; char file[NAME_LEN]; uint8_t scope, position, large;
    uint16_t nframes; ba_frame_t *frames;
    uint16_t ntim; ba_timing_t *tim;
} ba_anim_t;

static ba_anim_t A[BA_MAX];
static int nA;
static bool tried;

static struct {
    const ba_anim_t *a;
    const character_t *target;
    int frame;                 /* game frames since start */
    int flash_t, flash_idx;
} P;

static void load(void) {
    if (tried) return;
    tried = true;
    size_t len = 0;
    uint8_t *buf = (uint8_t *)plat_load_file("anim.dat", &len, 512 * 1024);
    if (!buf) { os_log("banim: anim.dat missing (animations not drawn)"); return; }
    reader_t r; rd_init(&r, buf, len);
    const uint8_t *m = rd_bytes(&r, 4);
    if (!m || memcmp(m, "OSAN", 4) || rd_u16(&r) != 1) { os_log("banim: bad anim.dat"); free(buf); return; }
    int n = rd_u16(&r);
    for (int i = 0; i < n && !r.err; i++) {
        ba_anim_t a; memset(&a, 0, sizeof(a));
        a.id = rd_u16(&r); rd_str(&r, a.file, NAME_LEN);
        a.scope = rd_u8(&r); a.position = rd_u8(&r); a.large = rd_u8(&r);
        a.nframes = rd_u16(&r);
        a.frames = a.nframes ? (ba_frame_t *)calloc(a.nframes, sizeof(ba_frame_t)) : NULL;
        for (int f = 0; f < a.nframes && !r.err; f++) {
            int nc = rd_u8(&r);
            for (int k = 0; k < nc; k++) {
                ba_cell_t c;
                c.valid = rd_u8(&r); c.id = rd_u8(&r); c.x = rd_i16(&r); c.y = rd_i16(&r); c.zoom = rd_u16(&r);
                c.tr = rd_u8(&r); c.tg = rd_u8(&r); c.tb = rd_u8(&r); c.tgray = rd_u8(&r); c.transp = rd_u8(&r);
                if (a.frames && k < BA_MAX_CELLS) { a.frames[f].c[k] = c; a.frames[f].n = (uint8_t)(k + 1); }
            }
        }
        a.ntim = rd_u16(&r);
        a.tim = a.ntim ? (ba_timing_t *)calloc(a.ntim, sizeof(ba_timing_t)) : NULL;
        for (int t = 0; t < a.ntim && !r.err; t++) {
            ba_timing_t x; memset(&x, 0, sizeof(x));
            x.frame = rd_u16(&r); rd_str(&r, x.se, NAME_LEN);
            x.vol = rd_u8(&r); x.tempo = rd_u8(&r); x.pan = rd_u8(&r); x.flash_scope = rd_u8(&r);
            x.r = rd_u8(&r); x.g = rd_u8(&r); x.b = rd_u8(&r); x.power = rd_u8(&r); x.shake = rd_u8(&r);
            if (a.tim) a.tim[t] = x;
        }
        if (nA < BA_MAX && !r.err) A[nA++] = a; else { free(a.frames); free(a.tim); }
    }
    if (r.err) os_log("banim: anim.dat truncated (%d animations kept)", nA);
    free(buf);
}

int banim_count(void) { load(); return nA; }
bool banim_active(void) { return P.a != NULL; }
void banim_stop(void) { memset(&P, 0, sizeof(P)); P.flash_idx = -1; }

int banim_start(int anim_id, const character_t *target) {
    load();
    const ba_anim_t *a = NULL;
    for (int i = 0; i < nA; i++) if (A[i].id == anim_id) a = &A[i];
    if (!a) { os_diag("banim: animation %d not found", anim_id); banim_stop(); return 0; }
    P.a = a; P.target = target; P.frame = 0; P.flash_idx = -1; P.flash_t = 0;
    return a->nframes * 2;
}

void banim_update(void) {
    if (!P.a) return;
    int real = P.frame / 2;
    if (real >= P.a->nframes) { banim_stop(); return; }
    if ((P.frame & 1) == 0)
        for (int t = 0; t < P.a->ntim; t++) {
            const ba_timing_t *x = &P.a->tim[t];
            if (x->frame != real + 1) continue;
            if (x->se[0] && os_stricmp(x->se, "(OFF)")) audio_se_play(x->se, x->vol, x->pan);
            if (x->flash_scope) { P.flash_idx = t; P.flash_t = 0; }   /* target or screen: shown as a screen flash */
        }
    if (P.flash_idx >= 0) {   /* EasyRPG CalculateFlashPower */
        const ba_timing_t *x = &P.a->tim[P.flash_idx];
        int f = 7 - ((P.flash_t + 1) / 2), pw = f * x->power / 6;
        if (P.flash_t > 10 || pw <= 0) P.flash_idx = -1;
        else {
            if (pw > 31) pw = 31;
            GS.flash_rgb[0] = x->r * 255 / 31; GS.flash_rgb[1] = x->g * 255 / 31; GS.flash_rgb[2] = x->b * 255 / 31;
            GS.flash_level = pw * 255 / 31; GS.flash_frames = 1;
        }
        P.flash_t++;
    }
    P.frame++;
}

void banim_draw(void) {
    if (!P.a) return;
    int real = P.frame / 2;
    if (real >= P.a->nframes || !P.a->frames) return;
    const gtex_t *t = gfx_tex(CAT_BATTLE, P.a->file, true, LIFE_ROOM);
    if (!t->valid) return;
    float x0, y0;
    if (P.a->scope == 1 || !P.target) { x0 = SCREEN_W / 2; y0 = SCREEN_H / 2; }
    else {
        const character_t *c = P.target;
        x0 = (float)(c->rx - GS.display_x) * TILE / REAL_TILE + TILE / 2 + GS.shake_ofs;
        float bottom = (float)(c->ry - GS.display_y) * TILE / REAL_TILE + TILE;
        y0 = bottom - 12 + (P.a->position == 0 ? -12 : P.a->position == 2 ? 12 : 0);
    }
    int size = P.a->large ? 128 : 96;
    const ba_frame_t *f = &P.a->frames[real];
    for (int k = 0; k < f->n; k++) {
        const ba_cell_t *c = &f->c[k];
        if (!c->valid) continue;
        float s = (float)size * c->zoom / 100.0f;
        int sx = (c->id % 5) * size, sy = (c->id / 5) * size;
        if (sx + size > t->w || sy + size > t->h) continue;
        uint32_t a = (uint32_t)(255 * (100 - c->transp) / 100);
        int mr = c->tr > 100 ? 255 : c->tr * 255 / 100, mg = c->tg > 100 ? 255 : c->tg * 255 / 100, mb = c->tb > 100 ? 255 : c->tb * 255 / 100;
        int ar = c->tr > 100 ? (c->tr - 100) * 255 / 100 : 0, ag = c->tg > 100 ? (c->tg - 100) * 255 / 100 : 0, ab = c->tb > 100 ? (c->tb - 100) * 255 / 100 : 0;
        gfx_draw_tex(t, sx, sy, size, size, x0 + c->x - s / 2, y0 + c->y - s / 2, s, s,
                     a << 24 | (uint32_t)mr << 16 | (uint32_t)mg << 8 | (uint32_t)mb,
                     (uint32_t)ar << 16 | (uint32_t)ag << 8 | (uint32_t)ab);
    }
}
