/* SPDX-License-Identifier: GPL-3.0-or-later
 * Controller-operated virtual desktop backed by the sandboxed VFS.
 * Original art: procedurally drawn chrome, no third-party OS imagery. */
#include "desktop.h"
#include "../vfs/vfs.h"
#include "../events/bridge.h"
#include "../renderer/gfx.h"
#include "../audio/audio.h"
#include "../ui/ui.h"
#include "../resources/gamedata.h"
#include "../game/transition.h"
#include "../platform/platform.h"
#include "debugmenu.h"
#include "../events/interp.h"
#include "../game/game.h"
#include <stdio.h>
#include <stdlib.h>

#define MAX_WIN 4
#define MAX_LIST 24
#define TEXT_MAX VFS_MAX_FILE

typedef enum { W_FOLDER, W_TEXT, W_IMAGE, W_CARD, W_SETTINGS, W_DEBUG } wkind_t;

#define LR_HOLD_FRAMES 30   /* "hold L + R together": both triggers held for 0.5 s */

typedef struct {
    bool used;
    wkind_t kind;
    char path[VFS_MAX_PATH + 1];
    char title[40];
    int x, y, w, h;
    int sel, scroll;
    vfs_meta_t list[MAX_LIST];
    int nlist;
    char *text;
    int text_lines;
    char img_asset[48];
    canvas_t cv;
    bool cv_ok;
    int page, psel[DBG_PAGES], pscroll[DBG_PAGES];   /* W_DEBUG */
} win_t;

static struct {
    bool active;
    win_t win[MAX_WIN];
    int order[MAX_WIN], nwin;        /* order[nwin-1] = focused */
    int icon_sel;
    char account[NAME_LEN];
    char wallpaper[32];
    char new_paths[8][VFS_MAX_PATH + 1];
    int nnew;
    char opened[16][VFS_MAX_PATH + 1];
    int nopened;
    canvas_t icons_cv;
    bool icons_ok;
    int anim;
    uint32_t guard;
    int confirm_close;
    /* hidden Debug Menu: unlocked for the rest of the power-on session */
    bool dbg_unlocked;
    int seq, seq_hold;
    /* manual save from the Memory Card window: runs the game's own SAVE GAME (CE26) */
    int save_state;            /* 0 idle, 1 running, 2 waiting for the result dialog */
    int save_commits0, save_frames;
    char save_msg[64];
} D;

/* ------------------------------------------------------------- state */
void desktop_init(void) {
    memset(&D, 0, sizeof(D));
    os_strlcpy(D.account, "Player", sizeof(D.account));
    os_strlcpy(D.wallpaper, "default", sizeof(D.wallpaper));
}
bool desktop_active(void) { return D.active; }
const char *desktop_account_name(void) { return D.account; }
void desktop_set_account_name(const char *n) { if (n && n[0]) os_strlcpy(D.account, n, sizeof(D.account)); }
const char *desktop_wallpaper(void) { return BM.wallpaper[0] ? BM.wallpaper : D.wallpaper; }
int desktop_open_window_count(void) { return D.nwin; }
const char *desktop_focused_title(void) { return D.nwin ? D.win[D.order[D.nwin - 1]].title : ""; }

bool desktop_set_wallpaper(const char *name) {
    if (strcmp(name, "clue") == 0 && !asset_find(CAT_PICTURE, "~wall_clue")) {
        os_log("desktop: clue wallpaper asset missing");
        return false;
    }
    os_strlcpy(BM.wallpaper, name, sizeof(BM.wallpaper));
    os_diag("desktop: wallpaper -> %s", name);
    return true;
}

void desktop_mark_new(const char *path) {
    if (D.nnew < 8) os_strlcpy(D.new_paths[D.nnew++], path, VFS_MAX_PATH + 1);
}
bool desktop_has_unseen(void) { return D.nnew > 0; }

bool desktop_place_clue_file(void) {
    const char ref[] = "asset:~wall_clue";
    if (!vfs_file_exists("/Desktop")) vfs_create_directory("/Desktop", VFS_LAYER_PERSISTENT);
    int r = vfs_create_file(DESKTOP_CLUE_PATH, ref, sizeof(ref) - 1, VFS_T_IMAGE, VFS_LAYER_PERSISTENT);
    if (r == VFS_OK) desktop_mark_new(DESKTOP_CLUE_PATH);
    return r == VFS_OK;
}

