/* SPDX-License-Identifier: GPL-3.0-or-later
 * Map, characters, movement, passability, triggers and map drawing.
 * Semantics follow RPG Maker 2003 as documented by EasyRPG Player
 * (GPL-3.0-or-later); this is an independent C implementation. */
#include "game.h"
#include "banim.h"
#include "../renderer/gfx.h"
#include "../events/interp.h"
#include "../audio/audio.h"
#include "../platform/platform.h"
#include "../util.h"
#include <stdlib.h>
#include <stdio.h>

static mapdata_t M;
static bool s_loaded;
/* Maps left behind by a teleport while an event was still running: the running
 * event's command lists live inside them, so they are freed only once the main
 * interpreter is idle (map_gc_retired). */
static mapdata_t s_retired[3];
static int s_nretired;

enum { PASS_DOWN = 1, PASS_LEFT = 2, PASS_RIGHT = 4, PASS_UP = 8, PASS_ABOVE = 16, PASS_WALL = 32 };
static const int DX[4] = {0, 1, 0, -1}, DY[4] = {-1, 0, 1, 0};
static int dir_bit(int d) { return d == DIR_UP ? PASS_UP : d == DIR_RIGHT ? PASS_RIGHT : d == DIR_DOWN ? PASS_DOWN : PASS_LEFT; }

const mapdata_t *map_data(void) { return s_loaded ? &M : NULL; }
int map_w(void) { return s_loaded ? M.w : 0; }
int map_h(void) { return s_loaded ? M.h : 0; }

static const chipset_def_t *chip(void) {
    return (M.chipset_id >= 1 && M.chipset_id <= G.n_chipsets) ? &G.chipsets[M.chipset_id] : NULL;
}

static int lower_pass_index(int id) {
    if (id >= 5000) return 18 + (id - 5000);
    if (id >= 4000) return 6 + (id - 4000) / 50;
    if (id >= 3000) return 3 + (id - 3000) / 50;
    return id / 1000;
}

static uint8_t lower_flags(int x, int y) {
    const chipset_def_t *c = chip();
    if (!c) return 0;
    int idx = lower_pass_index(M.lower[y * M.w + x]);
    return (idx >= 0 && idx < 162) ? c->pass_lower[idx] : 0;
}
static uint8_t upper_flags(int x, int y) {
    const chipset_def_t *c = chip();
    if (!c) return 0;
    int idx = M.upper[y * M.w + x] - 10000;
    return (idx >= 0 && idx < 144) ? c->pass_upper[idx] : 0;
}

static bool lower_passable(int x, int y, int bit) {
    int id = M.lower[y * M.w + x];
    uint8_t f = lower_flags(x, y);
    if (id >= 4000 && id < 5000 && (f & PASS_WALL)) {
        int a = (id - 4000) % 50;
        if ((a >= 20 && a <= 23) || (a >= 33 && a <= 37) || a == 42 || a == 43 || a == 45 || a == 46) return true;
    }
    return (f & bit) != 0;
}

static bool tile_passable(const character_t *self, int x, int y, int bit) {
    if (x < 0 || y < 0 || x >= M.w || y >= M.h) return false;
    /* below-layer tile-graphic events override the chipset */
    int ev_tile = 0;
    for (int i = 0; i < GS.nev; i++) {
        const character_t *e = &GS.ev[i];
        if (e == self || !e->active || e->through) continue;
        if (e->x == x && e->y == y && e->layer == LAYER_BELOW && e->tile_sprite) ev_tile = e->sprite_index;
    }
    const chipset_def_t *c = chip();
    if (ev_tile > 0 && c && ev_tile < 144 && !(c->pass_upper[ev_tile] & PASS_ABOVE)) return (c->pass_upper[ev_tile] & bit) != 0;
    uint8_t uf = upper_flags(x, y);
    if (!(uf & bit)) return false;
    if (!(uf & PASS_ABOVE)) return true;
    return lower_passable(x, y, bit);
}

static bool blocked_by_char(const character_t *self, int x, int y) {
    if (self->layer != LAYER_SAME) return false;
    for (int i = 0; i < GS.nev; i++) {
        const character_t *e = &GS.ev[i];
        if (e == self || !e->active || e->through || e->layer != LAYER_SAME) continue;
        if (e->x == x && e->y == y) return true;
    }
    if (self != &GS.player && !GS.player.through && GS.player.x == x && GS.player.y == y) return true;
    return false;
}

bool map_tile_passable_dir(int x, int y, int dir) { return tile_passable(&GS.player, x, y, dir_bit(dir)); }

bool map_passable_from(const character_t *c, int x, int y, int dir) {
    int nx = x + DX[dir], ny = y + DY[dir];
    if (nx < 0 || ny < 0 || nx >= M.w || ny >= M.h) return false;
    if (c->through) return true;
    if (!tile_passable(c, x, y, dir_bit(dir))) return false;
    if (!tile_passable(c, nx, ny, dir_bit((dir + 2) & 3))) return false;
    if (blocked_by_char(c, nx, ny)) return false;
    return true;
}

