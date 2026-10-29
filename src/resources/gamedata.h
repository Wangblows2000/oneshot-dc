/* SPDX-License-Identifier: GPL-3.0-or-later
 * Loaders for the converted data produced by tools/convert.py (docs/FORMATS.md). */
#ifndef OS_GAMEDATA_H
#define OS_GAMEDATA_H
#include "../common.h"


typedef struct {
    uint16_t code, indent, nparams, slen;
    const int32_t *params; /* points into the owning blob (4-byte aligned) */
    const char *str;       /* NOT NUL terminated; use slen */
} cmd_t;

typedef struct {
    uint32_t count;
    cmd_t *cmds;
} cmdlist_t;

typedef struct { char name[NAME_LEN]; uint8_t vol, tempo, pan; } sound_ref_t;

typedef struct {
    char name[NAME_LEN], charset[NAME_LEN], face[NAME_LEN];
    uint8_t char_index, face_index;
} actor_def_t;

typedef struct {
    char file[NAME_LEN];
    uint8_t anim_type, anim_speed;
    uint8_t pass_lower[162], pass_upper[144];
} chipset_def_t;

typedef struct {
    char name[NAME_LEN];
    uint8_t trigger, switch_flag;
    uint16_t switch_id;
    cmdlist_t list;
} common_event_t;

typedef struct {
    uint16_t id, parent;
    char name[NAME_LEN];
    uint8_t music_type;
    char music[NAME_LEN];
    uint8_t converted;
} mapinfo_t;

typedef struct {
    uint8_t cat;
    char name[NAME_LEN];
    char path[32];
} asset_t;

enum { CAT_CHIPSET = 1, CAT_CHARSET, CAT_FACESET, CAT_PICTURE, CAT_PANORAMA, CAT_SYSTEM, CAT_SYSTEM2,
       CAT_TITLE, CAT_GAMEOVER, CAT_BATTLE, CAT_SOUND = 20, CAT_MUSIC = 21 };

typedef struct {
    uint16_t n_switches, n_vars;
    char system_name[NAME_LEN], system2_name[NAME_LEN], title_name[NAME_LEN], gameover_name[NAME_LEN];
    sound_ref_t se_cursor, se_decision, se_cancel, se_buzzer;
    char title_music[NAME_LEN];
    uint8_t transition_out, transition_in, message_stretch;
    uint16_t n_actors;
    actor_def_t actors[MAX_ACTORS + 1];
    uint16_t n_items;
    char item_names[MAX_ITEMS + 1][NAME_LEN];
    uint8_t item_type[MAX_ITEMS + 1];
    uint16_t item_switch[MAX_ITEMS + 1];
    char item_desc[MAX_ITEMS + 1][96];
    uint16_t n_skills;
    char skill_names[MAX_SKILLS + 1][NAME_LEN];
    uint8_t skill_type[MAX_SKILLS + 1];
    uint16_t skill_switch[MAX_SKILLS + 1];
    char skill_desc[MAX_SKILLS + 1][96];
    uint16_t n_chipsets;
    chipset_def_t chipsets[MAX_CHIPSETS + 1];
    uint16_t n_common;
    common_event_t common[MAX_COMMON_EVENTS + 1];
    uint16_t n_mapinfo;
    mapinfo_t mapinfo[MAX_MAPINFO];
    uint16_t start_map, start_x, start_y;
    uint16_t n_assets;
    asset_t assets[MAX_ASSETS];
    uint8_t menu_commands[8];
    uint8_t n_menu_commands;
    char term_items[32], term_skills[32], term_equip[32], term_quit[32], term_quit_msg[96], term_yes[32], term_no[32];
    uint8_t *blob; /* owns command storage */
} gamedata_t;

extern gamedata_t G;

bool gamedata_load(const char *rel);
const asset_t *asset_find(int cat, const char *name); /* case-insensitive */
const mapinfo_t *mapinfo_find(int id);

/* ---------------- maps ---------------- */
typedef struct { uint8_t code; int32_t a, b, c; char s[NAME_LEN]; } movecmd_t;

typedef struct {
    uint8_t cond_flags;
    uint16_t switch_a, switch_b, var_id;
    int32_t var_value;
    uint16_t item_id;
    uint8_t compare_op;
    uint16_t actor_id;     /* condition flag 0x10: this actor is in the party (map format 2) */
    char charset[NAME_LEN];
    uint8_t char_index, direction, pattern, translucent, move_type, move_freq, trigger, layer,
        overlap_forbidden, anim_type, move_speed;
    uint8_t route_repeat, route_skippable;
    uint16_t route_len;
    movecmd_t *route;
    cmdlist_t list;
} page_def_t;

typedef struct {
    uint16_t id;
    int16_t x, y;
    char name[NAME_LEN];
    uint16_t npages;
    page_def_t *pages;
} event_def_t;

typedef struct {
    uint16_t w, h, chipset_id;
    uint8_t scroll_type, parallax_flag;
    char parallax_name[NAME_LEN];
    uint8_t px_loop_x, px_loop_y, px_auto_x, px_auto_y;
    int16_t px_sx, px_sy;
    uint16_t *lower, *upper;    /* raw tile ids (mutable copies) */
    const uint8_t *lower_q;     /* w*h*4 frames*4 quarters*(x,y) */
    const uint8_t *upper_q;     /* w*h*4 quarters*(x,y) */
    uint16_t nevents;
    event_def_t *events;
    uint8_t *blob;
    size_t blob_len;
} mapdata_t;

bool mapdata_load(int id, mapdata_t *m);
void mapdata_free(mapdata_t *m);

#endif
