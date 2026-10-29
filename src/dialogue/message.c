/* SPDX-License-Identifier: GPL-3.0-or-later
 * RPG Maker 2003-style message window. */
#include "message.h"
#include "../game/game.h"
#include "../renderer/gfx.h"
#include "../audio/audio.h"
#include "../events/bridge.h"
#include "../platform/platform.h"
#include <stdio.h>
#include <stdlib.h>

static struct {
    bool active, composing;
    char lines[MSG_MAX_LINES][MSG_LINE_LEN];
    int nlines;
    char choices[4][64];
    int nchoices, cancel_type, choice_start, cursor, choice_result;
    int digits, number, num_cursor, number_result;
    bool number_mode;
    /* typewriter */
    int line, pos, wait, speed;
    bool instant, wait_key, auto_close, done_text;
    int color;
    int pen_x;
    canvas_t cv;
    bool cv_ok;
    bool face_loaded;
    image_t face;
    char face_name[NAME_LEN];
    uint32_t guard_held;   /* buttons held when the window opened: ignored until released */
    int blink;
} S;

#define PLAYER_PLACEHOLDER "_PlayerName_xxxxxxxxxxxxxxxxxxxx"

void msg_reset(void) {
    if (S.face_loaded) image_free(&S.face);
    canvas_t keep = S.cv;
    bool ok = S.cv_ok;
    memset(&S, 0, sizeof(S));
    S.cv = keep; S.cv_ok = ok;
}
bool msg_active(void) { return S.active || S.composing; }
void msg_begin(void) { msg_reset(); S.composing = true; }
int msg_line_count(void) { return S.nlines; }

int msg_current_text(char *out, int cap) {
    int o = 0;
    out[0] = 0;
    if (!S.active) return 0;
    for (int i = 0; i < S.nlines && o < cap - 2; i++) {
        o += msg_expand(S.lines[i], (int)strlen(S.lines[i]), out + o, cap - o - 1);
        out[o++] = ' '; out[o] = 0;
    }
    return o;
}

void msg_add_line(const char *s, int len) {
    if (S.nlines >= MSG_MAX_LINES) return;
    int n = len < MSG_LINE_LEN - 1 ? len : MSG_LINE_LEN - 1;
    memcpy(S.lines[S.nlines], s, (size_t)n);
    S.lines[S.nlines][n] = 0;
    S.nlines++;
}

void msg_set_choices(char labels[][64], int n, int cancel_type) {
    S.nchoices = clampi(n, 0, 4);
    for (int i = 0; i < S.nchoices; i++) os_strlcpy(S.choices[i], labels[i], 64);
    S.cancel_type = cancel_type;
    S.choice_start = S.nlines;
    S.cursor = 0;
    S.choice_result = -1;
}

void msg_set_number(int digits) {
    S.number_mode = true;
    S.digits = clampi(digits, 1, 7);
    S.number = 0; S.num_cursor = 0;
}

/* Expand \v[n], \n[n] and the OneShot player-name placeholder; keep other escapes. */
int msg_expand(const char *in, int len, char *out, int cap) {
    int o = 0;
    for (int i = 0; i < len && o < cap - 1;) {
        if (len - i >= (int)strlen(PLAYER_PLACEHOLDER) && memcmp(in + i, PLAYER_PLACEHOLDER, strlen(PLAYER_PLACEHOLDER)) == 0) {
            const char *nm = bridge_player_name();
            for (const char *p = nm; *p && o < cap - 1; p++) out[o++] = *p;
            i += (int)strlen(PLAYER_PLACEHOLDER);
            continue;
        }
        if (in[i] == '\\' && i + 1 < len && (in[i + 1] == 'v' || in[i + 1] == 'V' || in[i + 1] == 'n' || in[i + 1] == 'N') &&
            i + 2 < len && in[i + 2] == '[') {
            int j = i + 3, v = 0;
            bool vv = false;
            if (j < len && (in[j] == '\\') && j + 1 < len && (in[j + 1] == 'v' || in[j + 1] == 'V')) {
                /* nested \v[\v[n]] */
                int k = j + 3, inner = 0;
                while (k < len && in[k] >= '0' && in[k] <= '9') inner = inner * 10 + (in[k++] - '0');
                if (k < len && in[k] == ']') k++;
                v = game_var(inner); j = k; vv = true;
            }
            if (!vv) while (j < len && in[j] >= '0' && in[j] <= '9') v = v * 10 + (in[j++] - '0');
            if (j < len && in[j] == ']') {
                char tmp[NAME_LEN];
                if (in[i + 1] == 'v' || in[i + 1] == 'V') snprintf(tmp, sizeof(tmp), "%d", (int)game_var(v));
                else os_strlcpy(tmp, actor_name(v), sizeof(tmp));
                for (char *p = tmp; *p && o < cap - 1; p++) out[o++] = *p;
                i = j + 1;
                continue;
            }
        }
        out[o++] = in[i++];
    }
    out[o] = 0;
    return o;
}

