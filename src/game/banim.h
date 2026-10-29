/* SPDX-License-Identifier: GPL-3.0-or-later
 * Battle animations shown on the map (ShowBattleAnimation) - in OneShot these are
 * the emote balloons (Question, Exclamation, Love, Sweat, Frustration).
 * Data: anim.dat (tools/convert.py).  Timing/placement follow EasyRPG Player:
 * one animation frame per 2 game frames, 96x96 cells (5 per row) centred on the
 * target (24 px tall) or on the screen. */
#ifndef OS_BANIM_H
#define OS_BANIM_H
#include "game.h"

int banim_start(int anim_id, const character_t *target);  /* game frames it lasts; 0 if unknown */
void banim_update(void);
void banim_draw(void);
void banim_stop(void);
bool banim_active(void);
int banim_count(void);    /* animations loaded (tests) */

#endif
