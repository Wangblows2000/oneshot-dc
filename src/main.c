/* SPDX-License-Identifier: GPL-3.0-or-later
 * OneShot (2014) Dreamcast runtime - boot and main loop. */
#include "common.h"
#include "platform/platform.h"
#include "resources/gamedata.h"
#include "renderer/gfx.h"
#include "game/game.h"
#include "game/transition.h"
#include "game/banim.h"
#include "events/interp.h"
#include "events/bridge.h"
#include "dialogue/message.h"
#include "audio/audio.h"
#include "ui/ui.h"
#include "desktop/desktop.h"
#include "desktop/debugmenu.h"
#include "vfs/vfs.h"
#include "save/save.h"
#include "main.h"
#include <stdio.h>
#include <stdlib.h>
#include <malloc.h>

extern int persist_boot_meta(void);

static uint32_t s_prev;
static int s_hint_frames;
static uint64_t s_frame_no;
static canvas_t s_diag_cv;
static bool s_diag_ok;
static int s_title_sel;

/* ------------------------------------------------------------ screen effects */
static void update_pictures(void) {
    for (int i = 1; i <= MAX_PICTURES; i++) {
        picture_t *p = &GS.pic[i];
        if (!p->shown || p->duration <= 0) continue;
        float d = (float)p->duration;
        p->x += (p->tx - p->x) / d; p->y += (p->ty - p->y) / d; p->mag += (p->tmag - p->mag) / d;
        p->trans_top += (p->ttop - p->trans_top) / d; p->trans_bot += (p->tbot - p->trans_bot) / d;
        p->r += (int)((p->tr - p->r) / d); p->g += (int)((p->tg - p->g) / d); p->b += (int)((p->tb - p->b) / d);
        p->duration--;
        if (p->duration == 0) { p->r = p->tr; p->g = p->tg; p->b = p->tb; p->sat = p->tsat; }
    }
}

static void update_screen(void) {
    if (GS.tint_frames > 0) {
        for (int k = 0; k < 4; k++) GS.tint[k] += (GS.tint_target[k] - GS.tint[k]) / GS.tint_frames;
        if (--GS.tint_frames == 0) memcpy(GS.tint, GS.tint_target, sizeof(GS.tint));
    }
    if (GS.flash_frames > 0) { GS.flash_level -= GS.flash_level / GS.flash_frames; GS.flash_frames--; }
    else GS.flash_level = 0;
    if (GS.shake_frames > 0) {
        GS.shake_frames--;
        int amp = GS.shake_power * 2;
        GS.shake_ofs += (GS.shake_dir ? 1 : -1) * clampi(GS.shake_speed, 1, 9);
        if (GS.shake_ofs > amp) GS.shake_dir = 0;
        if (GS.shake_ofs < -amp) GS.shake_dir = 1;
    } else if (GS.shake_ofs) GS.shake_ofs += GS.shake_ofs > 0 ? -1 : 1;
    update_pictures();
}

static void draw_pictures(void) {
    for (int i = 1; i <= MAX_PICTURES; i++) {
        picture_t *p = &GS.pic[i];
        if (!p->shown) continue;
        const gtex_t *t = gfx_tex(CAT_PICTURE, p->name, p->keyed, LIFE_ROOM);
        if (!t->valid) continue;
        float sc = p->mag / 100.0f;
        float w = t->w * sc, h = t->h * sc;
        float x = p->x - w / 2, y = p->y - h / 2;
        if (p->fixed_to_map) { x -= (float)GS.display_x * TILE / REAL_TILE; y -= (float)GS.display_y * TILE / REAL_TILE; }
        int a = clampi((int)(255.0f * (100.0f - p->trans_top) / 100.0f), 0, 255);
        if (!a) continue;
        int r = clampi(p->r, 0, 200), g = clampi(p->g, 0, 200), b = clampi(p->b, 0, 200);
        uint32_t mod = (uint32_t)a << 24 | (uint32_t)(r > 100 ? 255 : r * 255 / 100) << 16 |
                       (uint32_t)(g > 100 ? 255 : g * 255 / 100) << 8 | (uint32_t)(b > 100 ? 255 : b * 255 / 100);
        uint32_t add = (uint32_t)(r > 100 ? (r - 100) * 255 / 100 : 0) << 16 | (uint32_t)(g > 100 ? (g - 100) * 255 / 100 : 0) << 8 |
                       (uint32_t)(b > 100 ? (b - 100) * 255 / 100 : 0);
        gfx_draw_tex(t, 0, 0, t->w, t->h, x + GS.shake_ofs, y, w, h, mod, add);
    }
}