bool desktop_file_was_opened(const char *path) {
    for (int i = 0; i < D.nopened; i++) if (os_stricmp(D.opened[i], path) == 0) return true;
    return false;
}
static void note_opened(const char *path) {
    if (desktop_file_was_opened(path)) return;
    if (D.nopened < 16) os_strlcpy(D.opened[D.nopened++], path, VFS_MAX_PATH + 1);
    for (int i = 0; i < D.nnew; i++)
        if (os_stricmp(D.new_paths[i], path) == 0) { memmove(D.new_paths[i], D.new_paths[i + 1], (size_t)(D.nnew - i - 1) * sizeof(D.new_paths[0])); D.nnew--; break; }
}

/* ------------------------------------------------------------- windows */
static win_t *focused(void) { return D.nwin ? &D.win[D.order[D.nwin - 1]] : NULL; }

static void win_free(win_t *w) {
    if (w->kind == W_DEBUG) dbg_music_stop();   /* closing the window ends a preview */
    free(w->text);
    if (w->cv_ok) canvas_free(&w->cv);
    memset(w, 0, sizeof(*w));
}

static void close_focused(void) {
    if (!D.nwin) return;
    win_free(&D.win[D.order[--D.nwin]]);
    audio_system_se(2);
}

static void refresh_folder(win_t *w) {
    w->nlist = 0;
    vfs_list_directory(w->path, w->list, MAX_LIST, &w->nlist);
    if (w->nlist > MAX_LIST) w->nlist = MAX_LIST;
    if (w->sel >= w->nlist) w->sel = w->nlist ? w->nlist - 1 : 0;
}

static win_t *open_win(wkind_t k, const char *path, const char *title, int ww, int hh) {
    int slot = -1;
    for (int i = 0; i < MAX_WIN; i++) if (!D.win[i].used) { slot = i; break; }
    if (slot < 0) { /* close the oldest */
        win_free(&D.win[D.order[0]]);
        memmove(D.order, D.order + 1, sizeof(int) * (MAX_WIN - 1));
        D.nwin--;
        for (int i = 0; i < MAX_WIN; i++) if (!D.win[i].used) { slot = i; break; }
    }
    win_t *w = &D.win[slot];
    memset(w, 0, sizeof(*w));
    w->used = true; w->kind = k;
    os_strlcpy(w->path, path ? path : "", sizeof(w->path));
    os_strlcpy(w->title, title, sizeof(w->title));
    w->w = ww; w->h = hh;
    w->x = 40 + D.nwin * 12; w->y = 28 + D.nwin * 10;
    if (w->x + ww > SCREEN_W - 8) w->x = SCREEN_W - 8 - ww;
    if (w->y + hh > SCREEN_H - 24) w->y = SCREEN_H - 24 - hh;
    w->cv_ok = canvas_init(&w->cv, ww - 4, hh - 18);
    D.order[D.nwin++] = slot;
    audio_system_se(1);
    return w;
}

static void wrap_text(win_t *w, const char *src, size_t len) {
    int cols = (w->w - 16) / 6;
    w->text = (char *)malloc(len * 2 + 2);
    if (!w->text) return;
    size_t o = 0;
    int col = 0;
    w->text_lines = 1;
    for (size_t i = 0; i < len; i++) {
        char c = src[i];
        if (c == '\r') continue;
        if (c == '\n' || col >= cols) { w->text[o++] = '\n'; w->text_lines++; col = 0; if (c == '\n') continue; }
        if ((unsigned char)c < 32) c = ' ';
        w->text[o++] = c;
        col++;
    }
    w->text[o] = 0;
}

