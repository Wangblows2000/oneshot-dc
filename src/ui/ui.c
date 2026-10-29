/* SPDX-License-Identifier: GPL-3.0-or-later
 * Menus, on-screen keyboard and modal dialogs (TV-safe, controller only). */
#include "ui.h"
#include "../game/game.h"
#include "../game/transition.h"
#include "../renderer/gfx.h"
#include "../audio/audio.h"
#include "../events/bridge.h"
#include "../desktop/desktop.h"
#include "../platform/platform.h"
#include <stdio.h>

/* ------------------------------------------------------------ repeat */
static int s_rep_frames;
uint32_t ui_repeat(uint32_t pressed, uint32_t held) {
    uint32_t dirs = held & (BTN_UP | BTN_DOWN | BTN_LEFT | BTN_RIGHT);
    if (!dirs) { s_rep_frames = 0; return pressed; }
    if (pressed & dirs) { s_rep_frames = 0; return pressed; }
    s_rep_frames++;
    if (s_rep_frames >= 24 && (s_rep_frames - 24) % 5 == 0) return pressed | dirs;
    return pressed;
}

/* ------------------------------------------------------------ dialog chrome (virtual computer style) */
static canvas_t s_cv;
static bool s_cv_ok;

void ui_init(void) { s_cv_ok = canvas_init(&s_cv, 288, 200); }

static void sys_window(int x, int y, int w, int h, const char *title, bool focused) {
    plat_fill(x + 3, y + 3, w, h, 0x60000000);                      /* drop shadow */
    plat_fill(x, y, w, h, 0xFFC8C8D0);                               /* body */
    plat_fill(x, y, w, 14, focused ? 0xFF3A2A6A : 0xFF6A6A78);       /* title bar */
    plat_fill(x, y, w, 1, 0xFFFFFFFF); plat_fill(x, y, 1, h, 0xFFFFFFFF);
    plat_fill(x, y + h - 1, w, 1, 0xFF404048); plat_fill(x + w - 1, y, 1, h, 0xFF404048);
    (void)title;
}

/* ------------------------------------------------------------ modal state */
typedef enum { M_NONE, M_MSGBOX, M_NOTICE, M_OSK, M_LEAVE, M_RELAUNCH, M_MENU } modal_t;
static modal_t s_modal;
static char s_title[64], s_text[512];

/* Word-wrap dialog text to the dialog width (36 columns of the 6-px font). */
#define DLG_COLS 36
static void set_text(const char *src) {
    char out[sizeof(s_text)];
    size_t o = 0; int col = 0;
    for (const char *p = src; *p && o < sizeof(out) - 2; p++) {
        if (*p == '\n') { out[o++] = '\n'; col = 0; continue; }
        if (*p == ' ' && col > 0) {          /* break before a word that would not fit */
            int wl = 0; while (p[1 + wl] && p[1 + wl] != ' ' && p[1 + wl] != '\n') wl++;
            if (col + 1 + wl > DLG_COLS) { out[o++] = '\n'; col = 0; continue; }
        }
        if (col >= DLG_COLS) { out[o++] = '\n'; col = 0; }
        out[o++] = *p; col++;
    }
    out[o] = 0;
    os_strlcpy(s_text, out, sizeof(s_text));
}
static bool s_yesno;
static int s_sel, s_result, s_anim;
static uint32_t s_guard;

bool ui_modal_active(void) { return s_modal != M_NONE && s_modal != M_MENU; }
bool ui_menu_active(void) { return s_modal == M_MENU; }
void ui_modal_dismiss(void) { s_modal = M_NONE; }

static void open_modal(modal_t m) {
    s_modal = m; s_sel = 0; s_anim = 0;
    s_guard = plat_poll_buttons(); /* a held button never confirms a fresh dialog */
}

