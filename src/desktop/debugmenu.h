/* SPDX-License-Identifier: GPL-3.0-or-later
 * Hidden Debug Menu back end (the window itself is a desktop window, desktop.c).
 * Lists come from data/debug.dat (tools/mkdebug.py, discovered from the original
 * data) and from the music assets in the runtime's own manifest. */
#ifndef OS_DEBUGMENU_H
#define OS_DEBUGMENU_H
#include "../common.h"

enum { DBG_PAGE_ROOMS, DBG_PAGE_CUTSCENES, DBG_PAGE_MUSIC, DBG_PAGES };

bool dbg_load(void);                     /* lazy; false if debug.dat is missing/invalid */
int dbg_count(int page);
const char *dbg_label(int page, int i, char *buf, size_t cap);

/* Actions.  Return false (and set dbg_status()) when the entry cannot be used. */
bool dbg_goto_room(int i);               /* existing teleport; caller closes the desktop */
bool dbg_play_cutscene(int i);           /* caller closes the desktop */
bool dbg_music_play(int i);
void dbg_music_stop(void);               /* restores the music that was playing before */
bool dbg_music_previewing(void);
const char *dbg_music_current(void);
const char *dbg_status(void);

/* Cutscene playback controller (main.c). */
bool dbg_playback_active(void);
bool dbg_playback_abort_check(uint32_t pressed);   /* B: abort -> desktop.  true if aborted */
bool dbg_playback_after_interp(void);              /* finished -> debug menu.  true if it ended */

#endif