static void draw_hint(void) {
    if (s_hint_frames <= 0) return;
    s_hint_frames--;
    static canvas_t cv;
    static bool ok;
    if (!ok) { ok = canvas_init(&cv, 120, 14); canvas_text(&cv, 0, 0, "START: computer", 0); }
    plat_fill(SCREEN_W - 120 - 16, 12, 112, 16, 0x80000000);
    canvas_draw(&cv, SCREEN_W - 120 - 12, 14, 0xFFFFFFFF);
}

/* performance counters (diagnostics overlay + periodic log) */
static uint64_t s_perf_t0, s_perf_frames, s_cpu_acc;
static int s_fps, s_cpu_ms10;
static void perf_frame(uint64_t cpu_ms_x10) {
    uint64_t now = plat_ticks_ms();
    if (!s_perf_t0) s_perf_t0 = now;
    s_perf_frames++; s_cpu_acc += cpu_ms_x10;
    if (now - s_perf_t0 >= 1000) {
        s_fps = (int)(s_perf_frames * 1000 / (now - s_perf_t0));
        s_cpu_ms10 = (int)(s_cpu_acc / s_perf_frames);
        if (g_diag_mode) {
            struct mallinfo mi = mallinfo();
            os_log("perf: %d fps, cpu %d.%d ms/frame, heap %d KB, vram textures %d KB", s_fps, s_cpu_ms10 / 10, s_cpu_ms10 % 10,
                   (int)(mi.uordblks / 1024), plat_tex_bytes_in_use() / 1024);
        }
        s_perf_t0 = now; s_perf_frames = 0; s_cpu_acc = 0;
    }
}

static void draw_diag(void) {
    if (!g_diag_mode) return;
    if (!s_diag_ok) s_diag_ok = canvas_init(&s_diag_cv, 300, 40);
    char l[96];
    canvas_clear(&s_diag_cv);
    struct mallinfo mi = mallinfo();
    snprintf(l, sizeof(l), "%d fps cpu %d.%dms heap %dK tex %dK free %dK fail %d", s_fps, s_cpu_ms10 / 10, s_cpu_ms10 % 10,
             (int)(mi.uordblks / 1024), plat_tex_bytes_in_use() / 1024, plat_vram_free() / 1024, gfx_tex_failures());
    canvas_text(&s_diag_cv, 0, 26, l, 0);
    snprintf(l, sizeof(l), "map %d (%d,%d) ev %d tex %d cmds %u", GS.map_id, GS.player.x, GS.player.y, GS.nev,
             gfx_cache_count(), (unsigned)interp_commands_executed());
    canvas_text(&s_diag_cv, 0, 0, l, 0);
    snprintf(l, sizeof(l), "bridge fn %d -> %d  vfs s%u p%u  %s", bridge_last_func(), bridge_last_result(),
             (unsigned)vfs_used_bytes(VFS_LAYER_SESSION), (unsigned)vfs_used_bytes(VFS_LAYER_PERSISTENT),
             persist_storage_available() ? "VMU" : "RAM-only");
    canvas_text(&s_diag_cv, 0, 13, l, 0);
    plat_fill(8, SCREEN_H - 48, 304, 42, 0xA0000000);
    canvas_draw(&s_diag_cv, 10, SCREEN_H - 46, 0xFFFFFFFF);
}

/* ------------------------------------------------------------ title */
static void title_update(uint32_t pr) {
    if (pr & (BTN_UP | BTN_DOWN)) { s_title_sel ^= 1; audio_system_se(0); }
    if (pr & (BTN_A | BTN_START)) {
        audio_system_se(1);
        if (s_title_sel == 0) game_boot_new_session();
        else desktop_open();
    }
}
static void title_draw(void) {
    /* The original title picture already contains the words "start" and
     * "quit"; the cursor is placed on them.  "quit" leaves the game for the
     * virtual computer (there is no OS to return to on a Dreamcast). */
    const gtex_t *t = gfx_tex(CAT_TITLE, G.title_name, false, LIFE_PERSISTENT);
    if (t->valid) gfx_draw_tex(t, 0, 0, t->w, t->h, 0, 0, SCREEN_W, SCREEN_H, 0xFFFFFFFF, 0);
    int y = s_title_sel == 0 ? 164 : 196;
    gfx_cursor(208, y - 4, 48, 18, (int)s_frame_no);
}

