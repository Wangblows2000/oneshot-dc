/* SPDX-License-Identifier: GPL-3.0-or-later
 * Headless runner: plays a controller script against the real converted data.
 *
 *   host_runner DATA_DIR SCRIPT [--frames N] [--shots DIR] [--store DIR] [--no-vmu] [--diag]
 *
 * Script lines:  <frame> <BUTTONS|->      buttons held from that frame on
 *                <frame> shot <name>      write DIR/<name>.ppm at that frame
 *                <frame> expect-var <id> <value>
 *                <frame> expect-sw <id> <0|1>
 *                <frame> expect-map <id>
 *                <frame> expect-file <vfs path> <0|1>
 *                <frame> warp <map> <x> <y> <dir 1-4>   (debug teleport)
 *                <frame> setsw <id> <0|1> / setvar <id> <value> / item <id> <count>
 *                <frame> type-number <digits|@vfs-path>  queue D-pad presses for a number prompt;
 *                        @path reads the digits from a virtual file the way a player would
 *                <frame> bridge <fn> <arg1>             invoke a bridge function as an event would
 *                <frame> common <id>                    run a common event (e.g. 26 = SAVE GAME)
 *                <frame> expect-opened <vfs path>       file was opened in the virtual desktop
 *                <frame> expect-ending <n> / expect-wallpaper <name> / expect-trans <name>
 *                <frame> expect-page <event> <page>   the last map event page that started
 *                <frame> expect-ce <id> <0|1>       common event <id> ran since the last check (reset-ce <id> clears)
 *                <frame> expect-msg <text>        a message containing <text> ('_' = space) was shown
 *                        since the previous expect-msg (every message is also logged as [msg])
 *                <frame> advance <max_frames>   press A whenever text/dialogs wait; the script
 *                        clock pauses until the game has been idle for 40 frames
 * Script frame numbers count only frames where no "advance" is running.
 * BUTTONS: comma list of UP DOWN LEFT RIGHT A B X Y START L R. */
#include "../src/common.h"
#include "../src/main.h"
#include "../src/platform/platform.h"
#include "../src/platform/host.h"
#include "../src/game/game.h"
#include "../src/vfs/vfs.h"
#include "../src/events/interp.h"
#include "../src/events/bridge.h"
#include "../src/game/transition.h"
#include "../src/desktop/desktop.h"
#include "../src/ui/ui.h"
#include "../src/dialogue/message.h"
#include "../src/desktop/debugmenu.h"
#include "../src/audio/audio.h"
#include "../src/game/banim.h"
#include "../src/renderer/gfx.h"
#include <stdio.h>
#include <stdlib.h>

typedef struct { int frame; char op[32]; char a[128]; char b[64]; char c[64]; } line_t;

static uint32_t parse_buttons(const char *s) {
    if (!strcmp(s, "-")) return 0;
    uint32_t m = 0;
    char tmp[128];
    os_strlcpy(tmp, s, sizeof(tmp));
    for (char *t = strtok(tmp, ","); t; t = strtok(NULL, ",")) {
        if (!strcmp(t, "UP")) m |= BTN_UP; else if (!strcmp(t, "DOWN")) m |= BTN_DOWN;
        else if (!strcmp(t, "LEFT")) m |= BTN_LEFT; else if (!strcmp(t, "RIGHT")) m |= BTN_RIGHT;
        else if (!strcmp(t, "A")) m |= BTN_A; else if (!strcmp(t, "B")) m |= BTN_B;
        else if (!strcmp(t, "X")) m |= BTN_X; else if (!strcmp(t, "Y")) m |= BTN_Y;
        else if (!strcmp(t, "START")) m |= BTN_START; else if (!strcmp(t, "L")) m |= BTN_L; else if (!strcmp(t, "R")) m |= BTN_R;
        else fprintf(stderr, "unknown button %s\n", t);
    }
    return m;
}

/* queued button presses (press/release pairs) */
static uint32_t Q[4096];
static int qn, qi, qhold;
static void qpress(uint32_t b) { if (qn < 4094) { Q[qn++] = b; Q[qn++] = 0; } }
static char last_msg[1024], seen[16384], remembered_bgm[64];

