/* SPDX-License-Identifier: GPL-3.0-or-later
 * Hidden Debug Menu back end: data lists, room warp, music preview and sandboxed
 * cutscene playback.  Everything goes through the existing systems:
 *   rooms     -> trans_teleport (the same path as RPG Maker's Teleport command)
 *   cutscenes -> the event interpreter, started by the map's own Autorun check
 *   music     -> audio_bgm_play
 * Cutscene playback snapshots the game state and the bridge's persistent meta
 * first, runs with the bridge sandbox on (no VMU/VFS writes, no relaunch) and
 * restores everything afterwards, so watching a scene never changes a save. */
#include "debugmenu.h"
#include "desktop.h"
#include "../game/game.h"
#include "../game/transition.h"
#include "../events/interp.h"
#include "../events/bridge.h"
#include "../dialogue/message.h"
#include "../audio/audio.h"
#include "../ui/ui.h"
#include "../resources/gamedata.h"
#include "../platform/platform.h"
#include <stdio.h>
#include <stdlib.h>

#define DBG_MAX_ROOMS 128
#define DBG_MAX_CUTS 128
#define DBG_MAX_MUSIC 128
#define START_TIMEOUT_FRAMES 600   /* scene must begin within 10 s of arriving */

typedef struct { uint16_t map, x, y; uint8_t dir, source; char name[61]; } dbg_room_t;
typedef struct {
    uint16_t map, event; uint8_t page, flags;
    uint16_t sw_a, sw_b, var; int32_t val; uint8_t op; uint16_t item;
    char label[61];
} dbg_cut_t;

static struct {
    bool tried, ok;
    dbg_room_t room[DBG_MAX_ROOMS]; int nroom;
    dbg_cut_t cut[DBG_MAX_CUTS]; int ncut;
    int music[DBG_MAX_MUSIC]; int nmusic;           /* indices into G.assets */
    char status[64];
    /* music preview */
    bool previewing; int preview_idx;
    char prev_bgm[NAME_LEN]; int prev_vol;
} DB;

/* ------------------------------------------------------------- data */
static void set_status(const char *s) { os_strlcpy(DB.status, s, sizeof(DB.status)); os_diag("debug: %s", s); }
const char *dbg_status(void) { return DB.status; }

bool dbg_load(void) {
    if (DB.tried) return DB.ok;
    DB.tried = true;
    /* music: straight from the asset manifest the audio system resolves names with */
    for (int i = 0; i < G.n_assets && DB.nmusic < DBG_MAX_MUSIC; i++)
        if (G.assets[i].cat == CAT_MUSIC && G.assets[i].name[0]) DB.music[DB.nmusic++] = i;
    size_t len = 0;
    uint8_t *buf = (uint8_t *)plat_load_file("debug.dat", &len, 256 * 1024);
    if (!buf) { set_status("debug.dat missing - rooms/cutscenes unavailable"); return DB.ok = false; }
    reader_t r;
    rd_init(&r, buf, len);
    const uint8_t *m = rd_bytes(&r, 4);
    if (!m || memcmp(m, "OSDB", 4) || rd_u16(&r) != 1) { free(buf); set_status("debug.dat: bad header"); return DB.ok = false; }
    int n = rd_u16(&r);
    for (int i = 0; i < n && !r.err; i++) {
        dbg_room_t t;
        t.map = rd_u16(&r); t.x = rd_u16(&r); t.y = rd_u16(&r); t.dir = rd_u8(&r); t.source = rd_u8(&r);
        rd_str(&r, t.name, sizeof(t.name));
        if (DB.nroom < DBG_MAX_ROOMS && mapinfo_find(t.map)) DB.room[DB.nroom++] = t;   /* skip maps the runtime lacks */
    }
    n = rd_u16(&r);
    for (int i = 0; i < n && !r.err; i++) {
        dbg_cut_t c;
        c.map = rd_u16(&r); c.event = rd_u16(&r); c.page = rd_u8(&r); c.flags = rd_u8(&r);
        c.sw_a = rd_u16(&r); c.sw_b = rd_u16(&r); c.var = rd_u16(&r); c.val = rd_i32(&r); c.op = rd_u8(&r);
        c.item = rd_u16(&r); rd_str(&r, c.label, sizeof(c.label));
        if (DB.ncut < DBG_MAX_CUTS && mapinfo_find(c.map)) DB.cut[DB.ncut++] = c;
    }
    bool bad = r.err;
    free(buf);
    if (bad) { set_status("debug.dat truncated - partial lists"); }
    DB.ok = true;
    return true;
}