/* ---------------------------------------------------------------- pages */
static bool page_ok(const page_def_t *p) {
    if ((p->cond_flags & 0x01) && !game_sw(p->switch_a)) return false;
    if ((p->cond_flags & 0x02) && !game_sw(p->switch_b)) return false;
    if (p->cond_flags & 0x04) {
        int32_t v = game_var(p->var_id), t = p->var_value;
        bool ok;
        switch (p->compare_op) {
        case 0: ok = v == t; break;
        case 1: ok = v >= t; break;
        case 2: ok = v <= t; break;
        case 3: ok = v > t; break;
        case 4: ok = v < t; break;
        case 5: ok = v != t; break;
        default: ok = v >= t;
        }
        if (!ok) return false;
    }
    if ((p->cond_flags & 0x08) && (p->item_id > MAX_ITEMS || !GS.items[p->item_id])) return false;
    if (p->cond_flags & 0x10) {   /* actor in the party (used by Map 34 "Calamus" page 4: Alula) */
        bool in = false;
        for (int i = 0; i < GS.party_n; i++) in |= GS.party[i] == p->actor_id;
        if (!in) return false;
    }
    return true;
}

static int speed_step(int s) { s = clampi(s, 1, 6); return 1 << (1 + s); }

static void apply_page(character_t *c, const event_def_t *d, int pi) {
    if (pi < 0) {
        c->active = false; c->page = -1; c->sprite[0] = 0; c->tile_sprite = false; c->trigger = -1;
        c->route = NULL; c->route_len = 0;
        return;
    }
    const page_def_t *p = &d->pages[pi];
    bool changed = c->page != pi || !c->active;
    c->active = true;
    c->page = pi;
    if (!changed) return;
    os_strlcpy(c->sprite, p->charset, sizeof(c->sprite));
    c->tile_sprite = p->charset[0] == 0;
    c->sprite_index = p->char_index;
    c->dir = c->facing = (p->direction == 8 ? DIR_UP : p->direction == 6 ? DIR_RIGHT : p->direction == 4 ? DIR_LEFT : DIR_DOWN);
    /* LCF stores direction as 0..3 (up,right,down,left) in 2k3 data */
    if (p->direction <= 3) c->dir = c->facing = p->direction;
    c->anim_frame = p->pattern <= 3 ? p->pattern : 1;
    c->opacity = p->translucent ? 160 : 255;
    c->move_type = p->move_type;
    c->freq = clampi(p->move_freq, 1, 8);
    c->speed = clampi(p->move_speed, 1, 6);
    c->trigger = p->trigger;
    c->layer = p->layer;
    c->overlap_forbidden = p->overlap_forbidden;
    c->through = false;
    /* anim_type: 0 standing, 1 walking, 2 fixed dir+standing, 3 fixed dir+walking, 4 fixed graphic, 5 spin */
    c->step_anim = p->anim_type == 1 || p->anim_type == 3;
    c->dir_fixed = p->anim_type == 2 || p->anim_type == 3 || p->anim_type == 4;
    c->anim_paused = p->anim_type == 4;
    c->spinning = p->anim_type == 5;
    c->route_forced = false;
    if (p->move_type == 6 && p->route_len) {
        c->route = p->route; c->route_len = p->route_len; c->route_index = 0;
        c->route_repeat = p->route_repeat; c->route_skip = p->route_skippable;
    } else {
        c->route = NULL; c->route_len = 0;
    }
    c->max_stop_count = (c->freq >= 8) ? 0 : 1 << (9 - c->freq);
    c->stop_count = 0;
}

void map_refresh_pages(void) {
    if (!s_loaded) return;
    for (int i = 0; i < M.nevents && i < GS.nev; i++) {
        character_t *c = &GS.ev[i];
        if (c->erased) { c->active = false; continue; }
        const event_def_t *d = &M.events[i];
        int found = -1;
        for (int p = d->npages - 1; p >= 0; p--) if (page_ok(&d->pages[p])) { found = p; break; }
        int before = c->active ? c->page : -2;
        apply_page(c, d, found);
        if (before != (c->active ? c->page : -2)) interp_event_page_changed(c);
    }
    interp_refresh_parallels();
    GS.need_refresh = false;
}

/* ---------------------------------------------------------------- setup */
void map_unload(void) {
    interp_clear_map();
    for (int i = 0; i < s_nretired; i++) mapdata_free(&s_retired[i]);
    s_nretired = 0;
    if (s_loaded) mapdata_free(&M);
    s_loaded = false;
    GS.nev = 0;
    gfx_purge_room();
    audio_purge_room();
}