void ui_msgbox_open(const char *title, const char *text, bool yesno) {
    os_strlcpy(s_title, title, sizeof(s_title)); set_text(text);
    s_yesno = yesno; s_result = 1;
    open_modal(M_MSGBOX);
    audio_se_play("pc_messagebox", 100, 50);
}
int ui_msgbox_result(void) { return s_result; }

void ui_notice(const char *title, const char *text) {
    os_strlcpy(s_title, title, sizeof(s_title)); set_text(text);
    s_yesno = false;
    open_modal(M_NOTICE);
}

void ui_notice_storage(bool session_only, const char *why) {
    char t[512];
    if (session_only)
        snprintf(t, sizeof(t), "Progress could NOT be saved to the\nmemory card (%s).\n\nIt is kept until the console is\nswitched off. Insert a VMU with free\nspace and save again to keep it.", why);
    else
        snprintf(t, sizeof(t), "Saving failed: %s.\nNothing was written.", why);
    ui_notice("Memory card", t);
}

void ui_notice_load_failed(const char *why) {
    char t[512];
    snprintf(t, sizeof(t), "The save could not be loaded\n(%s).\n\nA new game will start. The data on\nthe memory card has not been erased.", why);
    ui_notice("Memory card", t);
}

void ui_leave_window_start(void) { open_modal(M_LEAVE); }
void ui_show_relaunch_notice(void) {
    os_strlcpy(s_title, "OneShot", sizeof(s_title));
    os_strlcpy(s_text, "OneShot has closed.\n\nPress A to open it again.", sizeof(s_text));
    open_modal(M_RELAUNCH);
}

/* ------------------------------------------------------------ on-screen keyboard */
static const char *OSK_ROWS[] = {
    "ABCDEFGHIJ", "KLMNOPQRST", "UVWXYZ.,'-",
    "abcdefghij", "klmnopqrst", "uvwxyz!?& ",
    "0123456789",
};
#define OSK_NROWS 7
#define OSK_COLS 10
static char s_osk_buf[NAME_LEN];
static int s_osk_max, s_osk_actor, s_osk_r, s_osk_c;
static char s_osk_last[NAME_LEN];

void ui_osk_open(const char *title, const char *initial, int maxlen, int actor) {
    os_strlcpy(s_title, title, sizeof(s_title));
    os_strlcpy(s_osk_buf, initial ? initial : "", sizeof(s_osk_buf));
    if (strcmp(s_osk_buf, "TPlayerNameT") == 0) s_osk_buf[0] = 0;
    s_osk_max = clampi(maxlen, 1, NAME_LEN - 1);
    if ((int)strlen(s_osk_buf) > s_osk_max) s_osk_buf[s_osk_max] = 0;
    s_osk_actor = actor;
    s_osk_r = s_osk_c = 0;
    open_modal(M_OSK);
}
const char *ui_osk_last(void) { return s_osk_last; }

static void osk_update(uint32_t pr) {
    int rows = OSK_NROWS + 1; /* last row: [Del] [Clear] [OK] */
    if (pr & BTN_UP) s_osk_r = (s_osk_r + rows - 1) % rows;
    if (pr & BTN_DOWN) s_osk_r = (s_osk_r + 1) % rows;
    int cols = s_osk_r == OSK_NROWS ? 3 : OSK_COLS;
    if (s_osk_c >= cols) s_osk_c = cols - 1;
    if (pr & BTN_LEFT) s_osk_c = (s_osk_c + cols - 1) % cols;
    if (pr & BTN_RIGHT) s_osk_c = (s_osk_c + 1) % cols;
    if (pr & (BTN_UP | BTN_DOWN | BTN_LEFT | BTN_RIGHT)) audio_system_se(0);
    size_t n = strlen(s_osk_buf);
    bool confirm = (pr & BTN_START) != 0;
    if (pr & BTN_A) {
        if (s_osk_r < OSK_NROWS) {
            if ((int)n < s_osk_max) { s_osk_buf[n] = OSK_ROWS[s_osk_r][s_osk_c]; s_osk_buf[n + 1] = 0; audio_system_se(1); }
            else audio_system_se(3);
        } else if (s_osk_c == 0) { if (n) s_osk_buf[n - 1] = 0; audio_system_se(2); }
        else if (s_osk_c == 1) { s_osk_buf[0] = 0; audio_system_se(2); }
        else confirm = true;
    }
    if (pr & BTN_B) { if (n) s_osk_buf[n - 1] = 0; audio_system_se(2); }
    if (confirm) {
        /* validation: trim spaces; empty names are rejected */
        char *s = s_osk_buf;
        while (*s == ' ') s++;
        size_t l = strlen(s);
        while (l && s[l - 1] == ' ') s[--l] = 0;
        if (!l) { audio_system_se(3); return; }
        os_strlcpy(s_osk_last, s, sizeof(s_osk_last));
        if (s_osk_actor >= 1 && s_osk_actor <= MAX_ACTORS) os_strlcpy(GS.actor[s_osk_actor].name, s, NAME_LEN);
        else desktop_set_account_name(s);
        audio_system_se(1);
        s_modal = M_NONE;
    }
}