int dbg_count(int page) {
    dbg_load();
    return page == DBG_PAGE_ROOMS ? DB.nroom : page == DBG_PAGE_CUTSCENES ? DB.ncut : page == DBG_PAGE_MUSIC ? DB.nmusic : 0;
}

const char *dbg_label(int page, int i, char *buf, size_t cap) {
    buf[0] = 0;
    if (i < 0 || i >= dbg_count(page)) return buf;
    if (page == DBG_PAGE_ROOMS) snprintf(buf, cap, "%3d %s", DB.room[i].map, DB.room[i].name);
    else if (page == DBG_PAGE_CUTSCENES) snprintf(buf, cap, "%s", DB.cut[i].label);
    else snprintf(buf, cap, "%s", G.assets[DB.music[i]].name);
    return buf;
}

/* ------------------------------------------------------------- rooms */
bool dbg_goto_room(int i) {
    if (i < 0 || i >= DB.nroom) { set_status("invalid room"); return false; }
    const dbg_room_t *t = &DB.room[i];
    if (!mapinfo_find(t->map)) { set_status("map not available"); return false; }
    trans_teleport(t->map, t->x, t->y, t->dir);
    set_status("warping");
    return true;
}

/* ------------------------------------------------------------- music */
bool dbg_music_play(int i) {
    if (i < 0 || i >= DB.nmusic) { set_status("invalid track"); return false; }
    const asset_t *a = &G.assets[DB.music[i]];
    if (!DB.previewing) {            /* remember what the game was playing */
        os_strlcpy(DB.prev_bgm, audio_bgm_current(), sizeof(DB.prev_bgm));
        DB.prev_vol = audio_bgm_volume();
    }
    DB.previewing = true;
    DB.preview_idx = i;
    audio_bgm_play(a->name, 100, 100);
    char s[64]; snprintf(s, sizeof(s), "playing %.50s", a->name);
    set_status(s);
    return true;
}

void dbg_music_stop(void) {
    if (!DB.previewing) return;
    DB.previewing = false;
    if (DB.prev_bgm[0]) audio_bgm_play(DB.prev_bgm, DB.prev_vol, 100);
    else audio_bgm_play("(OFF)", 100, 100);
    set_status("music stopped");
}
bool dbg_music_previewing(void) { return DB.previewing; }
const char *dbg_music_current(void) { return DB.previewing ? G.assets[DB.music[DB.preview_idx]].name : ""; }

/* ------------------------------------------------------------- cutscenes */
#define SNAP_FIELDS(X) X(sw) X(var) X(items) X(party) X(party_n) X(actor) X(menu_enabled) \
    X(tint) X(tint_target) X(tint_frames) X(fade_level) X(pic) X(system_name) X(msg_transparent) \
    X(msg_position) X(msg_auto_pos) X(msg_event_move) X(face_name) X(face_index) X(face_right) X(face_flip) X(sys_se)
#define SNAP_DECL(f) __typeof__(GS.f) f;
#define SNAP_SAVE(f) memcpy(&P.snap.f, &GS.f, sizeof(GS.f));
#define SNAP_LOAD(f) memcpy(&GS.f, &P.snap.f, sizeof(GS.f));

static struct {
    bool active, started;
    int idx, frames;
    struct { SNAP_FIELDS(SNAP_DECL) } snap;
    int map, x, y, facing;
    bridge_meta_t bm;
    char bgm[NAME_LEN]; int bgm_vol;
} P;

bool dbg_playback_active(void) { return P.active; }

static void satisfy_page(const dbg_cut_t *c) {
    /* RPG Maker 2003 page conditions: 1 switch A, 2 switch B, 4 variable, 8 item */
    if (c->flags & 1) game_set_sw(c->sw_a, true);
    if (c->flags & 2) game_set_sw(c->sw_b, true);
    if (c->flags & 4) {
        int32_t v = c->val;
        if (c->op == 3) v = c->val + 1;        /* >  */
        else if (c->op == 4) v = c->val - 1;   /* <  */
        else if (c->op == 5) v = c->val + 1;   /* != */
        game_set_var(c->var, v);
    }
    if ((c->flags & 8) && c->item >= 1 && c->item <= MAX_ITEMS && !GS.items[c->item]) GS.items[c->item] = 1;
}