void map_gc_retired(void) {
    if (interp_blocks_events() || !s_nretired) return;
    for (int i = 0; i < s_nretired; i++) mapdata_free(&s_retired[i]);
    s_nretired = 0;
}

static bool map_setup_common(int map_id);

bool map_setup_keep_interp(int map_id) {
    if (!interp_blocks_events() || !s_loaded) return map_setup(map_id);
    if (s_nretired == 3) { os_log("map: too many nested teleports in one event"); return false; }
    s_retired[s_nretired++] = M;
    memset(&M, 0, sizeof(M));
    s_loaded = false;
    interp_clear_parallels();
    interp_on_map_changed();
    GS.nev = 0;
    gfx_purge_room();
    audio_purge_room();
    return map_setup_common(map_id);
}

bool map_setup(int map_id) {
    map_unload();
    return map_setup_common(map_id);
}

static bool map_setup_common(int map_id) {
    banim_stop();   /* an animation never outlives its room (its target is gone) */
    if (!mapdata_load(map_id, &M)) return false;
    s_loaded = true;
    GS.map_id = map_id;
    GS.nev = M.nevents;
    for (int i = 0; i < M.nevents; i++) {
        character_t *c = &GS.ev[i];
        memset(c, 0, sizeof(*c));
        c->id = M.events[i].id;
        c->x = M.events[i].x; c->y = M.events[i].y;
        c->rx = c->x * REAL_TILE; c->ry = c->y * REAL_TILE;
        c->page = -1;
        c->opacity = 255;
    }
    os_strlcpy(GS.parallax, M.parallax_flag ? M.parallax_name : "", sizeof(GS.parallax));
    GS.parallax_ox = GS.parallax_oy = 0;
    /* map BGM from the map tree (music_type 2 = specific track) */
    const mapinfo_t *mi = mapinfo_find(map_id);
    if (mi && mi->music_type == 2 && mi->music[0] && os_stricmp(mi->music, "(OFF)")) audio_bgm_play(mi->music, 100, 100);
    map_refresh_pages();
    map_center_on_player();
    os_diag("map %d loaded: %dx%d, %d events, chipset %d", map_id, M.w, M.h, M.nevents, M.chipset_id);
    return true;
}

character_t *map_char(int id, int self_id) {
    if (id == CHAR_PLAYER) return &GS.player;
    if (id == CHAR_THIS) id = self_id;
    for (int i = 0; i < GS.nev; i++) if (GS.ev[i].id == id) return &GS.ev[i];
    return NULL;
}

void map_center_on_player(void) {
    if (!s_loaded) return;
    int x = GS.player.rx - (SCREEN_W / 2 - TILE / 2) * (REAL_TILE / TILE);
    int y = GS.player.ry - (SCREEN_H / 2 - TILE / 2) * (REAL_TILE / TILE) ;
    int maxx = (M.w * TILE - SCREEN_W) * (REAL_TILE / TILE), maxy = (M.h * TILE - SCREEN_H) * (REAL_TILE / TILE);
    if (maxx < 0) x = maxx / 2; else x = clampi(x, 0, maxx);
    if (maxy < 0) y = maxy / 2; else y = clampi(y, 0, maxy);
    GS.display_x = x; GS.display_y = y;
}

/* ---------------------------------------------------------------- movement */
bool char_moving(const character_t *c) { return c->jumping || c->rx != c->x * REAL_TILE || c->ry != c->y * REAL_TILE; }

void char_turn(character_t *c, int dir) {
    c->dir = dir & 3;
    if (!c->dir_fixed) c->facing = c->dir;
}

static void start_touch_events_at(int x, int y, bool from_player_bump);

/* Diagonal step (move commands 4-7: up-right, down-right, down-left, up-left), as
 * RPG_RT / EasyRPG: try vertical-then-horizontal, else horizontal-then-vertical.
 * Facing keeps whichever of the two components it already shows, else reverses. */
static bool char_step_diag(character_t *c, int code) {
    static const int DDX[4] = {1, 1, -1, -1}, DDY[4] = {-1, 1, 1, -1};
    int dx = DDX[code - 4], dy = DDY[code - 4];
    int hd = dx > 0 ? DIR_RIGHT : DIR_LEFT, vd = dy > 0 ? DIR_DOWN : DIR_UP;
    if (!c->dir_fixed && c->facing != hd && c->facing != vd) c->facing = (c->facing + 2) & 3;
    c->dir = c->facing;
    bool ok = c->through
        ? (c->x + dx >= 0 && c->y + dy >= 0 && c->x + dx < M.w && c->y + dy < M.h)
        : ((map_passable_from(c, c->x, c->y, vd) && map_passable_from(c, c->x, c->y + dy, hd)) ||
           (map_passable_from(c, c->x, c->y, hd) && map_passable_from(c, c->x + dx, c->y, vd)));
    if (!ok) return false;
    c->x += dx; c->y += dy;
    c->walking = true;
    return true;
}