/* ------------------------------------------------------------ boot */
bool game_init(void) {
    if (!gamedata_load("game.dat")) return false;
    if (!gfx_init()) return false;
    vfs_reset();
    size_t bl = 0;
    void *base = plat_load_file("vfs_base.dat", &bl, 256 * 1024);
    if (!base || vfs_load_base(base, bl) != VFS_OK) os_log("vfs: base image missing or invalid (continuing with empty disc layer)");
    free(base);
    bridge_init();
    desktop_init();
    ui_init();
    audio_init();
    interp_init();
    trans_init();
    msg_reset();
    game_new();
    gfx_set_system(G.system_name);
    /* power-on: storage probe + persistent meta (ending, name, VFS) */
    persist_probe_storage();
    int r = persist_boot_meta();
    BM.launches++;
    if (!persist_storage_available())
        ui_notice("Memory card", "No memory card (VMU) was found.\n\nYou can play normally, but progress\nwill be lost when the console is\nswitched off.");
    else if (r == SAVE_CORRUPT || r == SAVE_UNSUPPORTED)
        ui_notice("Memory card", "The OneShot data on the memory card\ncould not be read. It has not been\nerased. A new game will start.");
    trans_set_scene(SCENE_TITLE);
    s_hint_frames = 60 * 6;
    os_log("boot complete: storage=%s launches=%u", persist_storage_available() ? "VMU" : "none", (unsigned)BM.launches);
    return true;
}

void game_frame(void) {
    uint64_t t_start = plat_ticks_ms();
    uint32_t held = plat_poll_buttons();
    uint32_t pressed = held & ~s_prev;
    s_prev = held;
    s_frame_no++;

    /* ---- update ---- */
    if (dbg_playback_abort_check(pressed)) {
        /* debug cutscene stopped with B: state restored, back on the desktop */
    } else if (ui_modal_active()) {
        ui_update(pressed, held);
    } else if (desktop_active()) {
        desktop_update(pressed, held);
    } else if (ui_menu_active()) {
        ui_update(pressed, held);
    } else if (trans_scene() == SCENE_TITLE) {
        title_update(pressed);
        trans_update();
    } else if (trans_scene() == SCENE_MAP) {
        if ((pressed & BTN_START) && !dbg_playback_active()) {
            desktop_open();
        } else {
            bool idle = !interp_busy() && !trans_busy() && !msg_active();
            if (idle && (pressed & BTN_B) && GS.menu_enabled) ui_menu_open();
            interp_update();
            banim_update();
            if (dbg_playback_after_interp()) goto updated;   /* debug cutscene ended -> Debug Menu */
            bridge_tick();
            map_update(idle);
            msg_update(pressed, held);
            update_screen();
            trans_update();
            map_gc_retired();
        }
    }
updated:
    map_note_buttons(held);
    audio_bgm_update();

    /* ---- draw ---- */
    uint64_t t_update = plat_ticks_ms() - t_start;
    plat_frame_begin();                            /* waits for the PVR / vsync on Dreamcast */
    uint64_t t_draw0 = plat_ticks_ms();
    if (trans_scene() == SCENE_TITLE) title_draw();
    else if (trans_scene() == SCENE_MAP) {
        map_draw();
        draw_pictures();
        banim_draw();
        if (GS.flash_level > 0)
            plat_fill(0, 0, SCREEN_W, SCREEN_H, (uint32_t)clampi(GS.flash_level, 0, 255) << 24 |
                      (uint32_t)GS.flash_rgb[0] << 16 | (uint32_t)GS.flash_rgb[1] << 8 | (uint32_t)GS.flash_rgb[2]);
        msg_draw();
        trans_draw();
        draw_hint();
    }
    if (desktop_active()) desktop_draw();
    ui_draw();
    draw_diag();
    uint64_t t_cpu = t_update + (plat_ticks_ms() - t_draw0);   /* update + draw submission, excl. vsync wait */
    plat_frame_end();
    perf_frame(t_cpu * 10);
}

uint64_t game_frame_count(void) { return s_frame_no; }

#ifndef OS_NO_MAIN
int main(int argc, char **argv) {
    (void)argc; (void)argv;
    if (!plat_init()) return 1;
    if (!game_init()) {
        os_log("FATAL: game data could not be loaded - see log above");
        for (;;) { plat_frame_begin(); plat_fill(0, 0, SCREEN_W, SCREEN_H, 0xFF400000); plat_frame_end(); }
    }
    for (;;) game_frame();
    return 0;
}
#endif