static int win_y(void) { return GS.msg_position == 0 ? 0 : GS.msg_position == 1 ? 80 : 160; }

void msg_show(void) {
    S.composing = false;
    S.active = true;
    for (int i = 0; i < S.nlines; i++) {
        char tmp[MSG_LINE_LEN];
        msg_expand(S.lines[i], (int)strlen(S.lines[i]), tmp, sizeof(tmp));
        os_strlcpy(S.lines[i], tmp, MSG_LINE_LEN);
    }
    if (!S.cv_ok) S.cv_ok = canvas_init(&S.cv, 304, 64);
    canvas_clear(&S.cv);
    S.line = 0; S.pos = 0; S.wait = 0; S.speed = 1; S.color = 0; S.pen_x = 0;
    S.done_text = S.nlines == 0;
    S.guard_held = plat_poll_buttons();
    if (GS.face_name[0]) {
        if (!S.face_loaded || os_stricmp(S.face_name, GS.face_name)) {
            if (S.face_loaded) image_free(&S.face);
            S.face_loaded = image_load(CAT_FACESET, GS.face_name, &S.face);
            os_strlcpy(S.face_name, GS.face_name, sizeof(S.face_name));
        }
        if (S.face_loaded) {
            int fi = GS.face_index & 15;
            int fx = GS.face_right ? 304 - 48 : 0;
            canvas_blit_image(&S.cv, fx, 0 + 8, &S.face, (fi % 4) * 48, (fi / 4) * 48, 48, 48, true);
        }
    }
    os_diag("message: %d lines, %d choices, number=%d", S.nlines, S.nchoices, S.number_mode);
}

int msg_choice_result(void) { return S.choice_result; }
int msg_number_result(void) { return S.number_result; }

static int text_x0(void) { return (GS.face_name[0] && !GS.face_right) ? 64 : 0; }

/* Advance the typewriter by one step; returns false when waiting. */
static bool type_step(void) {
    while (S.line < S.nlines) {
        const char *ln = S.lines[S.line];
        int x0 = text_x0() + ((S.nchoices && S.line >= S.choice_start) ? 12 : 0);
        if (S.pos == 0) S.pen_x = x0;
        char c = ln[S.pos];
        if (!c) { S.line++; S.pos = 0; continue; }
        if (c == '\\' && ln[S.pos + 1]) {
            char e = ln[S.pos + 1];
            S.pos += 2;
            switch (e) {
            case 'c': case 'C': case 's': case 'S': {
                int v = 0;
                if (ln[S.pos] == '[') { S.pos++; while (ln[S.pos] >= '0' && ln[S.pos] <= '9') v = v * 10 + (ln[S.pos++] - '0'); if (ln[S.pos] == ']') S.pos++; }
                if (e == 'c' || e == 'C') S.color = clampi(v, 0, 19); else S.speed = clampi(v, 1, 20);
                continue;
            }
            case '.': S.wait = 15; return false;
            case '|': S.wait = 60; return false;
            case '!': S.wait_key = true; return false;
            case '^': S.auto_close = true; continue;
            case '>': S.instant = true; continue;
            case '<': S.instant = false; continue;
            case '_': S.pen_x += 3; continue;
            case '$': continue;
            case '\\': S.pen_x = canvas_char(&S.cv, S.pen_x, 2 + S.line * 16, '\\', S.color); return true;
            default: continue;
            }
        }
        S.pen_x = canvas_char(&S.cv, S.pen_x, 2 + S.line * 16, (uint8_t)c, S.color);
        S.pos++;
        return true;
    }
    S.done_text = true;
    return false;
}

