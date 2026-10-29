/* SPDX-License-Identifier: GPL-3.0-or-later
 * Name-based audio front end: resolves RPG Maker sound/music names through
 * the asset manifest; SE handles are cached per room and freed on map change. */
#ifndef OS_AUDIO_H
#define OS_AUDIO_H
#include "../common.h"

void audio_init(void);
void audio_se_play(const char *name, int volume /*0..100*/, int pan /*0..100*/);
void audio_system_se(int which); /* 0 cursor, 1 decision, 2 cancel, 3 buzzer */
void audio_bgm_play(const char *name, int volume, int tempo);
void audio_bgm_fadeout(int ms);
void audio_bgm_update(void);
const char *audio_bgm_current(void);
int audio_bgm_volume(void);
void audio_purge_room(void);
/* Test hook: last SE requested (name) and count. */
const char *audio_last_se(void);
uint32_t audio_se_count(void);

#endif