bool dbg_play_cutscene(int i) {
    if (i < 0 || i >= DB.ncut) { set_status("invalid cutscene"); return false; }
    const dbg_cut_t *c = &DB.cut[i];
    if (!mapinfo_find(c->map)) { set_status("map not available"); return false; }
    dbg_music_stop();
    SNAP_FIELDS(SNAP_SAVE)
    P.map = GS.map_id; P.x = GS.player.x; P.y = GS.player.y; P.facing = GS.player.facing;
    P.bm = BM;
    os_strlcpy(P.bgm, audio_bgm_current(), sizeof(P.bgm)); P.bgm_vol = audio_bgm_volume();
    P.active = true; P.started = false; P.idx = i; P.frames = 0;
    /* whatever the current room was running (e.g. a message opened just before the
       desktop) stops here; the room is reloaded from scratch when playback ends */
    interp_clear_map();
    msg_reset();
    bridge_new_session();
    bridge_set_sandbox(true);
    satisfy_page(c);
    /* arrive where the map's usual entrance is (room table), else keep coordinates */
    int x = GS.player.x, y = GS.player.y, d = 0;
    for (int k = 0; k < DB.nroom; k++) if (DB.room[k].map == c->map) { x = DB.room[k].x; y = DB.room[k].y; d = DB.room[k].dir; }
    interp_watch_event(c->map, c->event);
    if (c->map == GS.map_id) { map_setup(c->map); GS.player.x = x; GS.player.y = y;    /* reload: fresh events */
        GS.player.rx = x * REAL_TILE; GS.player.ry = y * REAL_TILE; map_center_on_player(); }
    else trans_teleport(c->map, x, y, d);
    char s[64]; snprintf(s, sizeof(s), "playing %.50s", c->label);
    set_status(s);
    return true;
}

static void playback_end(bool to_debug_menu, const char *why) {
    interp_watch_event(0, 0);
    interp_clear_map();
    msg_reset();
    ui_modal_dismiss();
    bridge_new_session();
    bridge_set_sandbox(false);
    SNAP_FIELDS(SNAP_LOAD)
    BM = P.bm;
    GS.shake_frames = 0; GS.flash_level = 0; GS.teleport_pending = false;
    GS.player.x = P.x; GS.player.y = P.y;
    GS.player.rx = P.x * REAL_TILE; GS.player.ry = P.y * REAL_TILE; GS.player.walking = false;
    if (!map_setup(P.map)) os_log("debug: could not reload map %d after playback", P.map);
    char_turn(&GS.player, P.facing);
    map_center_on_player();
    audio_bgm_play(P.bgm[0] ? P.bgm : "(OFF)", P.bgm_vol, 100);
    P.active = false;
    set_status(why);
    if (to_debug_menu) desktop_open_debug(DBG_PAGE_CUTSCENES);
    else desktop_open();
}

bool dbg_playback_abort_check(uint32_t pressed) {
    if (!P.active || !(pressed & BTN_B)) return false;
    playback_end(false, "cutscene stopped (B)");
    return true;
}

bool dbg_playback_after_interp(void) {
    if (!P.active) return false;
    P.frames++;
    if (!P.started && interp_watch_started()) P.started = true;
    if (!P.started && !GS.teleport_pending && !trans_busy() && GS.map_id == DB.cut[P.idx].map && !interp_blocks_events()) {
        /* arrived: start the chosen page directly (before the map's own Autorun check) */
        const dbg_cut_t *c = &DB.cut[P.idx];
        for (int k = 0; k < GS.nev; k++)
            if (GS.ev[k].id == c->event) { P.started = interp_start_event_page(&GS.ev[k], c->page - 1); break; }
        if (P.started) return false;
    }
    bool idle = !interp_blocks_events() && !msg_active() && !ui_modal_active() && !trans_busy();
    if (P.started && idle) { playback_end(true, "cutscene finished"); return true; }
    if (!P.started && P.frames > START_TIMEOUT_FRAMES) {
        playback_end(true, "cutscene did not start (conditions)");
        return true;
    }
    return false;
}
