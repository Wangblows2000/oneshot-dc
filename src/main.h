/* SPDX-License-Identifier: GPL-3.0-or-later */
#ifndef OS_MAIN_H
#define OS_MAIN_H
#include "common.h"
bool game_init(void);
void game_frame(void);
uint64_t game_frame_count(void);
#endif