/* Jump (move commands 24 ... 25): the moves in between give the landing offset. */
static bool char_jump(character_t *c, int jdx, int jdy) {
    int nx = c->x + jdx, ny = c->y + jdy;
    if (jdx || jdy) {
        int d = abs(jdy) >= abs(jdx) ? (jdy >= 0 ? DIR_DOWN : DIR_UP) : (jdx >= 0 ? DIR_RIGHT : DIR_LEFT);
        char_turn(c, d);
        if (nx < 0 || ny < 0 || nx >= M.w || ny >= M.h) return false;
        if (!c->through && (!tile_passable(c, nx, ny, 0x0F) || blocked_by_char(c, nx, ny))) return false;
    }
    c->x = nx; c->y = ny;
    c->jumping = true; c->jump_rem = REAL_TILE;
    c->walking = true;
    return true;
}

int char_jump_height(const character_t *c) {   /* pixels, EasyRPG Game_Character::GetJumpHeight */
    if (!c->jumping) return 0;
    int r = c->jump_rem, h = (r > REAL_TILE / 2 ? REAL_TILE - r : r) / 8;
    return h < 5 ? h * 2 : h < 13 ? h + 4 : 16;
}

static bool char_step(character_t *c, int dir) {
    char_turn(c, dir);
    if (!map_passable_from(c, c->x, c->y, dir)) {
        if (c == &GS.player) start_touch_events_at(c->x + DX[dir], c->y + DY[dir], true);
        else if (c->trigger == TRIG_COLLISION && GS.player.x == c->x + DX[dir] && GS.player.y == c->y + DY[dir])
            interp_start_event(c);
        return false;
    }
    c->x += DX[dir]; c->y += DY[dir];
    c->walking = true;
    return true;
}

void char_set_route(character_t *c, const movecmd_t *cmds, int n, bool repeat, bool skip) {
    if (n > 64) n = 64;
    memcpy(c->forced, cmds, sizeof(movecmd_t) * (size_t)n);
    c->route = c->forced; c->route_len = n; c->route_index = 0;
    c->route_repeat = repeat; c->route_skip = skip; c->route_forced = true;
    c->wait_count = 0;
    c->stop_count = c->max_stop_count; /* start immediately */
}

bool any_forced_route_running(void) {
    if (GS.player.route_forced) return true;
    for (int i = 0; i < GS.nev; i++) if (GS.ev[i].route_forced) return true;
    return false;
}

static void route_finish(character_t *c) {
    c->route_forced = false;
    c->route = NULL; c->route_len = 0;
    /* fall back to the page's own custom route if any */
    if (c != &GS.player && c->page >= 0 && c->move_type == 6) {
        const event_def_t *d = NULL;
        for (int i = 0; i < M.nevents; i++) if (M.events[i].id == c->id) d = &M.events[i];
        if (d) {
            const page_def_t *p = &d->pages[c->page];
            c->route = p->route; c->route_len = p->route_len; c->route_index = 0;
            c->route_repeat = p->route_repeat; c->route_skip = p->route_skippable;
        }
    }
}