static void osk_draw(void) {
    int x = 16, y = 24, w = 288, h = 192;
    gfx_window(x, y, w, h, 230, true);
    canvas_clear(&s_cv);
    canvas_text(&s_cv, 8, 4, s_title, 0);
    char line[NAME_LEN + 2];
    snprintf(line, sizeof(line), "%s_", s_osk_buf);
    canvas_text(&s_cv, 8, 22, line, 0);
    for (int r = 0; r < OSK_NROWS; r++)
        for (int c = 0; c < OSK_COLS; c++) {
            char ch = OSK_ROWS[r][c];
            if (ch == ' ') canvas_text(&s_cv, 16 + c * 26 - 3, 44 + r * 16, "spc", 1);
            else canvas_char(&s_cv, 16 + c * 26, 44 + r * 16, (uint8_t)ch, 0);
        }
    const char *btn[3] = {"Del", "Clear", "OK"};
    for (int i = 0; i < 3; i++) canvas_text(&s_cv, 24 + i * 88, 44 + OSK_NROWS * 16 + 4, btn[i], i == 2 ? 3 : 0);
    canvas_text(&s_cv, 8, 176, "A:type  B:delete  START:done", 1);
    if (s_osk_r < OSK_NROWS) gfx_cursor(x + 16 + s_osk_c * 26 - 6, y + 8 + 44 + s_osk_r * 16 - 2, 18, 16, s_anim);
    else gfx_cursor(x + 16 + s_osk_c * 88, y + 8 + 44 + OSK_NROWS * 16 + 2, 50, 16, s_anim);
    canvas_draw(&s_cv, x + 8, y + 8, 0xFFFFFFFF);
}

/* ------------------------------------------------------------ main menu */
static int s_menu_level, s_menu_cmd, s_menu_sub, s_menu_list[MAX_ITEMS + 1], s_menu_n, s_equip_slot;

bool ui_menu_open(void) {
    if (!GS.menu_enabled || s_modal != M_NONE) return false;
    open_modal(M_MENU);
    s_menu_level = 0; s_menu_cmd = 0;
    audio_system_se(1);
    return true;
}

static int menu_cmd_count(void) { return G.n_menu_commands + 1; }
static const char *menu_cmd_name(int i) {
    if (i >= G.n_menu_commands) return G.term_quit[0] ? G.term_quit : "Quit";
    switch (G.menu_commands[i]) {
    case 1: return G.term_items[0] ? G.term_items : "Items";
    case 2: return G.term_skills[0] ? G.term_skills : "Skills";
    case 3: return G.term_equip[0] ? G.term_equip : "Equipment";
    default: return "-";
    }
}

