/* SPDX-License-Identifier: GPL-3.0-or-later */
#include "transition.h"
#include "game.h"
#include "../events/interp.h"
#include "../events/bridge.h"
#include "../dialogue/message.h"
#include "../vfs/vfs.h"
#include "../ui/ui.h"
#include "../platform/platform.h"
#include "../renderer/gfx.h"

#define FADE_FRAMES 30

static struct {
    trans_kind_t pending, last;
    int fade_level, fade_from, fade_to, fade_t, fade_total;
    bool tp_active;
    int tp_phase;  /* 0 fading out, 1 swap, 2 fading in */
    int tp_map, tp_x, tp_y, tp_dir;
    bool tp_faded;
    scene_t scene;
    int phase;
} T;

void trans_init(void) { memset(&T, 0, sizeof(T)); T.scene = SCENE_BOOT; }
scene_t trans_scene(void) { return T.scene; }
void trans_set_scene(scene_t s) { T.scene = s; }
int trans_fade_level(void) { return T.fade_level; }
trans_kind_t trans_last(void) { return T.last; }

const char *trans_name(trans_kind_t k) {
    switch (k) {
    case TR_NONE: return "none"; case TR_ROOM: return "room"; case TR_TITLE: return "title";
    case TR_SESSION_RESTART: return "session-restart"; case TR_RELAUNCH: return "relaunch";
    case TR_LOAD: return "load"; case TR_POWER_CYCLE: return "power-cycle";
    }
    return "?";
}

static void fade_to(int target, int frames) {
    T.fade_from = T.fade_level; T.fade_to = target; T.fade_t = 0; T.fade_total = frames;
    if (frames <= 0) T.fade_level = target;
}
static bool fading(void) { return T.fade_total > 0 && T.fade_t < T.fade_total; }

static bool instant_type(int type) { return type >= 19; }

void trans_erase_screen(int type) { fade_to(255, instant_type(type) ? 0 : FADE_FRAMES); }
void trans_show_screen(int type) { fade_to(0, instant_type(type) ? 0 : FADE_FRAMES); }

void trans_teleport(int map, int x, int y, int dir) {
    T.tp_active = true;
    T.tp_map = map; T.tp_x = x; T.tp_y = y; T.tp_dir = dir;
    GS.teleport_pending = true;
    bool sys_instant = G.transition_out == 20 || G.transition_out == 21;
    T.tp_faded = !sys_instant && T.fade_level == 0 && map != GS.map_id;
    T.tp_phase = 0;
    if (T.tp_faded) fade_to(255, FADE_FRAMES / 2);
    T.last = TR_ROOM;
    os_diag("teleport -> map %d (%d,%d) dir %d", map, x, y, dir);
}

void trans_request(trans_kind_t k) {
    T.pending = k;
    T.phase = 0;
    os_log("transition requested: %s", trans_name(k));
}

bool trans_busy(void) { return T.tp_active || fading() || T.pending != TR_NONE; }

void game_boot_new_session(void) {
    game_new();
    GS.player.x = G.start_x; GS.player.y = G.start_y;
    GS.player.rx = GS.player.x * REAL_TILE; GS.player.ry = GS.player.y * REAL_TILE;
    gfx_set_system(GS.system_name);
    interp_clear_all();
    msg_reset();
    bridge_new_session();
    if (!map_setup(G.start_map)) os_log("boot: start map %d missing", G.start_map);
    T.fade_level = 0;
    T.scene = SCENE_MAP;
}

static void do_teleport(void) {
    character_t *p = &GS.player;
    if (T.tp_map != GS.map_id) {
        if (!mapinfo_find(T.tp_map) || !map_setup_keep_interp(T.tp_map)) {
            os_log("teleport: map %d unavailable; staying on map %d", T.tp_map, GS.map_id);
            GS.teleport_pending = false;
            return;
        }
    }
    p->x = clampi(T.tp_x, 0, map_w() - 1); p->y = clampi(T.tp_y, 0, map_h() - 1);
    p->rx = p->x * REAL_TILE; p->ry = p->y * REAL_TILE;
    p->walking = false;
    if (T.tp_dir >= 1 && T.tp_dir <= 4) char_turn(p, T.tp_dir - 1);
    map_center_on_player();
    GS.teleport_pending = false;
    map_refresh_pages();
}

void trans_update(void) {
    if (fading()) {
        T.fade_t++;
        T.fade_level = T.fade_from + (T.fade_to - T.fade_from) * T.fade_t / T.fade_total;
        if (!fading()) T.fade_total = 0;
    }
    if (T.tp_active) {
        if (T.tp_phase == 0 && !fading()) { do_teleport(); T.tp_phase = 1; if (T.tp_faded) fade_to(0, FADE_FRAMES / 2); }
        else if (T.tp_phase == 1 && !fading()) T.tp_active = false;
        return;
    }
    if (T.pending == TR_NONE) return;
    if (T.phase == 0) { fade_to(255, FADE_FRAMES); T.phase = 1; return; }
    if (fading()) return;
    trans_kind_t k = T.pending;
    T.pending = TR_NONE;
    T.last = k;
    switch (k) {
    case TR_TITLE:
        map_unload();
        game_new();
        interp_clear_all();
        msg_reset();
        T.scene = SCENE_TITLE;
        fade_to(0, FADE_FRAMES);
        break;
    case TR_SESSION_RESTART:
        map_unload();
        game_boot_new_session();
        break;
    case TR_RELAUNCH:
        /* OneShot closed and is opened again: what survives is exactly what
         * the original kept outside the process (save + Documents/VFS). */
        BM.launches++;
        map_unload();
        ui_show_relaunch_notice();
        game_boot_new_session();
        break;
    case TR_POWER_CYCLE:
        map_unload();
        vfs_clear_layer(VFS_LAYER_SESSION);
        game_boot_new_session();
        break;
    default:
        break;
    }
}

void trans_draw(void) {
    if (T.fade_level > 0) plat_fill(0, 0, SCREEN_W, SCREEN_H, (uint32_t)clampi(T.fade_level, 0, 255) << 24);
}