static void route_step(character_t *c) {
    if (!c->route || c->route_len == 0) return;
    if (c->wait_count > 0) { c->wait_count--; return; }
    if (c->stop_count < c->max_stop_count && !c->route_forced) return;
    for (int guard = 0; guard < 64; guard++) {
        if (c->route_index >= c->route_len) {
            if (c->route_repeat) c->route_index = 0;
            else { route_finish(c); return; }
            if (guard) return;
        }
        const movecmd_t *m = &c->route[c->route_index];
        int code = m->code;
        if (code <= 3) {
            if (char_step(c, code == 0 ? DIR_UP : code == 1 ? DIR_RIGHT : code == 2 ? DIR_DOWN : DIR_LEFT) || c->route_skip)
                c->route_index++;
            return;
        }
        if (code <= 7) {
            if (char_step_diag(c, code) || c->route_skip) c->route_index++;
            return;
        }
        if (code == 24) {   /* begin jump: sum the moves up to "end jump" (25) */
            static const int JX[8] = {0, 1, 0, -1, 1, 1, -1, -1}, JY[8] = {-1, 0, 1, 0, -1, 1, 1, -1};
            int jdx = 0, jdy = 0, k = c->route_index + 1;
            for (; k < c->route_len && c->route[k].code != 25; k++) {
                int mc = c->route[k].code;
                if (mc <= 7) { jdx += JX[mc]; jdy += JY[mc]; }
                else if (mc == 11) { jdx += DX[c->dir]; jdy += DY[c->dir]; }
            }
            if (char_jump(c, jdx, jdy) || c->route_skip) c->route_index = k < c->route_len ? k + 1 : k;
            return;
        }
        switch (code) {
        case 8: { int d = os_rand() & 3; if (char_step(c, d) || true) c->route_index++; return; }
        case 9: case 10: {
            int dx = GS.player.x - c->x, dy = GS.player.y - c->y;
            int d = (abs(dx) > abs(dy)) ? (dx > 0 ? DIR_RIGHT : DIR_LEFT) : (dy > 0 ? DIR_DOWN : DIR_UP);
            if (code == 10) d = (d + 2) & 3;
            char_step(c, d); c->route_index++; return;
        }
        case 11: if (char_step(c, c->dir) || c->route_skip) c->route_index++; return;
        case 12: char_turn(c, DIR_UP); break;
        case 13: char_turn(c, DIR_RIGHT); break;
        case 14: char_turn(c, DIR_DOWN); break;
        case 15: char_turn(c, DIR_LEFT); break;
        case 16: char_turn(c, (c->dir + 1) & 3); break;
        case 17: char_turn(c, (c->dir + 3) & 3); break;
        case 18: char_turn(c, (c->dir + 2) & 3); break;
        case 19: char_turn(c, (c->dir + ((os_rand() & 1) ? 1 : 3)) & 3); break;
        case 20: char_turn(c, os_rand() & 3); break;
        case 21: case 22: {
            int dx = GS.player.x - c->x, dy = GS.player.y - c->y;
            int d = (abs(dx) > abs(dy)) ? (dx > 0 ? DIR_RIGHT : DIR_LEFT) : (dy > 0 ? DIR_DOWN : DIR_UP);
            char_turn(c, code == 22 ? (d + 2) & 3 : d); break;
        }
        case 23: c->wait_count = 20 + ((c->freq >= 8) ? 0 : 1 << (8 - c->freq)); c->route_index++; return;
        case 26: c->dir_fixed = true; break;
        case 27: c->dir_fixed = false; break;
        case 28: c->speed = clampi(c->speed + 1, 1, 6); break;
        case 29: c->speed = clampi(c->speed - 1, 1, 6); break;
        case 30: c->freq = clampi(c->freq + 1, 1, 8); c->max_stop_count = c->freq >= 8 ? 0 : 1 << (9 - c->freq); break;
        case 31: c->freq = clampi(c->freq - 1, 1, 8); c->max_stop_count = c->freq >= 8 ? 0 : 1 << (9 - c->freq); break;
        case 32: game_set_sw(m->a, true); GS.need_refresh = true; break;
        case 33: game_set_sw(m->a, false); GS.need_refresh = true; break;
        case 34: os_strlcpy(c->sprite, m->s, sizeof(c->sprite)); c->sprite_index = m->a; c->tile_sprite = m->s[0] == 0; break;
        case 35: audio_se_play(m->s, m->a, m->c); break;
        case 36: c->through = true; break;
        case 37: c->through = false; break;
        case 38: c->anim_paused = true; break;
        case 39: c->anim_paused = false; break;
        case 40: c->opacity = clampi(c->opacity - 45, 0, 255); break;
        case 41: c->opacity = clampi(c->opacity + 45, 0, 255); break;
        default: os_diag("move command %d not implemented (skipped)", code); break;
        }
        c->route_index++;
    }
}

static void update_anim(character_t *c, bool moved_this_frame) {
    int s = clampi(c->speed, 1, 6);
    static const int stationary[] = {12, 10, 8, 6, 5, 4}, continuous[] = {16, 12, 10, 8, 7, 6}, spin[] = {24, 16, 12, 8, 6, 4};
    if (c->spinning) {
        if (++c->anim_count >= spin[s - 1]) { c->facing = (c->facing + 1) & 3; c->anim_count = 0; }
        return;
    }
    if (c->anim_paused) { c->anim_frame = 1; c->anim_count = 0; return; }
    if (c->tile_sprite) return;
    bool continuous_anim = c->step_anim;
    if (continuous_anim || moved_this_frame || c->anim_frame == 0 || c->anim_frame == 2 || c->anim_count < stationary[s - 1] - 1)
        c->anim_count++;
    if (c->anim_count >= continuous[s - 1] || (moved_this_frame && c->anim_count >= stationary[s - 1])) {
        c->anim_frame = (c->anim_frame + 1) & 3;
        c->anim_count = 0;
    }
}

static bool update_char_motion(character_t *c) {
    int tx = c->x * REAL_TILE, ty = c->y * REAL_TILE;
    int st = speed_step(c->speed);
    if (c->jumping) {   /* a jump always lasts one step, even in place */
        c->jump_rem -= st;
        if (c->jump_rem <= 0) { c->jumping = false; c->jump_rem = 0; c->rx = tx; c->ry = ty; c->walking = false; return true; }
        if (c->rx != tx) c->rx += (tx - c->rx) * st / (st + c->jump_rem > 0 ? st + c->jump_rem : 1);
        if (c->ry != ty) c->ry += (ty - c->ry) * st / (st + c->jump_rem > 0 ? st + c->jump_rem : 1);
        return true;
    }
    if (c->rx == tx && c->ry == ty) return false;
    if (c->rx < tx) c->rx = c->rx + st > tx ? tx : c->rx + st;
    if (c->rx > tx) c->rx = c->rx - st < tx ? tx : c->rx - st;
    if (c->ry < ty) c->ry = c->ry + st > ty ? ty : c->ry + st;
    if (c->ry > ty) c->ry = c->ry - st < ty ? ty : c->ry - st;
    if (c->rx == tx && c->ry == ty) c->walking = false;
    return true;
}

