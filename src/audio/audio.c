/* SPDX-License-Identifier: GPL-3.0-or-later */
#include "audio.h"
#include "../resources/gamedata.h"
#include "../game/game.h"
#include "../platform/platform.h"

#define SE_CACHE 48
typedef struct { bool used; char name[NAME_LEN]; int handle; } se_ent_t;
static se_ent_t s_se[SE_CACHE];
static char s_bgm[NAME_LEN];
static int s_fade_frames, s_fade_total, s_bgm_vol = 100;
static char s_last_se[NAME_LEN];
static uint32_t s_se_count;

void audio_init(void) { memset(s_se, 0, sizeof(s_se)); s_bgm[0] = 0; }

void audio_purge_room(void) {
    plat_sfx_unload_all();
    memset(s_se, 0, sizeof(s_se));
}

static int se_handle(const char *name) {
    for (int i = 0; i < SE_CACHE; i++) if (s_se[i].used && os_stricmp(s_se[i].name, name) == 0) return s_se[i].handle;
    const asset_t *a = asset_find(CAT_SOUND, name);
    if (!a) { os_diag("sound missing: %s", name); return -1; }
    int h = plat_sfx_load(a->path);
    for (int i = 0; i < SE_CACHE; i++)
        if (!s_se[i].used) { s_se[i].used = true; os_strlcpy(s_se[i].name, name, NAME_LEN); s_se[i].handle = h; break; }
    return h;
}

void audio_se_play(const char *name, int volume, int pan) {
    if (!name || !name[0] || os_stricmp(name, "(OFF)") == 0) return;
    if (name[0] == '_') return; /* bridge marker sounds (_func/_init) are silent */
    os_strlcpy(s_last_se, name, sizeof(s_last_se));
    s_se_count++;
    int h = se_handle(name);
    if (h >= 0) plat_sfx_play(h, clampi(volume, 0, 100) * 255 / 100, clampi(pan, 0, 100) * 255 / 100);
}

void audio_system_se(int which) {
    if (which < 0 || which > 3) return;
    const sound_ref_t *s = &GS.sys_se[which];
    audio_se_play(s->name, s->vol, s->pan);
}

void audio_bgm_play(const char *name, int volume, int tempo) {
    (void)tempo;
    if (!name || !name[0] || os_stricmp(name, "(OFF)") == 0) { plat_bgm_stop(); s_bgm[0] = 0; return; }
    s_fade_frames = 0;
    s_bgm_vol = clampi(volume, 0, 100);
    if (os_stricmp(name, s_bgm) == 0) { plat_bgm_volume(s_bgm_vol * 255 / 100); return; }
    const asset_t *a = asset_find(CAT_MUSIC, name);
    os_strlcpy(s_bgm, name, sizeof(s_bgm));
    os_strlcpy(GS.bgm_name, name, sizeof(GS.bgm_name));
    if (!a) { os_diag("music missing: %s", name); return; }
    plat_bgm_play(a->path, s_bgm_vol * 255 / 100, true);
}

void audio_bgm_fadeout(int ms) {
    s_fade_total = s_fade_frames = ms * 60 / 1000;
    if (s_fade_frames <= 0) { plat_bgm_stop(); s_bgm[0] = 0; }
}

void audio_bgm_update(void) {
    if (s_fade_frames > 0) {
        s_fade_frames--;
        plat_bgm_volume(s_bgm_vol * 255 / 100 * s_fade_frames / (s_fade_total ? s_fade_total : 1));
        if (s_fade_frames == 0) { plat_bgm_stop(); s_bgm[0] = 0; GS.bgm_name[0] = 0; }
    }
    plat_audio_update();
}

const char *audio_bgm_current(void) { return s_bgm; }
int audio_bgm_volume(void) { return s_bgm_vol; }
const char *audio_last_se(void) { return s_last_se; }
uint32_t audio_se_count(void) { return s_se_count; }