int main(int argc, char **argv) {
    if (argc < 3) { fprintf(stderr, "usage: %s DATA SCRIPT [--frames N] [--shots DIR] [--store DIR] [--no-vmu] [--diag]\n", argv[0]); return 2; }
    int frames = 3600;
    const char *shots = ".", *store = "build/host_vmu";
    host_store_mode_t mode = HOST_STORE_OK;
    for (int i = 3; i < argc; i++) {
        if (!strcmp(argv[i], "--frames") && i + 1 < argc) frames = atoi(argv[++i]);
        else if (!strcmp(argv[i], "--shots") && i + 1 < argc) shots = argv[++i];
        else if (!strcmp(argv[i], "--store") && i + 1 < argc) store = argv[++i];
        else if (!strcmp(argv[i], "--no-vmu")) mode = HOST_STORE_NONE;
        else if (!strcmp(argv[i], "--diag")) g_diag_mode = true;
    }
    host_set_data_root(argv[1]);
    host_set_store(store, mode, 128 * 1024);
    static line_t L[4096];
    int n = 0;
    FILE *f = fopen(argv[2], "r");
    if (!f) { perror(argv[2]); return 2; }
    char buf[512];
    while (fgets(buf, sizeof(buf), f) && n < 4096) {
        if (buf[0] == '#' || buf[0] == '\n') continue;
        line_t *l = &L[n];
        memset(l, 0, sizeof(*l));
        int k = sscanf(buf, "%d %31s %127s %63s %63s", &l->frame, l->op, l->a, l->b, l->c);
        if (k >= 2) n++;
    }
    fclose(f);
    if (!plat_init() || !game_init()) { fprintf(stderr, "init failed\n"); return 1; }
    int fails = 0, li = 0, sc = 0, adv_left = 0, adv_idle = 0, adv_tick = 0;
    bool adv_until_banim = false;
    for (int fr = 0; fr < frames; fr++) {
        if (adv_left > 0) {
            adv_left--;
            bool busy = interp_busy() || ui_modal_active() || trans_busy() || qi < qn;
            if (adv_until_banim && banim_active()) {   /* stop while a battle animation is on screen */
                adv_left = 0; adv_until_banim = false;
                printf("[%d] battle animation on screen (script clock %d)\n", fr, sc);
                goto step;
            }
            if (busy) { adv_idle = 0; if (qi >= qn && (++adv_tick % 12) == 0) qpress(BTN_A); }
            else if (++adv_idle > 40) adv_left = 0;
            if (adv_left == 0) printf("[%d] advance finished (script clock %d)\n", fr, sc);
            goto step;
        }
        while (li < n && L[li].frame == sc) {
            line_t *l = &L[li++];
            if (!strcmp(l->op, "shot")) {
                char p[600];
                snprintf(p, sizeof(p), "%s/%s.ppm", shots, l->a);
                game_frame();                /* render current state */
                plat_screenshot(p);
                printf("[%d] screenshot %s\n", fr, p);
            } else if (!strcmp(l->op, "expect-var")) {
                int got = game_var(atoi(l->a)), want = atoi(l->b);
                printf("[%d] expect var%s == %d: got %d %s\n", fr, l->a, want, got, got == want ? "PASS" : "FAIL");
                fails += got != want;
            } else if (!strcmp(l->op, "expect-sw")) {
                int got = game_sw(atoi(l->a)), want = atoi(l->b);
                printf("[%d] expect sw%s == %d: got %d %s\n", fr, l->a, want, got, got == want ? "PASS" : "FAIL");
                fails += got != want;
            } else if (!strcmp(l->op, "expect-map")) {
                int want = atoi(l->a);
                printf("[%d] expect map %d: got %d %s\n", fr, want, GS.map_id, GS.map_id == want ? "PASS" : "FAIL");
                fails += GS.map_id != want;
            } else if (!strcmp(l->op, "expect-file")) {
                bool got = vfs_file_exists(l->a), want = atoi(l->b) != 0;
                printf("[%d] expect file %s exists=%d: got %d %s\n", fr, l->a, want, got, got == want ? "PASS" : "FAIL");
                fails += got != want;
            } else if (!strcmp(l->op, "expect-busy")) {
                bool got = interp_busy(), want = atoi(l->a) != 0;
                printf("[%d] expect busy=%d: got %d %s\n", fr, want, got, got == want ? "PASS" : "FAIL"); fails += got != want;
            } else if (!strcmp(l->op, "common")) {
                interp_start_common(atoi(l->a));
            } else if (!strcmp(l->op, "advance-until-banim")) {
                adv_left = atoi(l->a); adv_idle = 0; adv_tick = 0; adv_until_banim = true;
            } else if (!strcmp(l->op, "expect-banim-count")) {
                int got = banim_count(), want = atoi(l->a);
                printf("[%d] expect >= %d battle animations loaded: got %d %s\n", fr, want, got, got >= want ? "PASS" : "FAIL"); fails += got < want;
            } else if (!strcmp(l->op, "advance")) {
                adv_left = atoi(l->a); adv_idle = 0; adv_tick = 0;
            } else if (!strcmp(l->op, "warp")) {
                trans_teleport(atoi(l->a), atoi(l->b), atoi(l->c), 1);
            } else if (!strcmp(l->op, "setsw")) {
                game_set_sw(atoi(l->a), atoi(l->b) != 0);
            } else if (!strcmp(l->op, "setvar")) {
                game_set_var(atoi(l->a), atoi(l->b));
            } else if (!strcmp(l->op, "item")) {
                int id = atoi(l->a); if (id >= 1 && id <= MAX_ITEMS) GS.items[id] = (uint8_t)atoi(l->b);
            } else if (!strcmp(l->op, "expect-page")) {    /* last map event page started: <event> <page> */
                int ev, pg; interp_test_last_page(&ev, &pg);
                bool ok = ev == atoi(l->a) && pg == atoi(l->b);
                printf("[%d] expect event %s page %s started: got event %d page %d %s\n", fr, l->a, l->b, ev, pg, ok ? "PASS" : "FAIL"); fails += !ok;
            } else if (!strcmp(l->op, "expect-ce")) {      /* common event <id> ran (1) or not (0) since the last expect-ce for it */
                int n = interp_test_ce_calls(atoi(l->a), true); bool want = atoi(l->b) != 0;
                printf("[%d] expect common event %s called=%d: got %d call(s) %s\n", fr, l->a, want, n, (n > 0) == want ? "PASS" : "FAIL"); fails += (n > 0) != want;
            } else if (!strcmp(l->op, "reset-ce")) {
                interp_test_ce_calls(atoi(l->a), true);
            } else if (!strcmp(l->op, "store-mode")) {     /* emulated VMU: ok | full | none (pulled out) */
                host_store_mode_t m2 = !strcmp(l->a, "full") ? HOST_STORE_FULL : !strcmp(l->a, "none") ? HOST_STORE_NONE : HOST_STORE_OK;
                host_set_store(store, m2, 128 * 1024);
                printf("[%d] memory card now: %s\n", fr, l->a);
            } else if (!strcmp(l->op, "expect-commit")) {  /* last save commit: ok | session (not on card) */
                const char *why; persist_status_t st = bridge_last_commit(&why);
                bool ok = bridge_commit_count() > 0 && (!strcmp(l->a, "ok") ? st == PERSIST_OK : st != PERSIST_OK);
                printf("[%d] expect last save %s: got %s (%s) %s\n", fr, l->a, st == PERSIST_OK ? "ok" : "not on card", why, ok ? "PASS" : "FAIL"); fails += !ok;
            } else if (!strcmp(l->op, "print-save-size")) {
                size_t pl = persist_last_payload_size();
                printf("[%d] save payload %u bytes -> %d VMU blocks per copy, %d blocks for both copies (%u bytes on the card)\n", fr,
                       (unsigned)pl, persist_vmu_blocks_per_copy(pl), 2 * persist_vmu_blocks_per_copy(pl), (unsigned)(2 * persist_vmu_blocks_per_copy(pl) * 512));
            } else if (!strcmp(l->op, "expect-tex-failures")) {   /* textures that failed to load/upload */
                int got = gfx_tex_failures(), want = atoi(l->a);
                printf("[%d] expect texture failures %d: got %d %s\n", fr, want, got, got == want ? "PASS" : "FAIL"); fails += got != want;
            } else if (!strcmp(l->op, "print-screen")) {
                printf("[%d] screen: transition fade %d, erase level %d, flash %d, tint %d %d %d %d, display (%d,%d) player (%d,%d), pictures:", fr, trans_fade_level(), GS.fade_level, GS.flash_level, GS.tint[0], GS.tint[1], GS.tint[2], GS.tint[3], GS.display_x / REAL_TILE, GS.display_y / REAL_TILE, GS.player.x, GS.player.y);
                for (int i = 1; i <= MAX_PICTURES; i++) if (GS.pic[i].shown) printf(" %d:%s(tr %.0f)", i, GS.pic[i].name, GS.pic[i].trans_top);
                printf("\n");
            } else if (!strcmp(l->op, "print-gfx")) {
                printf("[%d] textures cached %d, VRAM in use %d KB, big-image window refreshes %d\n", fr, gfx_cache_count(), plat_tex_bytes_in_use() / 1024, gfx_big_window_uploads());
            } else if (!strcmp(l->op, "party")) {          /* party <actor> <1 add | 0 remove> */
                int a = atoi(l->a), k = -1;
                for (int i = 0; i < GS.party_n; i++) if (GS.party[i] == a) k = i;
                if (atoi(l->b) && k < 0 && GS.party_n < MAX_PARTY) GS.party[GS.party_n++] = a;
                if (!atoi(l->b) && k >= 0) { for (int i = k; i < GS.party_n - 1; i++) GS.party[i] = GS.party[i + 1]; GS.party_n--; }
                GS.need_refresh = true;
            } else if (!strcmp(l->op, "expect-active-page")) {   /* event's current page (1-based, 0 = none) */
                int got = -2;
                for (int i = 0; i < GS.nev; i++) if (GS.ev[i].id == atoi(l->a)) got = GS.ev[i].page + 1;
                bool ok = got == atoi(l->b);
                printf("[%d] expect event %s on page %s: got %d %s\n", fr, l->a, l->b, got, ok ? "PASS" : "FAIL"); fails += !ok;
            } else if (!strcmp(l->op, "route-player")) {   /* comma list of RM2003 move codes (no-arg codes only) */
                movecmd_t mc[32]; int nm = 0; char tmp[128]; os_strlcpy(tmp, l->a, sizeof(tmp));
                for (char *t = strtok(tmp, ","); t && nm < 32; t = strtok(NULL, ",")) { memset(&mc[nm], 0, sizeof(mc[0])); mc[nm++].code = (uint8_t)atoi(t); }
                char_set_route(&GS.player, mc, nm, false, atoi(l->b) != 0);
            } else if (!strcmp(l->op, "expect-pos")) {
                bool ok = GS.player.x == atoi(l->a) && GS.player.y == atoi(l->b);
                printf("[%d] expect player at (%s,%s): got (%d,%d) %s\n", fr, l->a, l->b, GS.player.x, GS.player.y, ok ? "PASS" : "FAIL"); fails += !ok;
            } else if (!strcmp(l->op, "expect-jumping")) {
                bool got = GS.player.jumping, want = atoi(l->a) != 0;
                printf("[%d] expect jumping=%d: got %d (height %d px) %s\n", fr, want, got, char_jump_height(&GS.player), got == want ? "PASS" : "FAIL"); fails += got != want;
            } else if (!strcmp(l->op, "expect-debug")) {   /* Debug Menu unlocked 0/1 */
                bool got = desktop_debug_unlocked(), want = atoi(l->a) != 0;
                printf("[%d] expect debug unlocked=%d: got %d %s\n", fr, want, got, got == want ? "PASS" : "FAIL"); fails += got != want;
            } else if (!strcmp(l->op, "expect-window")) {  /* focused desktop window title ('_' = space), "none" */
                char want[128]; os_strlcpy(want, l->a, sizeof(want));
                for (char *p = want; *p; p++) if (*p == '_') *p = ' ';
                const char *got = !desktop_active() ? "(desktop closed)" : desktop_open_window_count() ? desktop_focused_title() : "none";
                bool ok = !strcmp(got, want);
                printf("[%d] expect window \"%s\": got \"%s\" %s\n", fr, want, got, ok ? "PASS" : "FAIL"); fails += !ok;
            } else if (!strcmp(l->op, "expect-bgm")) {     /* current music name or "none" */
                const char *got = audio_bgm_current()[0] ? audio_bgm_current() : "none";
                bool ok = !os_stricmp(got, l->a);   /* asset names are matched case-insensitively */
                printf("[%d] expect bgm %s: got %s %s\n", fr, l->a, got, ok ? "PASS" : "FAIL"); fails += !ok;
            } else if (!strcmp(l->op, "expect-playback")) {
                bool got = dbg_playback_active(), want = atoi(l->a) != 0;
                printf("[%d] expect debug playback=%d: got %d %s\n", fr, want, got, got == want ? "PASS" : "FAIL"); fails += got != want;
            } else if (!strcmp(l->op, "remember-bgm")) {
                os_strlcpy(remembered_bgm, audio_bgm_current(), sizeof(remembered_bgm));
                printf("[%d] remembered music: %s\n", fr, remembered_bgm[0] ? remembered_bgm : "none");
            } else if (!strcmp(l->op, "expect-bgm-remembered")) {
                bool ok = !os_stricmp(audio_bgm_current(), remembered_bgm);
                printf("[%d] expect the remembered music: got %s %s\n", fr, audio_bgm_current(), ok ? "PASS" : "FAIL"); fails += !ok;
            } else if (!strcmp(l->op, "expect-bgm-index")) {   /* the Debug Menu music entry <i> is playing */
                char nm[64]; dbg_label(DBG_PAGE_MUSIC, atoi(l->a), nm, sizeof(nm));
                bool ok = nm[0] && !os_stricmp(audio_bgm_current(), nm);
                printf("[%d] expect music entry %s (%s): got %s %s\n", fr, l->a, nm, audio_bgm_current(), ok ? "PASS" : "FAIL"); fails += !ok;
            } else if (!strcmp(l->op, "bgm")) {            /* SETUP: start a music track as an event would */
                audio_bgm_play(l->a, 100, 100);
            } else if (!strcmp(l->op, "status")) {
                printf("[%d] debug status: %s\n", fr, dbg_status());
            } else if (!strcmp(l->op, "clear-items")) {
                memset(GS.items, 0, sizeof(GS.items));
            } else if (!strcmp(l->op, "equip")) {        /* equip <actor> <slot 0-4> <item|0> */
                int a = atoi(l->a), s = atoi(l->b);
                if (a >= 1 && a <= MAX_ACTORS && s >= 0 && s < 5) GS.actor[a].equip[s] = (uint16_t)atoi(l->c);
            } else if (!strcmp(l->op, "expect-item")) {  /* expect-item <id> <count> */
                int got = GS.items[atoi(l->a)], want = atoi(l->b);
                printf("[%d] expect item %s count %d: got %d %s\n", fr, l->a, want, got, got == want ? "PASS" : "FAIL");
                fails += got != want;
            } else if (!strcmp(l->op, "type-number")) {
                char digits[32] = "";
                if (l->a[0] == '@') {
                    size_t n = 0; char buf[64];
                    if (vfs_read_file(l->a + 1, buf, sizeof(buf) - 1, &n) == VFS_OK) {
                        int k = 0; for (size_t i = 0; i < n && k < 31; i++) if (buf[i] >= '0' && buf[i] <= '9') digits[k++] = buf[i];
                        digits[k] = 0;
                    }
                    printf("[%d] read %s from %s\n", fr, digits, l->a + 1);
                } else os_strlcpy(digits, l->a, sizeof(digits));
                for (char *d = digits; *d; d++) {
                    for (int k = 0; k < *d - '0'; k++) qpress(BTN_UP);
                    if (d[1]) qpress(BTN_RIGHT);
                }
                qpress(BTN_A);
            } else if (!strcmp(l->op, "bridge")) {
                game_set_var(VAR_FUNC, atoi(l->a)); game_set_var(VAR_ARG1, atoi(l->b));
                bool sync = bridge_invoke(false);
                printf("[%d] bridge fn %s arg %s -> sync=%d return=%d\n", fr, l->a, l->b, sync, (int)game_var(VAR_RETURN));
            } else if (!strcmp(l->op, "expect-opened")) {
                bool got = desktop_file_was_opened(l->a);
                printf("[%d] expect opened %s: %s\n", fr, l->a, got ? "PASS" : "FAIL"); fails += !got;
            } else if (!strcmp(l->op, "expect-ending")) {
                int want = atoi(l->a);
                printf("[%d] expect ending %d: got %d %s\n", fr, want, (int)BM.ending, BM.ending == want ? "PASS" : "FAIL"); fails += BM.ending != want;
            } else if (!strcmp(l->op, "expect-wallpaper")) {
                bool ok = !strcmp(desktop_wallpaper(), l->a);
                printf("[%d] expect wallpaper %s: got %s %s\n", fr, l->a, desktop_wallpaper(), ok ? "PASS" : "FAIL"); fails += !ok;
            } else if (!strcmp(l->op, "expect-trans")) {
                bool ok = !strcmp(trans_name(trans_last()), l->a);
                printf("[%d] expect last transition %s: got %s %s\n", fr, l->a, trans_name(trans_last()), ok ? "PASS" : "FAIL"); fails += !ok;
            } else if (!strcmp(l->op, "expect-msg")) {
                /* substring of any message shown so far since the last expect-msg ('_' = space) */
                char want[128]; os_strlcpy(want, l->a, sizeof(want));
                for (char *p = want; *p; p++) if (*p == '_') *p = ' ';
                bool ok = strstr(seen, want) != NULL;
                printf("[%d] expect message containing \"%s\": %s\n", fr, want, ok ? "PASS" : "FAIL"); fails += !ok;
                seen[0] = 0;
            } else if (!strcmp(l->op, "print")) {
                printf("[%d] map %d pos (%d,%d) dir %d busy %d\n", fr, GS.map_id, GS.player.x, GS.player.y, GS.player.facing, interp_busy());
            } else {
                host_set_buttons(parse_buttons(l->op));
            }
        }
        sc++;
    step:
        if (qi < qn) { /* queued presses: hold 2 frames, release 2 frames */
            host_set_buttons(Q[qi]);
            if (++qhold >= 2) { qhold = 0; qi++; }
        }
        game_frame();
        { /* log every distinct message the game shows (also feeds expect-msg) */
            char cur[1024];
            msg_current_text(cur, sizeof(cur));
            if (cur[0] && strcmp(cur, last_msg)) {
                printf("[%d] [msg] %s\n", fr, cur);
                os_strlcpy(last_msg, cur, sizeof(last_msg));
                size_t sl = strlen(seen);
                if (sl + strlen(cur) + 2 < sizeof(seen)) { strcat(seen, cur); strcat(seen, "|"); }
            }
        }
    }
    unsupported_t u[64];
    int nu = interp_unsupported(u, 64);
    printf("commands executed: %u; unsupported command kinds: %d\n", (unsigned)interp_commands_executed(), nu);
    for (int i = 0; i < nu && i < 64; i++) printf("  unsupported %u x%u\n", u[i].code, (unsigned)u[i].count);
    printf("RESULT: %s (%d failed expectations)\n", fails ? "FAIL" : "PASS", fails);
    return fails ? 1 : 0;
}
