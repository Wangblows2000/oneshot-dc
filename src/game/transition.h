/* SPDX-License-Identifier: GPL-3.0-or-later
 *
 * Central state-transition manager.  Every kind of "restart" goes through
 * here with an explicit, documented preservation policy:
 *
 *   transition          room state  session state  VFS session  VFS persistent  save (VMU)
 *   ------------------  ----------  -------------  -----------  --------------  ----------
 *   TR_ROOM (teleport)  replaced    kept           kept         kept            untouched
 *   TR_TITLE            cleared     cleared        kept         kept            untouched
 *   TR_SESSION_RESTART  cleared     cleared        kept         kept            untouched
 *   TR_RELAUNCH         cleared     cleared*       kept         kept            committed first
 *   TR_LOAD             cleared     from save      kept         from save       read only
 *   TR_POWER_CYCLE      cleared     cleared        CLEARED      from save       read only
 *
 *   *TR_RELAUNCH simulates OneShot's window closing and the game being opened
 *    again: the game boots through Map 1 (INIT) exactly like a fresh launch,
 *    and anything the original kept on the real PC (save file, Documents
 *    folder) survives because it lives in the save / VFS layers.
 */
#ifndef OS_TRANSITION_H
#define OS_TRANSITION_H
#include "../common.h"

typedef enum { TR_NONE, TR_ROOM, TR_TITLE, TR_SESSION_RESTART, TR_RELAUNCH, TR_LOAD, TR_POWER_CYCLE } trans_kind_t;

void trans_init(void);
void trans_teleport(int map, int x, int y, int dir);
void trans_erase_screen(int type);
void trans_show_screen(int type);
void trans_request(trans_kind_t k);
bool trans_busy(void);
void trans_update(void);
void trans_draw(void);
int trans_fade_level(void);
trans_kind_t trans_last(void);
const char *trans_name(trans_kind_t k);
/* scene the main loop should show */
typedef enum { SCENE_BOOT, SCENE_TITLE, SCENE_MAP } scene_t;
scene_t trans_scene(void);
void trans_set_scene(scene_t s);
void game_boot_new_session(void); /* start at the database start map (Map 1 INIT) */

#endif