static void auto_move(character_t *c) {
    if (c->route_forced || (c->route && c->move_type == 6)) { route_step(c); return; }
    if (c->stop_count < c->max_stop_count) return;
    switch (c->move_type) {
    case 1: if ((os_rand() % 4) == 0) char_turn(c, os_rand() & 3); else char_step(c, os_rand() & 3); c->stop_count = 0; break;
    case 2: if (!char_step(c, c->dir == DIR_DOWN ? DIR_DOWN : DIR_UP)) char_turn(c, (c->dir + 2) & 3); c->stop_count = 0; break;
    case 3: if (!char_step(c, c->dir == DIR_LEFT ? DIR_LEFT : DIR_RIGHT)) char_turn(c, (c->dir + 2) & 3); c->stop_count = 0; break;
    case 4: case 5: {
        int dx = GS.player.x - c->x, dy = GS.player.y - c->y;
        int d = (abs(dx) > abs(dy)) ? (dx > 0 ? DIR_RIGHT : DIR_LEFT) : (dy > 0 ? DIR_DOWN : DIR_UP);
        char_step(c, c->move_type == 5 ? (d + 2) & 3 : d); c->stop_count = 0; break;
    }
    default: break;
    }
}

/* ---------------------------------------------------------------- triggers */
static void start_touch_events_at(int x, int y, bool from_player_bump) {
    if (interp_busy()) return;
    for (int i = 0; i < GS.nev; i++) {
        character_t *e = &GS.ev[i];
        if (!e->active || e->x != x || e->y != y) continue;
        if (e->trigger != TRIG_TOUCH && e->trigger != TRIG_COLLISION) continue;
        if (from_player_bump ? e->layer != LAYER_SAME : e->layer == LAYER_SAME) continue;
        interp_start_event(e);
    }
}

static void check_action_button(void) {
    character_t *p = &GS.player;
    if (interp_busy()) return;
    /* same position, non-same layer */
    for (int i = 0; i < GS.nev; i++) {
        character_t *e = &GS.ev[i];
        if (e->active && e->x == p->x && e->y == p->y && e->layer != LAYER_SAME && e->trigger == TRIG_ACTION) {
            interp_start_event(e); return;
        }
    }
    int fx = p->x + DX[p->facing], fy = p->y + DY[p->facing];
    for (int i = 0; i < GS.nev; i++) {
        character_t *e = &GS.ev[i];
        if (e->active && e->x == fx && e->y == fy && e->layer == LAYER_SAME &&
            (e->trigger == TRIG_ACTION || e->trigger == TRIG_TOUCH || e->trigger == TRIG_COLLISION)) {
            interp_start_event(e); return;
        }
    }
    /* counter tiles: look one further */
    if (fx >= 0 && fy >= 0 && fx < M.w && fy < M.h && (upper_flags(fx, fy) & 0x40)) {
        int gx = fx + DX[p->facing], gy = fy + DY[p->facing];
        for (int i = 0; i < GS.nev; i++) {
            character_t *e = &GS.ev[i];
            if (e->active && e->x == gx && e->y == gy && e->layer == LAYER_SAME && e->trigger == TRIG_ACTION) { interp_start_event(e); return; }
        }
    }
}

/* ---------------------------------------------------------------- update */
static uint32_t s_prev_buttons;

/* Called every frame (also while a menu / the virtual computer has the input) so a
   button pressed there is not seen as a fresh press when the map resumes. */
void map_note_buttons(uint32_t held) { s_prev_buttons = held; }