static void build_list(int kind) {
    s_menu_n = 0;
    int a = leader_actor();
    if (kind == 1) { for (int i = 1; i <= G.n_items; i++) if (GS.items[i] && G.item_names[i][0]) s_menu_list[s_menu_n++] = i; }
    else if (kind == 2) { for (int i = 1; i <= G.n_skills; i++) if (a && GS.actor[a].skills[i] && G.skill_names[i][0]) s_menu_list[s_menu_n++] = i; }
    else if (kind == 4) { /* items for equip slot */
        for (int i = 1; i <= G.n_items; i++) {
            int t = G.item_type[i];
            if (GS.items[i] && t >= 1 && t <= 5 && t - 1 == s_equip_slot) s_menu_list[s_menu_n++] = i;
        }
    }
}

static void menu_update(uint32_t pr) {
    if (s_menu_level == 0) {
        int n = menu_cmd_count();
        if (pr & BTN_DOWN) { s_menu_cmd = (s_menu_cmd + 1) % n; audio_system_se(0); }
        if (pr & BTN_UP) { s_menu_cmd = (s_menu_cmd + n - 1) % n; audio_system_se(0); }
        if (pr & BTN_B) { audio_system_se(2); s_modal = M_NONE; return; }
        if (pr & BTN_A) {
            audio_system_se(1);
            if (s_menu_cmd >= G.n_menu_commands) { s_menu_level = 9; s_menu_sub = 1; return; }
            int kind = G.menu_commands[s_menu_cmd];
            if (kind == 3) { s_menu_level = 3; s_menu_sub = 0; return; }
            build_list(kind);
            s_menu_level = kind; s_menu_sub = 0;
        }
        return;
    }
    if (s_menu_level == 9) { /* quit confirm */
        if (pr & (BTN_UP | BTN_DOWN)) { s_menu_sub ^= 1; audio_system_se(0); }
        if (pr & BTN_B) { s_menu_level = 0; audio_system_se(2); }
        if (pr & BTN_A) {
            if (s_menu_sub == 0) { audio_system_se(1); s_modal = M_NONE; trans_request(TR_TITLE); }
            else { s_menu_level = 0; audio_system_se(2); }
        }
        return;
    }
    if (s_menu_level == 3) { /* equipment slots */
        if (pr & BTN_DOWN) { s_menu_sub = (s_menu_sub + 1) % 5; audio_system_se(0); }
        if (pr & BTN_UP) { s_menu_sub = (s_menu_sub + 4) % 5; audio_system_se(0); }
        if (pr & BTN_B) { s_menu_level = 0; audio_system_se(2); return; }
        if (pr & BTN_A) { s_equip_slot = s_menu_sub; build_list(4); s_menu_level = 4; s_menu_sub = 0; audio_system_se(1); }
        return;
    }
    /* item / skill / equip lists */
    int n = s_menu_n + (s_menu_level == 4 ? 1 : 0); /* equip list has "(remove)" */
    if (n == 0) { if (pr & (BTN_A | BTN_B)) { s_menu_level = s_menu_level == 4 ? 3 : 0; audio_system_se(2); } return; }
    if (pr & BTN_DOWN) { s_menu_sub = (s_menu_sub + 1) % n; audio_system_se(0); }
    if (pr & BTN_UP) { s_menu_sub = (s_menu_sub + n - 1) % n; audio_system_se(0); }
    if (pr & BTN_B) { s_menu_level = s_menu_level == 4 ? 3 : 0; s_menu_sub = 0; audio_system_se(2); return; }
    if (pr & BTN_A) {
        int a = leader_actor();
        if (s_menu_level == 4) {
            if (!a) return;
            int cur = GS.actor[a].equip[s_equip_slot];
            if (cur) { GS.items[cur]++; GS.actor[a].equip[s_equip_slot] = 0; }
            if (s_menu_sub < s_menu_n) { int it = s_menu_list[s_menu_sub]; GS.items[it]--; GS.actor[a].equip[s_equip_slot] = it; }
            GS.need_refresh = true;
            audio_system_se(1);
            s_menu_level = 3;
            return;
        }
        int id = s_menu_list[s_menu_sub];
        int sw = s_menu_level == 1 ? (G.item_type[id] == 10 ? G.item_switch[id] : 0)
                                   : (G.skill_type[id] == 3 ? G.skill_switch[id] : 0);
        if (sw) { audio_system_se(1); game_set_sw(sw, true); s_modal = M_NONE; }
        else audio_system_se(3);
    }
}