static void open_path(const char *path) {
    vfs_meta_t m;
    if (vfs_get_file_metadata(path, &m) != VFS_OK) { ui_msgbox_open("Error", "The file could not be found.", false); return; }
    if (m.is_dir) {
        win_t *w = open_win(W_FOLDER, path, path, 200, 150);
        refresh_folder(w);
        return;
    }
    static char buf[TEXT_MAX + 1];
    size_t n = 0;
    int r = vfs_read_file(path, buf, TEXT_MAX, &n);
    if (r != VFS_OK) { ui_msgbox_open("Error", "The file could not be opened.", false); return; }
    buf[n] = 0;
    note_opened(path);
    if (m.type == VFS_T_TEXT) {
        win_t *w = open_win(W_TEXT, path, m.name, 220, 140);
        wrap_text(w, buf, n);
    } else if (m.type == VFS_T_IMAGE) {
        win_t *w = open_win(W_IMAGE, path, m.name, 200, 170);
        if (strncmp(buf, "asset:", 6) == 0) os_strlcpy(w->img_asset, buf + 6, sizeof(w->img_asset));
    } else {
        ui_msgbox_open(m.name, "This file cannot be displayed.", false);
    }
}

/* ------------------------------------------------------------- desktop icons */
typedef enum { IC_DOCUMENTS, IC_DESKTOP_FILES, IC_PICTURES, IC_CARD, IC_SETTINGS, IC_RETURN, IC_CLOSE, IC_DEBUG, IC_N } icon_t;
static const char *ICON_LABEL[IC_N] = {"Documents", "Desktop", "Pictures", "Memory Card", "Settings", "Back to game", "Close OneShot", "Debug Menu"};
/* the Debug Menu icon (last) exists only after the unlock sequence */
static int icon_count(void) { return D.dbg_unlocked ? IC_N : IC_N - 1; }
bool desktop_debug_unlocked(void) { return D.dbg_unlocked; }

static win_t *open_debug_win(int page) {
    win_t *w = open_win(W_DEBUG, NULL, "Debug Menu", 264, 196);
    w->page = page >= 0 && page < DBG_PAGES ? page : 0;
    dbg_load();
    return w;
}

static void activate_icon(int i) {
    switch (i) {
    case IC_DOCUMENTS: open_path("/Documents"); break;
    case IC_DESKTOP_FILES: open_path("/Desktop"); break;
    case IC_PICTURES: open_path("/Pictures"); break;
    case IC_CARD: open_win(W_CARD, NULL, "Memory Card", 232, 132); D.save_msg[0] = 0; break;
    case IC_SETTINGS: open_win(W_SETTINGS, NULL, "Settings", 200, 90); break;
    case IC_RETURN: desktop_close(); break;
    case IC_DEBUG: if (D.dbg_unlocked) open_debug_win(DBG_PAGE_ROOMS); break;
    case IC_CLOSE:
        if (!bridge_close_enabled()) { ui_msgbox_open("OneShot", "OneShot cannot be closed right now.", false); break; }
        D.confirm_close = 1;
        ui_msgbox_open("Close OneShot", "Close the game?\nUnsaved progress will be lost.", true);
        break;
    }
}

/* ------------------------------------------------------------- public */
void desktop_open(void) {
    D.active = true;
    D.seq = 0; D.seq_hold = 0;
    D.guard = plat_poll_buttons();
    if (!D.icons_ok) D.icons_ok = canvas_init(&D.icons_cv, 96, 212);
    audio_se_play("pc_on", 100, 50);
    os_diag("desktop opened");
}
void desktop_open_debug(int page) {
    desktop_open();
    if (D.dbg_unlocked) open_debug_win(page);
}

/* Unlock: D-pad Up, Up, Down, Down, then hold L + R together.  Only watched on the
 * icon screen (no window open), using the desktop's own button edges (no auto-
 * repeat), so it cannot collide with the Debug window's controls or gameplay. */
static void debug_sequence(uint32_t pressed, uint32_t held) {
    static const uint32_t SEQ[4] = {BTN_UP, BTN_UP, BTN_DOWN, BTN_DOWN};
    const uint32_t DIRS = BTN_UP | BTN_DOWN | BTN_LEFT | BTN_RIGHT, LR = BTN_L | BTN_R;
    if (D.dbg_unlocked) return;
    uint32_t dirs = pressed & DIRS;
    if (pressed & ~(DIRS | LR)) { D.seq = 0; D.seq_hold = 0; return; }   /* any other button */
    if (D.seq < 4) {
        if (pressed & LR) { D.seq = 0; return; }                         /* L/R too early */
        if (!dirs) return;
        if (dirs == SEQ[D.seq]) D.seq++;
        else D.seq = dirs == BTN_UP ? (D.seq == 2 ? 2 : 1) : 0;          /* restart on a new Up */
        return;
    }
    if (dirs) { D.seq = dirs == BTN_UP ? 1 : 0; D.seq_hold = 0; return; }
    if ((held & LR) == LR) {
        if (++D.seq_hold >= LR_HOLD_FRAMES) {
            D.dbg_unlocked = true;
            D.seq = 0; D.seq_hold = 0;
            D.guard |= held;              /* the still-held triggers do nothing else */
            audio_system_se(1);
            os_log("desktop: Debug Menu unlocked");
        }
    } else D.seq_hold = 0;
}