void map_update(bool allow_player_input) {
    if (!s_loaded) return;
    if (GS.need_refresh) map_refresh_pages();
    GS.frame_counter++;
    character_t *p = &GS.player;
    uint32_t b = plat_poll_buttons();
    uint32_t pressed = b & ~s_prev_buttons;
    s_prev_buttons = b;

    bool p_was_moving = char_moving(p);
    bool p_moved = update_char_motion(p);
    if (p_was_moving && !char_moving(p)) start_touch_events_at(p->x, p->y, false);

    if (p->route_forced) route_step(p);
    else if (allow_player_input && !char_moving(p) && !interp_busy()) {
        int d = -1;
        if (b & BTN_UP) d = DIR_UP; else if (b & BTN_DOWN) d = DIR_DOWN;
        else if (b & BTN_LEFT) d = DIR_LEFT; else if (b & BTN_RIGHT) d = DIR_RIGHT;
        if (d >= 0) char_step(p, d);
        else if (pressed & BTN_A) check_action_button();
    }
    update_anim(p, p_moved || char_moving(p));

    for (int i = 0; i < GS.nev; i++) {
        character_t *e = &GS.ev[i];
        if (!e->active) continue;
        bool moved = update_char_motion(e);
        if (!char_moving(e)) {
            e->stop_count++;
            if (!interp_busy() || e->route_forced || !interp_blocks_events()) auto_move(e);
        }
        update_anim(e, moved || char_moving(e));
    }

    /* autorun events */
    if (!interp_busy()) {
        bool started = false;
        for (int i = 0; i < GS.nev && !started; i++)
            if (GS.ev[i].active && GS.ev[i].trigger == TRIG_AUTO) started = interp_start_event(&GS.ev[i]);
        /* autorun common events (trigger 3): "use ITEM" (sw 119), "use HELP" (sw 120),
           "FAST TRAVEL MENU" (sw 118).  Same rule as RPG Maker 2003 / EasyRPG:
           runs while its condition switch is ON and nothing else is running. */
        if (!started) interp_start_auto_common();
    }

    /* camera follows player smoothly (RPG Maker scrolls with the hero) */
    map_center_on_player();
    if (M.px_auto_x) GS.parallax_ox += M.px_sx;
    if (M.px_auto_y) GS.parallax_oy += M.px_sy;
}

/* ---------------------------------------------------------------- draw */
static uint32_t s_mod, s_add;

static void compute_tone(void) {
    int r = GS.tint[0], g = GS.tint[1], bl = GS.tint[2];
    int mr = r > 100 ? 255 : r * 255 / 100, mg = g > 100 ? 255 : g * 255 / 100, mb = bl > 100 ? 255 : bl * 255 / 100;
    int ar = r > 100 ? (r - 100) * 255 / 100 : 0, ag = g > 100 ? (g - 100) * 255 / 100 : 0, ab = bl > 100 ? (bl - 100) * 255 / 100 : 0;
    s_mod = 0xFF000000u | (uint32_t)clampi(mr, 0, 255) << 16 | (uint32_t)clampi(mg, 0, 255) << 8 | (uint32_t)clampi(mb, 0, 255);
    s_add = (uint32_t)clampi(ar, 0, 255) << 16 | (uint32_t)clampi(ag, 0, 255) << 8 | (uint32_t)clampi(ab, 0, 255);
}

static void draw_tiles(const gtex_t *cs, bool above_pass) {
    int ox = GS.display_x * TILE / REAL_TILE + GS.shake_ofs, oy = GS.display_y * TILE / REAL_TILE;
    int x0 = ox >> 4, y0 = oy >> 4;
    const chipset_def_t *c = chip();
    int frame = 0;
    if (c) {
        int spd = c->anim_speed ? 12 : 24; /* anim_speed 1 = fast */
        int step = GS.frame_counter / spd;
        frame = c->anim_type ? step % 3 : ((step % 4) == 3 ? 1 : step % 4);
    }
    int frame_c = (GS.frame_counter / 6) % 4;
    for (int ty = y0 - 1; ty <= y0 + SCREEN_H / TILE + 1; ty++) {
        if (ty < 0 || ty >= M.h) continue;
        for (int tx = x0 - 1; tx <= x0 + SCREEN_W / TILE + 1; tx++) {
            if (tx < 0 || tx >= M.w) continue;
            int cell = ty * M.w + tx;
            float dx = (float)(tx * TILE - ox), dy = (float)(ty * TILE - oy);
            /* lower */
            uint8_t lf = lower_flags(tx, ty);
            bool lower_above = (lf & (PASS_WALL | PASS_ABOVE)) != 0;
            if (lower_above == above_pass) {
                int id = M.lower[cell];
                int f = (id >= 3000 && id < 4000) ? frame_c : frame;
                const uint8_t *q = &M.lower_q[(cell * 4 + f) * 8];
                for (int k = 0; k < 4; k++) {
                    if (q[k * 2] == 0xFF) continue;
                    gfx_draw_tex(cs, q[k * 2] * 8, q[k * 2 + 1] * 8, 8, 8, dx + (k & 1) * 8, dy + (k >> 1) * 8, 8, 8, s_mod, s_add);
                }
            }
            /* upper */
            uint8_t uf = upper_flags(tx, ty);
            bool upper_above = (uf & PASS_ABOVE) != 0;
            if (upper_above == above_pass && M.upper[cell] != 10000) {
                const uint8_t *q = &M.upper_q[cell * 8];
                for (int k = 0; k < 4; k++) {
                    if (q[k * 2] == 0xFF) continue;
                    gfx_draw_tex(cs, q[k * 2] * 8, q[k * 2 + 1] * 8, 8, 8, dx + (k & 1) * 8, dy + (k >> 1) * 8, 8, 8, s_mod, s_add);
                }
            }
        }
    }
}

