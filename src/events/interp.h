/* SPDX-License-Identifier: GPL-3.0-or-later */
#ifndef OS_INTERP_H
#define OS_INTERP_H
#include "../game/game.h"

void interp_init(void);
void interp_update(void);                     /* run main + parallel interpreters for one frame */
bool interp_busy(void);                       /* main interpreter executing (blocks player) */
bool interp_blocks_events(void);
bool interp_start_event(character_t *ev);     /* trigger a map event page */
void interp_start_common(int ce_id);          /* run a common event on the main interpreter */
bool interp_start_auto_common(void);          /* start the first ready autorun common event */
void interp_watch_event(int map, int id);     /* debug playback: note when this event starts */
bool interp_watch_started(void);
bool interp_start_event_page(character_t *ev, int page);
void interp_test_last_page(int *ev, int *page);   /* tests: last map event page started (1-based page) */
int interp_test_ce_calls(int id, bool reset);     /* tests: times common event <id> started/called */   /* debug playback: run page index (0-based) */
void interp_event_page_changed(character_t *ev);
void interp_refresh_parallels(void);
void interp_clear_map(void);
void interp_clear_parallels(void);
void interp_on_map_changed(void);
void interp_clear_all(void);

/* Unsupported-command accounting (diagnostics + tests). */
typedef struct { uint16_t code; uint32_t count; } unsupported_t;
int interp_unsupported(unsupported_t *out, int max);
uint32_t interp_commands_executed(void);

#endif
