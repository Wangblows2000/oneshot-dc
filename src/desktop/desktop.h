/* SPDX-License-Identifier: GPL-3.0-or-later
 * The virtual computer ("outside the game").  Opened with START. */
#ifndef OS_DESKTOP_H
#define OS_DESKTOP_H
#include "../common.h"

#define DOC_CLUE_PATH "/Documents/document.txt"
#define DESKTOP_CLUE_PATH "/Desktop/clue.img"

void desktop_init(void);
void desktop_open(void);
void desktop_open_debug(int page);  /* desktop + Debug Menu window (only after unlock) */
bool desktop_debug_unlocked(void);
void desktop_close(void);
bool desktop_active(void);
void desktop_update(uint32_t pressed, uint32_t held);
void desktop_draw(void);

const char *desktop_account_name(void);
void desktop_set_account_name(const char *n);
bool desktop_set_wallpaper(const char *name);   /* "default" or "clue" */
const char *desktop_wallpaper(void);
bool desktop_place_clue_file(void);
void desktop_mark_new(const char *path);
bool desktop_has_unseen(void);

/* test/diagnostic hooks */
bool desktop_file_was_opened(const char *path);
int desktop_open_window_count(void);
const char *desktop_focused_title(void);

#endif
