/* SPDX-License-Identifier: GPL-3.0-or-later
 * Door sweep: for every map event page in the converted data that is started by
 * touching it (trigger "touch" / "collision") or with the action button and
 * contains a Teleport (doors,
 * stairs, elevators, exits), put the player on each neighbouring tile, walk into
 * the event with the D-pad and see whether the teleport happens.  This exercises
 * the real movement, passability, touch-trigger and interpreter code.
 *
 *   door_sweep DATA_DIR [--map N] [--tsv OUT]
 *
 * Results per door: TELEPORT (worked from at least one side), STARTED (the event
 * ran but did not teleport - usually a locked door / conditional message),
 * NOT_TRIGGERED (no standable side could start it), NO_SIDE (no standable
 * neighbour).  The page's own start conditions are satisfied first (switches,
 * variable, item) - this is state injection, documented in docs/TESTING.md. */
#include "../src/common.h"
#include "../src/main.h"
#include "../src/platform/platform.h"
#include "../src/platform/host.h"
#include "../src/game/game.h"
#include "../src/game/transition.h"
#include "../src/events/interp.h"
#include "../src/dialogue/message.h"
#include "../src/ui/ui.h"
#include "../src/desktop/desktop.h"
#include <stdio.h>
#include <stdlib.h>

static const int DXS[4] = {0, 1, 0, -1}, DYS[4] = {-1, 0, 1, 0};

typedef struct { int map, ev, page, x, y, target, trigger, layer; } door_t;
static door_t doors[2048];
static int ndoors;

static int first_teleport(const cmdlist_t *l) {
    for (uint32_t i = 0; i < l->count; i++) if (l->cmds[i].code == 10810 && l->cmds[i].nparams >= 1) return l->cmds[i].params[0];
    return 0;
}

static void satisfy(const page_def_t *p) {
    if (p->cond_flags & 1) game_set_sw(p->switch_a, true);
    if (p->cond_flags & 2) game_set_sw(p->switch_b, true);
    if (p->cond_flags & 4) {
        int32_t v = p->var_value;
        if (p->compare_op == 3 || p->compare_op == 5) v++; else if (p->compare_op == 4) v--;
        game_set_var(p->var_id, v);
    }
    if ((p->cond_flags & 8) && p->item_id <= MAX_ITEMS) GS.items[p->item_id] = 1;
}

static void reset_state(void) {
    interp_clear_all(); msg_reset(); ui_modal_dismiss();
    if (desktop_active()) desktop_close();
    memset(GS.sw, 0, sizeof(GS.sw)); memset(GS.var, 0, sizeof(GS.var)); memset(GS.items, 0, sizeof(GS.items));
    GS.teleport_pending = false;
    host_set_buttons(0);
}

/* place the player and run; returns 2 teleported, 1 event started only, 0 nothing */
static int attempt(const door_t *d, const page_def_t *p, int side) {
    reset_state();
    satisfy(p);
    game_set_sw(18, false);                       /* area-intro autoruns */
    int sx = d->x - DXS[side], sy = d->y - DYS[side];
    GS.player.x = sx; GS.player.y = sy; GS.player.rx = sx * REAL_TILE; GS.player.ry = sy * REAL_TILE;
    GS.player.walking = false; GS.player.through = false; GS.player.route_forced = false;
    if (!map_setup(d->map)) return -1;
    char_turn(&GS.player, side);
    /* let arrival autoruns / parallels settle (advance any text) */
    for (int f = 0; f < 2000 && (interp_busy() || ui_modal_active() || trans_busy()); f++) {
        host_set_buttons((f % 12) < 2 ? BTN_A : 0); game_frame();
    }
    if (GS.map_id != d->map) return -2;           /* an autorun moved us away first */
    /* an arrival scene may still hold a scripted move for the player (it expected
       the player elsewhere): drop it, the test only checks the doorway */
    interp_clear_map(); msg_reset(); ui_modal_dismiss();
    GS.player.route_forced = false; GS.player.route = NULL; GS.player.route_len = 0;
    GS.player.x = sx; GS.player.y = sy; GS.player.rx = sx * REAL_TILE; GS.player.ry = sy * REAL_TILE;
    map_center_on_player();
    int ev0, pg0; interp_test_last_page(&ev0, &pg0);
    (void)ev0; (void)pg0;
    bool started = false;
    for (int f = 0; f < 420; f++) {
        uint32_t b = 0;
        uint32_t pad = side == DIR_UP ? BTN_UP : side == DIR_DOWN ? BTN_DOWN : side == DIR_LEFT ? BTN_LEFT : BTN_RIGHT;
        if (d->trigger == TRIG_ACTION) {
            /* same layer: face it and press A; other layers: walk onto it, then press A */
            if (d->layer == LAYER_SAME) b = f < 4 ? pad : (f >= 10 && f < 12) ? BTN_A : 0;
            else b = f < 24 ? pad : (f >= 40 && f < 42) ? BTN_A : 0;
            if (f >= 60 && (interp_busy() || ui_modal_active()) && (f % 12) < 2) b = BTN_A;
        } else if (f < 24) b = pad;
        else if ((interp_busy() || ui_modal_active()) && (f % 12) < 2) b = BTN_A;
        host_set_buttons(b);
        game_frame();
        int ev, pg; interp_test_last_page(&ev, &pg);
        if (ev == d->ev && GS.map_id == d->map) started = true;
        if (GS.map_id != d->map) return 2;
    }
    return started ? 1 : 0;
}