void msg_update(uint32_t pressed, uint32_t held) {
    if (!S.active) return;
    S.blink++;
    S.guard_held &= held;            /* release clears the guard bit */
    pressed &= ~S.guard_held;
    if (!S.done_text) {
        if (S.wait_key) { if (pressed & (BTN_A | BTN_B)) S.wait_key = false; return; }
        if (S.wait > 0) { S.wait--; return; }
        bool fast = S.instant || (held & BTN_A && !(S.guard_held & BTN_A));
        int n = fast ? 1000 : 1;
        for (int i = 0; i < n && !S.done_text && !S.wait && !S.wait_key; i++) if (!type_step() && !S.done_text) break;
        return;
    }
    if (S.nchoices) {
        if (pressed & BTN_DOWN) { S.cursor = (S.cursor + 1) % S.nchoices; audio_system_se(0); }
        if (pressed & BTN_UP) { S.cursor = (S.cursor + S.nchoices - 1) % S.nchoices; audio_system_se(0); }
        if (pressed & BTN_A) { S.choice_result = S.cursor; audio_system_se(1); S.active = false; }
        else if ((pressed & BTN_B) && S.cancel_type > 0) {
            S.choice_result = S.cancel_type == 5 ? 4 : S.cancel_type - 1; audio_system_se(2); S.active = false;
        }
        return;
    }
    if (S.number_mode) {
        int p10 = 1;
        for (int i = 0; i < S.digits - 1 - S.num_cursor; i++) p10 *= 10;
        int dig = (S.number / p10) % 10;
        if (pressed & BTN_UP) { S.number += (dig == 9 ? -9 : 1) * p10; audio_system_se(0); }
        if (pressed & BTN_DOWN) { S.number += (dig == 0 ? 9 : -1) * p10; audio_system_se(0); }
        if (pressed & BTN_LEFT) S.num_cursor = (S.num_cursor + S.digits - 1) % S.digits;
        if (pressed & BTN_RIGHT) S.num_cursor = (S.num_cursor + 1) % S.digits;
        if (pressed & BTN_A) { S.number_result = S.number; audio_system_se(1); S.active = false; }
        return;
    }
    if (S.auto_close || (pressed & (BTN_A | BTN_B))) S.active = false;
}

void msg_draw(void) {
    if (!S.active) return;
    int y = win_y();
    if (!GS.msg_transparent) gfx_window(0, y, 320, 80, 255, true);
    if (S.done_text && S.nchoices) {
        int cy = y + 8 + 2 + (S.choice_start + S.cursor) * 16 - 2;
        gfx_cursor(8 + text_x0(), cy, 304 - text_x0() - 8, 16, S.blink);
    }
    canvas_draw(&S.cv, 8, y + 8, 0xFFFFFFFF);
    if (S.done_text && S.number_mode) {
        char buf[16];
        snprintf(buf, sizeof(buf), "%0*d", S.digits, S.number);
        int nx = 8 + text_x0() + 8, ny = y + 8 + 2 + S.nlines * 16;
        static canvas_t ncv;
        static bool ncv_ok;
        if (!ncv_ok) ncv_ok = canvas_init(&ncv, 64, 14);
        canvas_clear(&ncv);
        canvas_text(&ncv, 0, 0, buf, 0);
        gfx_cursor(nx - 4 + S.num_cursor * 6 - 2, ny - 2, 14, 16, S.blink);
        canvas_draw(&ncv, nx, ny, 0xFFFFFFFF);
    }
    if (S.done_text && !S.nchoices && !S.number_mode && (S.blink & 16)) plat_fill(156, y + 72, 8, 4, 0xC0FFFFFF);
}