static int debug_rows(const win_t *w) { return (w->h - 18 - 36 - 28) / 12; }  /* tabs above, status+hint below */

/* ---- manual save (Memory Card window, A) ----
 * Uses the game's own save: common event 26 "SAVE GAME" (what beds run) memorizes
 * the location in vars 21-23 and streams items/notes through the save bridge, which
 * commits to the VMU (two-slot, CRC-verified).  The bridge already shows its own
 * notice when the card is missing/full; here we add the result line + dialog. */
#define CE_SAVE_GAME 26
static void manual_save_start(void) {
    if (D.save_state) return;
    if (interp_blocks_events() || trans_busy() || GS.teleport_pending || !GS.menu_enabled) {
        os_strlcpy(D.save_msg, "Cannot save during a scene.", sizeof(D.save_msg));
        audio_system_se(3);
        ui_msgbox_open("Memory Card", "You cannot save right now.\nFinish the current scene first.", false);
        return;
    }
    if (G.n_common < CE_SAVE_GAME || !G.common[CE_SAVE_GAME].list.count) {
        os_strlcpy(D.save_msg, "Save event missing in data.", sizeof(D.save_msg));
        audio_system_se(3);
        return;
    }
    D.save_commits0 = bridge_commit_count();
    D.save_frames = 0;
    D.save_state = 1;
    os_strlcpy(D.save_msg, "Saving...", sizeof(D.save_msg));
    interp_start_common(CE_SAVE_GAME);
    os_log("desktop: manual save started");
}

static void manual_save_update(void) {
    interp_update();            /* the save event has no waits: done in a frame or two */
    bridge_tick();
    D.save_frames++;
    if (bridge_commit_count() != D.save_commits0 && !interp_blocks_events()) {
        const char *why;
        persist_status_t st = bridge_last_commit(&why);
        size_t pl = persist_last_payload_size();
        int blocks = persist_vmu_blocks_per_copy(pl);
        if (st == PERSIST_OK) {
            char t[200];
            snprintf(D.save_msg, sizeof(D.save_msg), "Saved: %d blocks per copy (2 copies).", blocks);
            snprintf(t, sizeof(t), "Game saved to the memory card.\n\n%u bytes, %d blocks per copy\n(two copies are kept).", (unsigned)pl, blocks);
            ui_msgbox_open("Memory Card", t, false);
            audio_system_se(1);
        } else {
            snprintf(D.save_msg, sizeof(D.save_msg), "NOT saved: %.40s", why);
            audio_system_se(3);   /* the bridge's own notice explains what to do */
        }
        os_log("desktop: manual save %s (%u bytes, %d blocks/copy)", st == PERSIST_OK ? "ok" : why, (unsigned)pl, blocks);
        D.save_state = 0;
    } else if (D.save_frames > 120) {
        interp_clear_map();
        os_strlcpy(D.save_msg, "NOT saved: the save did not finish.", sizeof(D.save_msg));
        ui_msgbox_open("Memory Card", "The save did not finish.\nNothing new was written.", false);
        D.save_state = 0;
    }
}

