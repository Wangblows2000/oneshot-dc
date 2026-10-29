/* SPDX-License-Identifier: GPL-3.0-or-later */
#ifndef OS_UI_H
#define OS_UI_H
#include "../common.h"

void ui_init(void);
bool ui_modal_active(void);
void ui_modal_dismiss(void);   /* close any dialog/menu without a result (debug playback abort) */
void ui_update(uint32_t pressed, uint32_t held);
void ui_draw(void);

/* on-screen keyboard (EnterHeroName, desktop settings) */
void ui_osk_open(const char *title, const char *initial, int maxlen, int actor /*0 = desktop account*/);
const char *ui_osk_last(void);

/* modal system dialogs */
void ui_msgbox_open(const char *title, const char *text, bool yesno);
int ui_msgbox_result(void);
void ui_notice(const char *title, const char *text);
void ui_notice_storage(bool session_only, const char *why);
void ui_notice_load_failed(const char *why);
void ui_leave_window_start(void);
void ui_show_relaunch_notice(void);

/* RPG Maker main menu */
bool ui_menu_open(void);
bool ui_menu_active(void);

/* directional auto-repeat shared by menus/desktop/keyboard */
uint32_t ui_repeat(uint32_t pressed, uint32_t held);

#endif
