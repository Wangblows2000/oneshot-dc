/* SPDX-License-Identifier: GPL-3.0-or-later */
#ifndef OS_MESSAGE_H
#define OS_MESSAGE_H
#include "../common.h"

#define MSG_MAX_LINES 4
#define MSG_LINE_LEN 200

void msg_reset(void);
bool msg_active(void);
void msg_begin(void);
void msg_add_line(const char *s, int len);
int msg_line_count(void);
int msg_current_text(char *out, int cap);  /* expanded text of the visible message ("" if none) */
void msg_set_choices(char labels[][64], int n, int cancel_type); /* cancel_type: RPG Maker ShowChoice param */
void msg_set_number(int digits);
void msg_show(void);
int msg_choice_result(void);   /* 0..3, or 4 for "cancel branch" */
int msg_number_result(void);
void msg_update(uint32_t pressed, uint32_t held);
void msg_draw(void);
/* Text expansion used by message + desktop: escape codes, \v[], \n[], player-name placeholder. */
int msg_expand(const char *in, int len, char *out, int cap);

#endif