static void debug_update(win_t *w, uint32_t pr) {
    int n = dbg_count(w->page);
    int *sel = &w->psel[w->page], *scr = &w->pscroll[w->page];
    int vis = debug_rows(w);
    if (n) {
        if (pr & BTN_DOWN) { *sel = (*sel + 1) % n; audio_system_se(0); }
        if (pr & BTN_UP) { *sel = (*sel + n - 1) % n; audio_system_se(0); }
        if (pr & BTN_RIGHT) { *sel = *sel + vis < n ? *sel + vis : n - 1; audio_system_se(0); }
        if (pr & BTN_LEFT) { *sel = *sel - vis > 0 ? *sel - vis : 0; audio_system_se(0); }
        if (*sel < *scr) *scr = *sel;
        if (*sel >= *scr + vis) *scr = *sel - vis + 1;
    }
    if (!(pr & BTN_A)) return;
    if (!n) { audio_system_se(3); return; }
    switch (w->page) {
    case DBG_PAGE_ROOMS:
        if (dbg_goto_room(*sel)) { audio_system_se(1); desktop_close(); } else audio_system_se(3);
        break;
    case DBG_PAGE_CUTSCENES:
        if (dbg_play_cutscene(*sel)) { audio_system_se(1); desktop_close(); } else audio_system_se(3);
        break;
    case DBG_PAGE_MUSIC:
        if (!dbg_music_play(*sel)) audio_system_se(3);
        break;
    }
}

void desktop_close(void) {
    while (D.nwin) win_free(&D.win[D.order[--D.nwin]]);
    D.active = false;
    audio_se_play("pc_off", 100, 50);
}

void desktop_update(uint32_t pressed, uint32_t held) {
    if (!D.active) return;
    D.anim++;
    if (D.save_state == 1) { manual_save_update(); return; }
    if (D.confirm_close && !ui_modal_active()) {
        D.confirm_close = 0;
        if (ui_msgbox_result() == 1) { desktop_close(); trans_request(TR_RELAUNCH); return; }
    }
    D.guard &= held;
    uint32_t pr = ui_repeat(pressed & ~D.guard, held & ~D.guard);
    if (pr & BTN_START) { desktop_close(); return; }
    win_t *w = focused();
    if (!w) debug_sequence(pressed & ~D.guard, held & ~D.guard);
    else { D.seq = 0; D.seq_hold = 0; }
    pr &= ~D.guard;                 /* the unlock may have just guarded L/R */
    if (w && w->kind == W_DEBUG && (pr & BTN_R)) {   /* R: Rooms -> Cutscenes -> Music */
        w->page = (w->page + 1) % DBG_PAGES;
        audio_system_se(0);
        return;
    }
    if (pr & (BTN_L | BTN_R) && D.nwin > 1) { /* cycle focus */
        if (pr & BTN_R) { int f = D.order[0]; memmove(D.order, D.order + 1, sizeof(int) * (size_t)(D.nwin - 1)); D.order[D.nwin - 1] = f; }
        else { int f = D.order[D.nwin - 1]; memmove(D.order + 1, D.order, sizeof(int) * (size_t)(D.nwin - 1)); D.order[0] = f; }
        audio_system_se(0);
        return;
    }
    if (w && (held & BTN_Y)) { /* move window */
        if (held & BTN_LEFT) w->x -= 2;
        if (held & BTN_RIGHT) w->x += 2;
        if (held & BTN_UP) w->y -= 2;
        if (held & BTN_DOWN) w->y += 2;
        w->x = clampi(w->x, 0, SCREEN_W - w->w); w->y = clampi(w->y, 0, SCREEN_H - 20 - w->h);
        return;
    }
    if (!w) { /* icon navigation */
        int nic = icon_count();
        if (D.icon_sel >= nic) D.icon_sel = nic - 1;
        if (pr & BTN_DOWN) { D.icon_sel = (D.icon_sel + 1) % nic; audio_system_se(0); }
        if (pr & BTN_UP) { D.icon_sel = (D.icon_sel + nic - 1) % nic; audio_system_se(0); }
        if (pr & BTN_A) activate_icon(D.icon_sel);
        if (pr & BTN_B) desktop_close();
        return;
    }
    if (pr & BTN_B) {
        /* Debug window: B first stops a music preview, a second B closes the window */
        if (w->kind == W_DEBUG && dbg_music_previewing()) { dbg_music_stop(); audio_system_se(2); return; }
        close_focused();
        return;
    }
    switch (w->kind) {
    case W_FOLDER:
        refresh_folder(w);
        if (pr & BTN_DOWN && w->nlist) { w->sel = (w->sel + 1) % w->nlist; audio_system_se(0); }
        if (pr & BTN_UP && w->nlist) { w->sel = (w->sel + w->nlist - 1) % w->nlist; audio_system_se(0); }
        if (pr & BTN_A && w->nlist) {
            char p[VFS_MAX_PATH + 1];
            snprintf(p, sizeof(p), "%s/%s", strcmp(w->path, "/") ? w->path : "", w->list[w->sel].name);
            open_path(p);
        }
        break;
    case W_TEXT: {
        int vis = (w->h - 22) / 12;
        if (pr & BTN_DOWN && w->scroll + vis < w->text_lines) w->scroll++;
        if (pr & BTN_UP && w->scroll > 0) w->scroll--;
        break;
    }
    case W_DEBUG:
        debug_update(w, pr);
        break;
    case W_CARD:
        if (pr & BTN_A) manual_save_start();
        break;
    case W_SETTINGS:
        if (pr & BTN_A) ui_osk_open("Computer account name", D.account, 16, 0);
        if (pr & BTN_X) { g_diag_mode = !g_diag_mode; audio_system_se(1); }
        break;
    default: break;
    }
}