static void menu_draw(void) {
    canvas_clear(&s_cv);
    int n = menu_cmd_count();
    gfx_window(16, 16, 88, 16 + n * 16, 255, true);
    for (int i = 0; i < n; i++) canvas_text(&s_cv, 4, 2 + i * 16, menu_cmd_name(i), 0);
    if (s_menu_level == 0) gfx_cursor(20, 22 + s_menu_cmd * 16, 80, 16, s_anim);
    /* description / list panel */
    if (s_menu_level == 1 || s_menu_level == 2 || s_menu_level == 4) {
        gfx_window(112, 16, 192, 168, 255, true);
        if (s_menu_n == 0 && s_menu_level != 4) canvas_text(&s_cv, 104, 2, "(nothing)", 1);
        for (int i = 0; i < s_menu_n && i < 9; i++) {
            int id = s_menu_list[i];
            canvas_text(&s_cv, 104, 2 + i * 16, s_menu_level == 2 ? G.skill_names[id] : G.item_names[id], 0);
        }
        if (s_menu_level == 4) canvas_text(&s_cv, 104, 2 + s_menu_n * 16, "(remove)", 1);
        if (s_menu_sub < 9) gfx_cursor(116, 22 + s_menu_sub * 16, 184, 16, s_anim);
        if (s_menu_sub < s_menu_n) {
            int id = s_menu_list[s_menu_sub];
            const char *d = s_menu_level == 2 ? G.skill_desc[id] : G.item_desc[id];
            gfx_window(16, 188, 288, 36, 255, true);
            canvas_text(&s_cv, 4, 174, d, 0);
        }
    }
    if (s_menu_level == 3) {
        gfx_window(112, 16, 192, 96, 255, true);
        static const char *slots[5] = {"Weapon", "Shield", "Armor", "Helmet", "Other"};
        int a = leader_actor();
        for (int i = 0; i < 5; i++) {
            canvas_text(&s_cv, 104, 2 + i * 16, slots[i], 1);
            int it = a ? GS.actor[a].equip[i] : 0;
            canvas_text(&s_cv, 160, 2 + i * 16, it ? G.item_names[it] : "-", 0);
        }
        gfx_cursor(116, 22 + s_menu_sub * 16, 184, 16, s_anim);
    }
    if (s_menu_level == 9) {
        gfx_window(40, 80, 240, 72, 255, true);
        canvas_text(&s_cv, 32, 66, G.term_quit_msg[0] ? G.term_quit_msg : "Quit the game?", 0);
        canvas_text(&s_cv, 48, 84, G.term_yes[0] ? G.term_yes : "Yes", 0);
        canvas_text(&s_cv, 48, 100, G.term_no[0] ? G.term_no : "No", 0);
        gfx_cursor(52, 102 + s_menu_sub * 16, 160, 16, s_anim);
    }
    canvas_draw(&s_cv, 24, 22, 0xFFFFFFFF);
}

/* ------------------------------------------------------------ dispatch */
static int count_lines(const char *t) { int n = 1; for (; *t; t++) n += *t == '\n'; return n; }