static bool standable(int x, int y) {
    if (x < 0 || y < 0 || x >= map_w() || y >= map_h()) return false;
    for (int dir = 0; dir < 4; dir++) if (map_tile_passable_dir(x, y, dir)) return true;
    return false;
}

int main(int argc, char **argv) {
    if (argc < 2) { fprintf(stderr, "usage: %s DATA [--map N] [--tsv OUT]\n", argv[0]); return 2; }
    int only = 0; const char *tsv = NULL;
    for (int i = 2; i < argc; i++) {
        if (!strcmp(argv[i], "--map") && i + 1 < argc) only = atoi(argv[++i]);
        else if (!strcmp(argv[i], "--tsv") && i + 1 < argc) tsv = argv[++i];
    }
    host_set_data_root(argv[1]);
    host_set_store("build/door_sweep_vmu", HOST_STORE_NONE, 0);
    if (!plat_init() || !game_init()) { fprintf(stderr, "init failed\n"); return 1; }
    game_boot_new_session();
    /* collect doors */
    for (int i = 0; i < G.n_mapinfo; i++) {
        const mapinfo_t *mi = &G.mapinfo[i];
        if (!mi->converted || (only && mi->id != only)) continue;
        if (!map_setup(mi->id)) continue;
        const mapdata_t *m = map_data();
        for (int e = 0; e < m->nevents; e++)
            for (int pi = 0; pi < m->events[e].npages; pi++) {
                const page_def_t *p = &m->events[e].pages[pi];
                if (p->trigger != TRIG_TOUCH && p->trigger != TRIG_COLLISION && p->trigger != TRIG_ACTION) continue;
                int tgt = first_teleport(&p->list);
                if (!tgt || ndoors >= 2048) continue;
                doors[ndoors++] = (door_t){mi->id, m->events[e].id, pi, m->events[e].x, m->events[e].y, tgt, p->trigger, p->layer};
            }
    }
    FILE *out = tsv ? fopen(tsv, "w") : NULL;
    if (out) fprintf(out, "map\tevent\tpage\tx\ty\ttarget\tlayer\tresult\tsides\n");
    int counts[5] = {0};
    for (int k = 0; k < ndoors; k++) {
        door_t *d = &doors[k];
        if (!map_setup(d->map)) continue;
        page_def_t pcopy = map_data()->events[0].pages[0];   /* copied: map_setup frees map data */
        for (int e = 0; e < map_data()->nevents; e++) if (map_data()->events[e].id == d->ev) pcopy = map_data()->events[e].pages[d->page];
        char sides[64] = ""; int best = -3;
        for (int side = 0; side < 4; side++) {
            if (!map_setup(d->map)) break;
            bool st = standable(d->x - DXS[side], d->y - DYS[side]);
            if (!st) { strcat(sides, "-"); continue; }
            int r = attempt(d, &pcopy, side);
            strcat(sides, r == 2 ? "T" : r == 1 ? "s" : r == 0 ? "x" : "a");
            if (r > best) best = r;
            if (r == 2) break;
        }
        const char *res = best == 2 ? "TELEPORT" : best == 1 ? "STARTED" : best == 0 ? "NOT_TRIGGERED" : best == -2 ? "AUTORUN_MOVED" : "NO_SIDE";
        counts[best == 2 ? 0 : best == 1 ? 1 : best == 0 ? 2 : best == -2 ? 3 : 4]++;
        printf("map %3d ev %3d p%d (%3d,%3d) -> map %3d %s layer %d  %-13s sides[U R D L approach]=%s\n",
               d->map, d->ev, d->page + 1, d->x, d->y, d->target, d->trigger == TRIG_ACTION ? "A    " : "touch", d->layer, res, sides);
        if (out) fprintf(out, "%d\t%d\t%d\t%d\t%d\t%d\t%d\t%s\t%s\n", d->map, d->ev, d->page + 1, d->x, d->y, d->target, d->layer, res, sides);
    }
    if (out) fclose(out);
    printf("doors: %d  teleport: %d  started-only: %d  not-triggered: %d  autorun-moved: %d  no-side: %d\n",
           ndoors, counts[0], counts[1], counts[2], counts[3], counts[4]);
    return counts[2] ? 1 : 0;
}