/* ------------------------------------------------------------- drawing */
static void draw_wallpaper(void) {
    const char *wp = desktop_wallpaper();
    if (strcmp(wp, "clue") == 0) {
        const gtex_t *t = gfx_tex(CAT_PICTURE, "~wall_clue", false, LIFE_PERSISTENT);
        if (t->valid) { gfx_draw_tex(t, 0, 0, t->w, t->h, 0, 0, SCREEN_W, SCREEN_H, 0xFFFFFFFF, 0); return; }
    }
    /* default: original procedural gradient with a faint grid */
    for (int y = 0; y < SCREEN_H; y += 8) {
        int k = y * 255 / SCREEN_H;
        uint32_t c = 0xFF000000u | (uint32_t)(20 + k / 6) << 16 | (uint32_t)(18 + k / 10) << 8 | (uint32_t)(48 + k / 4);
        plat_fill(0, y, SCREEN_W, 8, c);
    }
    for (int x = 0; x < SCREEN_W; x += 32) plat_fill(x, 0, 1, SCREEN_H, 0x18FFFFFF);
    for (int y = 0; y < SCREEN_H; y += 32) plat_fill(0, y, SCREEN_W, 1, 0x18FFFFFF);
}

static void draw_icon_glyph(int x, int y, int i) {
    uint32_t col = i == IC_CLOSE ? 0xFFC04050 : i == IC_RETURN ? 0xFF50B070 : i == IC_CARD ? 0xFF8080E0 :
                   i == IC_DEBUG ? 0xFFB0B0B8 : 0xFFE0C060;
    plat_fill(x, y + 2, 16, 12, col);
    plat_fill(x, y, 7, 3, col);
    plat_fill(x + 1, y + 3, 14, 1, 0x60FFFFFF);
}

