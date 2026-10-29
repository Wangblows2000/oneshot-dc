/* SPDX-License-Identifier: GPL-3.0-or-later */
#include "game.h"
#include "../renderer/gfx.h"

game_t GS;

bool game_sw(int id) {
    if (id < 1 || id >= MAX_SWITCHES) return false;
    return GS.sw[id];
}
void game_set_sw(int id, bool v) {
    if (id < 1 || id >= MAX_SWITCHES) { os_diag("switch %d out of range", id); return; }
    if (GS.sw[id] != v) os_diag("switch %d -> %d", id, v);
    GS.sw[id] = v;
    GS.need_refresh = true;
}
int32_t game_var(int id) {
    if (id < 1 || id >= MAX_VARIABLES) return 0;
    return GS.var[id];
}
void game_set_var(int id, int32_t v) {
    if (id < 1 || id >= MAX_VARIABLES) { os_diag("variable %d out of range", id); return; }
    GS.var[id] = v;
    GS.need_refresh = true;
}

const char *actor_name(int id) {
    if (id < 1 || id > MAX_ACTORS) return "";
    return GS.actor[id].name;
}
int leader_actor(void) { return GS.party_n > 0 ? GS.party[0] : 0; }

/* Fresh RPG Maker session state from the database. */
void game_new(void) {
    memset(&GS, 0, sizeof(GS));
    for (int i = 1; i <= G.n_actors && i <= MAX_ACTORS; i++) {
        os_strlcpy(GS.actor[i].name, G.actors[i].name, NAME_LEN);
        os_strlcpy(GS.actor[i].charset, G.actors[i].charset, NAME_LEN);
        GS.actor[i].char_index = G.actors[i].char_index;
        os_strlcpy(GS.actor[i].face, G.actors[i].face, NAME_LEN);
        GS.actor[i].face_index = G.actors[i].face_index;
    }
    GS.party_n = 0;
    GS.menu_enabled = true;
    GS.tint[0] = GS.tint[1] = GS.tint[2] = GS.tint[3] = 100;
    memcpy(GS.tint_target, GS.tint, sizeof(GS.tint));
    GS.msg_position = 2;
    GS.sys_se[0] = G.se_cursor; GS.sys_se[1] = G.se_decision; GS.sys_se[2] = G.se_cancel; GS.sys_se[3] = G.se_buzzer;
    os_strlcpy(GS.system_name, G.system_name, NAME_LEN);
    GS.player.id = 0;
    GS.player.speed = 4;
    GS.player.freq = 8;
    GS.player.layer = LAYER_SAME;
    GS.player.dir = GS.player.facing = DIR_DOWN;
    GS.player.anim_frame = 1;
    GS.player.opacity = 255;
    GS.player.active = true;
    GS.player.step_anim = false;
    GS.map_id = G.start_map;
}
