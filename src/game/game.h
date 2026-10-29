/* SPDX-License-Identifier: GPL-3.0-or-later
 * Runtime game state (RPG Maker 2003 semantics, OneShot subset). */
#ifndef OS_GAME_H
#define OS_GAME_H
#include "../common.h"
#include "../resources/gamedata.h"

enum { DIR_UP = 0, DIR_RIGHT = 1, DIR_DOWN = 2, DIR_LEFT = 3 };
enum { TRIG_ACTION = 0, TRIG_TOUCH = 1, TRIG_COLLISION = 2, TRIG_AUTO = 3, TRIG_PARALLEL = 4 };
enum { LAYER_BELOW = 0, LAYER_SAME = 1, LAYER_ABOVE = 2 };
#define CHAR_PLAYER 10001
#define CHAR_THIS 10005
#define REAL_TILE 256

typedef struct {
    bool active;          /* page found & not erased */
    bool erased;
    int id;               /* event id (0 = player) */
    int x, y;             /* tile position */
    int rx, ry;           /* real position, REAL_TILE units */
    int dir, facing;      /* movement dir and sprite facing */
    int anim_frame;       /* 0..3 */
    int anim_count;
    int stop_count, max_stop_count;
    int speed, freq;
    int layer;
    bool through, overlap_forbidden;
    bool dir_fixed, anim_paused, step_anim;   /* anim_type derived */
    bool spinning;
    int opacity;          /* 0..255 */
    char sprite[NAME_LEN];
    int sprite_index;     /* charset index or tile id (if sprite empty) */
    bool tile_sprite;
    int move_type;
    /* move route */
    const movecmd_t *route;
    int route_len, route_index;
    bool route_repeat, route_skip, route_forced;
    movecmd_t forced[64];
    int wait_count;
    bool walking;         /* currently between tiles */
    int page;             /* active page index (-1 none) */
    bool starting;        /* triggered, waiting for interpreter */
    int trigger;
    int jump_peak;
    bool jumping;         /* RM2003 jump: covers one step (REAL_TILE) at move speed */
    int jump_rem;
} character_t;

typedef struct {
    bool shown;
    char name[NAME_LEN];
    float x, y, mag, trans_top, trans_bot;
    float tx, ty, tmag, ttop, tbot;
    int r, g, b, sat, tr, tg, tb, tsat;
    int duration;
    bool keyed, fixed_to_map;
    int effect, effect_power;
    float angle;
} picture_t;

typedef struct {
    /* RPG Maker database state */
    bool sw[MAX_SWITCHES];
    int32_t var[MAX_VARIABLES];
    uint8_t items[MAX_ITEMS + 1];
    int party[MAX_PARTY];
    int party_n;
    struct {
        char name[NAME_LEN];
        char charset[NAME_LEN];
        int char_index;
        bool transparent;
        char face[NAME_LEN];
        int face_index;
        bool skills[MAX_SKILLS + 1];
        int equip[5];
    } actor[MAX_ACTORS + 1];

    /* map */
    int map_id;
    character_t player;
    character_t ev[MAX_EVENTS];
    int nev;
    int display_x, display_y;    /* REAL_TILE units, top-left of screen */
    bool menu_enabled;
    char parallax[NAME_LEN];
    int parallax_ox, parallax_oy;

    /* screen */
    int tint[4], tint_target[4], tint_frames;
    int flash_rgb[3], flash_level, flash_frames;
    int shake_power, shake_speed, shake_frames, shake_ofs, shake_dir;
    int fade_level;              /* 0 = visible, 255 = black (EraseScreen) */
    int fade_target, fade_frames;
    picture_t pic[MAX_PICTURES + 1];

    /* system / message options */
    char system_name[NAME_LEN];
    int msg_transparent, msg_position, msg_auto_pos, msg_event_move;
    char face_name[NAME_LEN];
    int face_index, face_right, face_flip;
    sound_ref_t sys_se[4];       /* cursor, decision, cancel, buzzer (ChangeSystemSFX) */
    char bgm_name[NAME_LEN];
    int frame_counter;

    /* teleport request */
    bool teleport_pending;
    int tp_map, tp_x, tp_y, tp_dir;
    bool need_refresh;
} game_t;

extern game_t GS;

/* switches/variables (1-based; out of range ids are ignored and reported) */
bool game_sw(int id);
void game_set_sw(int id, bool v);
int32_t game_var(int id);
void game_set_var(int id, int32_t v);

void game_new(void);
const char *actor_name(int id);
int leader_actor(void);

/* map / characters (map.c) */
bool map_setup(int map_id);
bool map_setup_keep_interp(int map_id);
void map_gc_retired(void);
void map_unload(void);
void map_refresh_pages(void);
void map_update(bool allow_player_input);
void map_note_buttons(uint32_t held);   /* keep the map's press detection in sync every frame */
void map_draw(void);
character_t *map_char(int id, int self_id);
bool map_passable_from(const character_t *c, int x, int y, int dir);
int map_w(void);
int map_h(void);
void map_center_on_player(void);
void char_set_route(character_t *c, const movecmd_t *cmds, int n, bool repeat, bool skip);
bool char_moving(const character_t *c);
bool any_forced_route_running(void);
void char_turn(character_t *c, int dir);
int char_jump_height(const character_t *c);   /* pixels above the ground while jumping */
const mapdata_t *map_data(void);
bool map_tile_passable_dir(int x, int y, int dir);   /* tests: chipset/tile-event passability */

#endif