static void draw_char(const character_t *c, const gtex_t *cs) {
    if (!c->active || c->opacity <= 0) return;
    float sx = (float)(c->rx - GS.display_x) * TILE / REAL_TILE + GS.shake_ofs;
    float sy = (float)(c->ry - GS.display_y) * TILE / REAL_TILE - (float)char_jump_height(c);
    if (sx < -32 || sy < -48 || sx > SCREEN_W + 16 || sy > SCREEN_H + 32) return;
    uint32_t col = ((uint32_t)c->opacity << 24) | (s_mod & 0xFFFFFF);
    if (c->tile_sprite) {
        int id = c->sprite_index;
        if (id <= 0 || id >= 144 || !cs) return;
        int col_t = id < 48 ? 18 + id % 6 : 24 + (id - 48) % 6;
        int row_t = id < 48 ? 8 + id / 6 : (id - 48) / 6;
        gfx_draw_tex(cs, col_t * 16, row_t * 16, 16, 16, sx, sy, 16, 16, col, s_add);
        return;
    }
    if (!c->sprite[0]) return;
    const gtex_t *t = gfx_tex(CAT_CHARSET, c->sprite, true, LIFE_ROOM);
    if (!t->valid) return;
    int idx = c->sprite_index & 7;
    int frame = c->anim_frame == 3 ? 1 : c->anim_frame;
    int srcx = (idx % 4) * 72 + frame * 24, srcy = (idx / 4) * 128 + (c->facing & 3) * 32;
    gfx_draw_tex(t, srcx, srcy, 24, 32, sx - 4, sy - 16, 24, 32, col, s_add);
}

void map_draw(void) {
    if (!s_loaded) return;
    compute_tone();
    /* parallax */
    if (GS.parallax[0]) {
        const gtex_t *pt = gfx_tex(CAT_PANORAMA, GS.parallax, false, LIFE_ROOM);
        if (pt->valid) {
            /* RPG Maker 2003 (EasyRPG Game_Map::Parallax): a panorama with "scroll"
               on moves at half the map speed (+ auto scroll); without it, it is
               fitted to the map: offset = camera * (pano - screen) / (map - screen). */
            int dpx = GS.display_x * TILE / REAL_TILE, dpy = GS.display_y * TILE / REAL_TILE;
            int ox, oy;
            if (M.px_loop_x) ox = dpx / 2 + GS.parallax_ox / 16;
            else {
                int w = (M.w - SCREEN_W / TILE) * TILE, span = pt->w - SCREEN_W;
                ox = (w > 0 && span > 0) ? dpx * (span < w ? span : w) / w : 0;
            }
            if (M.px_loop_y) oy = dpy / 2 + GS.parallax_oy / 16;
            else {
                int h = (M.h - SCREEN_H / TILE) * TILE, span = pt->h - SCREEN_H;
                oy = (h > 0 && span > 0) ? dpy * (span < h ? span : h) / h : 0;
            }
            for (int yy = -(oy % pt->h) - (oy < 0 ? pt->h : 0); yy < SCREEN_H; yy += pt->h)
                for (int xx = -(ox % pt->w) - (ox < 0 ? pt->w : 0); xx < SCREEN_W; xx += pt->w)
                    gfx_draw_tex(pt, 0, 0, pt->w, pt->h, (float)xx, (float)yy, (float)pt->w, (float)pt->h, s_mod, s_add);
        }
    }
    const chipset_def_t *c = chip();
    const gtex_t *cs = c ? gfx_tex(CAT_CHIPSET, c->file, true, LIFE_ROOM) : NULL;
    if (cs && cs->valid) draw_tiles(cs, false);
    for (int i = 0; i < GS.nev; i++) if (GS.ev[i].layer == LAYER_BELOW) draw_char(&GS.ev[i], cs);
    /* same layer: sort by screen y (simple insertion order by y) */
    const character_t *order[MAX_EVENTS + 1];
    int n = 0;
    for (int i = 0; i < GS.nev; i++) if (GS.ev[i].active && GS.ev[i].layer == LAYER_SAME) order[n++] = &GS.ev[i];
    order[n++] = &GS.player;
    for (int a = 1; a < n; a++)
        for (int b = a; b > 0 && order[b - 1]->ry > order[b]->ry; b--) { const character_t *t = order[b]; order[b] = order[b - 1]; order[b - 1] = t; }
    for (int i = 0; i < n; i++) draw_char(order[i], cs);
    if (cs && cs->valid) draw_tiles(cs, true);
    for (int i = 0; i < GS.nev; i++) if (GS.ev[i].layer == LAYER_ABOVE) draw_char(&GS.ev[i], cs);
}