void ui_update(uint32_t pressed, uint32_t held) {
    if (s_modal == M_NONE) return;
    s_anim++;
    s_guard &= held;
    uint32_t pr = ui_repeat(pressed & ~s_guard, held & ~s_guard);
    switch (s_modal) {
    case M_MSGBOX: case M_NOTICE:
        if (s_yesno && (pr & (BTN_LEFT | BTN_RIGHT))) { s_sel ^= 1; audio_system_se(0); }
        if (pr & BTN_A) { s_result = s_yesno ? (s_sel == 0 ? 1 : 0) : 1; s_modal = M_NONE; audio_system_se(1); }
        else if ((pr & BTN_B) && s_yesno) { s_result = 0; s_modal = M_NONE; audio_system_se(2); }
        break;
    case M_OSK: osk_update(pr); break;
    case M_MENU: menu_update(pr); break;
    case M_LEAVE: if (++s_anim > 120) s_modal = M_NONE; break;
    case M_RELAUNCH: if (pr & BTN_A) s_modal = M_NONE; break;
    default: break;
    }
}

void ui_draw(void) {
    switch (s_modal) {
    case M_MSGBOX: case M_NOTICE: {
        int lines = count_lines(s_text);
        int w = 232, h = 44 + lines * 13 + 20, x = (SCREEN_W - w) / 2, y = (SCREEN_H - h) / 2;
        sys_window(x, y, w, h, s_title, true);
        canvas_clear(&s_cv);
        canvas_set_plain_color(0xFFFF);
        canvas_text(&s_cv, 4, 1, s_title, 0);
        canvas_set_plain_color(0x8000 | (3 << 10) | (3 << 5) | 8);
        int ly = 22;
        const char *p = s_text;
        char line[64];
        while (*p) {
            int k = 0;
            while (*p && *p != '\n' && k < 63) line[k++] = *p++;
            line[k] = 0;
            if (*p == '\n') p++;
            canvas_text(&s_cv, 10, ly, line, 0);
            ly += 13;
        }
        const char *b0 = s_yesno ? "Yes" : "OK", *b1 = "No";
        int by = h - 22;
        /* the selected button sits on the dark cursor: draw its label in white */
        const uint16_t dark = 0x8000 | (3 << 10) | (3 << 5) | 8;
        canvas_set_plain_color(!s_yesno || s_sel == 0 ? 0xFFFF : dark);
        canvas_text(&s_cv, s_yesno ? w / 2 - 54 : w / 2 - 6, by, b0, 0);
        canvas_set_plain_color(s_yesno && s_sel == 1 ? 0xFFFF : dark);
        if (s_yesno) canvas_text(&s_cv, w / 2 + 34, by, b1, 0);
        if (s_yesno) gfx_cursor(x + (s_sel == 0 ? w / 2 - 64 : w / 2 + 24), y + by - 2, 40, 16, s_anim);
        else gfx_cursor(x + w / 2 - 20, y + by - 2, 40, 16, s_anim);
        canvas_set_plain_color(0);
        canvas_draw(&s_cv, x, y + 1, 0xFFFFFFFF);
        break;
    }
    case M_OSK: osk_draw(); break;
    case M_MENU: menu_draw(); break;
    case M_LEAVE: {
        int t = s_anim;
        plat_fill(0, 0, SCREEN_W, SCREEN_H, (uint32_t)clampi(t * 2, 0, 255) << 24);
        float sc = 1.0f - (float)t / 120.0f;
        int w = (int)(SCREEN_W * sc), h = (int)(SCREEN_H * sc);
        plat_fill((SCREEN_W - w) / 2, (SCREEN_H - h) / 2 - t, w, 2, 0xFFFFFFFF);
        break;
    }
    case M_RELAUNCH: {
        plat_fill(0, 0, SCREEN_W, SCREEN_H, 0xFF000000);
        canvas_clear(&s_cv);
        canvas_set_plain_color(0xFFFF);
        canvas_text(&s_cv, 64, 80, "OneShot has closed.", 0);
        canvas_text(&s_cv, 52, 104, "Press A to open it again.", 0);
        canvas_set_plain_color(0);
        canvas_draw(&s_cv, 16, 16, 0xFFFFFFFF);
        break;
    }
    default: break;
    }
}