static void draw_window(win_t *w, bool foc) {
    plat_fill(w->x + 3, w->y + 3, w->w, w->h, 0x60000000);
    plat_fill(w->x, w->y, w->w, w->h, 0xFFE4E4EA);
    plat_fill(w->x, w->y, w->w, 14, foc ? 0xFF3A2A6A : 0xFF707080);
    plat_fill(w->x, w->y, w->w, 1, 0xFFFFFFFF);
    plat_fill(w->x + w->w - 1, w->y, 1, w->h, 0xFF404048);
    plat_fill(w->x, w->y + w->h - 1, w->w, 1, 0xFF404048);
    if (!w->cv_ok) return;
    canvas_t *c = &w->cv;
    canvas_clear(c);
    canvas_set_plain_color(0xFFFF);
    canvas_text(c, 2, 0, w->title, 0);
    canvas_set_plain_color(0x8000 | (3 << 10) | (3 << 5) | 8);
    int y0 = 18;
    switch (w->kind) {
    case W_FOLDER:
        if (!w->nlist) canvas_text(c, 6, y0, "(empty)", 1);
        for (int i = 0; i < w->nlist && i < 10; i++) {
            char line[48];
            bool is_new = false;
            char full[VFS_MAX_PATH + 1];
            snprintf(full, sizeof(full), "%s/%s", strcmp(w->path, "/") ? w->path : "", w->list[i].name);
            for (int k = 0; k < D.nnew; k++) if (os_stricmp(D.new_paths[k], full) == 0) is_new = true;
            snprintf(line, sizeof(line), "%s %s%s", w->list[i].is_dir ? "[+]" : "   ", w->list[i].name, is_new ? " *" : "");
            canvas_text(c, 6, y0 + i * 12, line, i == w->sel ? 2 : 0);
        }
        if (foc && w->nlist) plat_fill(w->x + 4, w->y + 1 + y0 + w->sel * 12, w->w - 8, 12, 0x403A2A6A);
        break;
    case W_TEXT: {
        int line = 0, yy = y0;
        int vis = (w->h - 22) / 12;
        const char *p = w->text ? w->text : "";
        char buf[64];
        while (*p && line < w->scroll + vis) {
            int k = 0;
            while (*p && *p != '\n' && k < 63) buf[k++] = *p++;
            buf[k] = 0;
            if (*p == '\n') p++;
            if (line >= w->scroll) { canvas_text(c, 6, yy, buf, 0); yy += 12; }
            line++;
        }
        break;
    }
    case W_IMAGE:
        if (!w->img_asset[0]) canvas_text(c, 6, y0, "(no preview)", 1);
        break;
    case W_CARD: {
        char l[64];
        int fb = 0;
        store_result_t r = plat_store_probe(&fb);
        snprintf(l, sizeof(l), "Device: %s", r == STORE_OK ? plat_store_describe() : "none");
        canvas_text(c, 6, y0, l, 0);
        if (r == STORE_OK) { snprintf(l, sizeof(l), "Free: %d blocks", fb / 512); canvas_text(c, 6, y0 + 12, l, 0); }
        canvas_text(c, 6, y0 + 30, persist_storage_available() ? "Progress is kept on the card." : "Progress is kept only until", persist_storage_available() ? 0 : 2);
        if (!persist_storage_available()) canvas_text(c, 6, y0 + 42, "the console is switched off.", 2);
        if (persist_last_error()[0] && !D.save_msg[0]) { snprintf(l, sizeof(l), "Last error: %.40s", persist_last_error()); canvas_text(c, 6, y0 + 60, l, 1); }
        if (D.save_msg[0]) canvas_text(c, 6, y0 + 60, D.save_msg, 1);
        plat_fill(w->x + 4, w->y + 1 + y0 + 82, w->w - 8, 14, foc ? 0x403A2A6A : 0x20707080);
        canvas_text(c, 8, y0 + 83, "A: Save now (manual save)", 2);
        break;
    }
    case W_DEBUG: {
        static const char *TAB[DBG_PAGES] = {"Rooms", "Cutscenes", "Music"};
        int tx = 6;
        for (int p = 0; p < DBG_PAGES; p++) {
            int tw = (int)strlen(TAB[p]) * 6 + 8;
            if (p == w->page) plat_fill(w->x + 2 + tx - 4, w->y + 1 + y0 - 1, tw, 13, foc ? 0xFF3A2A6A : 0xFF707080);
            if (p == w->page) canvas_set_plain_color(0xFFFF);
            canvas_text(c, tx, y0, TAB[p], 0);
            canvas_set_plain_color(0x8000 | (3 << 10) | (3 << 5) | 8);
            tx += tw + 6;
        }
        int n = dbg_count(w->page), sel = w->psel[w->page], scr = w->pscroll[w->page];
        int ly = y0 + 18, vis = debug_rows(w);
        char line[64], pos[24];
        snprintf(pos, sizeof(pos), "%d/%d", n ? sel + 1 : 0, n);
        canvas_text(c, w->w - 10 - (int)strlen(pos) * 6, y0, pos, 1);
        if (!n) canvas_text(c, 6, ly, "(nothing found in the game data)", 1);
        for (int i = 0; i < vis && scr + i < n; i++) {
            dbg_label(w->page, scr + i, line, sizeof(line));
            line[40] = 0;
            bool playing = w->page == DBG_PAGE_MUSIC && dbg_music_previewing() && !strcmp(dbg_music_current(), line);
            canvas_text(c, 6, ly + i * 12, line, scr + i == sel ? 2 : 0);
            if (playing) canvas_text(c, w->w - 22, ly + i * 12, ">", 2);
        }
        if (foc && n) plat_fill(w->x + 4, w->y + 1 + ly + (sel - scr) * 12, w->w - 8, 12, 0x403A2A6A);
        if (scr > 0) canvas_text(c, w->w - 16, ly, "^", 1);
        if (scr + vis < n) canvas_text(c, w->w - 16, ly + (vis - 1) * 12, "v", 1);
        const char *hint = w->page == DBG_PAGE_MUSIC ? (dbg_music_previewing() ? "A:play  B:stop music  R:next page" : "A:play  B:close  R:next page")
                         : w->page == DBG_PAGE_CUTSCENES ? "A:play (B quits)  R:next page" : "A:warp  B:close  R:next page";
        canvas_text(c, 6, w->h - 18 - 26, dbg_status()[0] ? dbg_status() : "", 1);
        canvas_text(c, 6, w->h - 18 - 14, hint, 0);
        break;
    }
    case W_SETTINGS: {
        char l[64];
        snprintf(l, sizeof(l), "Account: %s", D.account);
        canvas_text(c, 6, y0, l, 0);
        canvas_text(c, 6, y0 + 16, "A: rename   X: diagnostics", 1);
        canvas_text(c, 6, y0 + 30, g_diag_mode ? "Diagnostics: ON" : "Diagnostics: off", 0);
        break;
    }
    }
    canvas_set_plain_color(0);
    canvas_draw(c, w->x + 2, w->y + 1, 0xFFFFFFFF);
    if (w->kind == W_IMAGE && w->img_asset[0]) {
        const gtex_t *t = gfx_tex(CAT_PICTURE, w->img_asset, false, LIFE_PERSISTENT);
        if (t->valid) gfx_draw_tex(t, 0, 0, t->w, t->h, (float)w->x + 4, (float)w->y + 16, (float)w->w - 8, (float)w->h - 20, 0xFFFFFFFF, 0);
    }
}

void desktop_draw(void) {
    if (!D.active) return;
    draw_wallpaper();
    /* icons column (TV safe area: 16px margin) */
    canvas_clear(&D.icons_cv);
    canvas_set_plain_color(0xFFFF);
    /* 7 icons: original layout.  8 (Debug Menu unlocked): slightly tighter so the
       last label still ends above the task bar. */
    int nic = icon_count();
    bool tight = nic > IC_N - 1;
    int step = tight ? 26 : 28, top = tight ? 14 : 20, lab = tight ? 14 : 16;
    for (int i = 0; i < nic; i++) {
        int y = top + i * step;
        draw_icon_glyph(24, y, i);
        bool badge = (i == IC_DOCUMENTS && D.nnew && strncmp(D.new_paths[0], "/Documents", 10) == 0) ||
                     (i == IC_DESKTOP_FILES && D.nnew && strncmp(D.new_paths[0], "/Desktop", 8) == 0);
        if (badge && (D.anim & 32)) plat_fill(38, y - 2, 6, 6, 0xFFFF4040);
        canvas_text(&D.icons_cv, 0, y - 20 + lab, ICON_LABEL[i], 0);
        if (!focused() && i == D.icon_sel) plat_fill(18, y - 3, 92, step - 2, 0x50FFFFFF);
    }
    canvas_draw(&D.icons_cv, 18, 20, 0xFFFFFFFF);
    canvas_set_plain_color(0xFFFF);
    for (int i = 0; i < D.nwin; i++) draw_window(&D.win[D.order[i]], i == D.nwin - 1);
    /* task bar */
    plat_fill(0, SCREEN_H - 18, SCREEN_W, 18, 0xFF282038);
    static canvas_t tb;
    static bool tb_ok;
    if (!tb_ok) tb_ok = canvas_init(&tb, 300, 14);
    canvas_clear(&tb);
    canvas_text(&tb, 0, 1, D.nwin ? "A:open B:close L/R:switch Y+pad:move" : "A:open  B/START:back to game", 0);
    canvas_set_plain_color(0);
    canvas_draw(&tb, 16, SCREEN_H - 16, 0xFFFFFFFF);
}
