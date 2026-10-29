/* SPDX-License-Identifier: GPL-3.0-or-later */
#include "gamedata.h"
#include "../platform/platform.h"
#include <stdlib.h>
#include <stdio.h>

gamedata_t G;

#define GAME_MAX_BYTES (4u * 1024 * 1024)
#define MAP_MAX_BYTES (4u * 1024 * 1024)
#define MAX_TOTAL_CMDS 200000u

static bool read_cmdlist(reader_t *r, const void *base, cmdlist_t *out) {
    rd_align4(r, base);
    uint32_t n = rd_u32(r);
    if (r->err || n > MAX_TOTAL_CMDS) return false;
    out->count = n;
    out->cmds = n ? (cmd_t *)calloc(n, sizeof(cmd_t)) : NULL;
    if (n && !out->cmds) return false;
    for (uint32_t i = 0; i < n; i++) {
        cmd_t *c = &out->cmds[i];
        c->code = rd_u16(r); c->indent = rd_u16(r); c->nparams = rd_u16(r); c->slen = rd_u16(r);
        if (c->nparams > MAX_PARAMS) { r->err = true; return false; }
        c->params = (const int32_t *)rd_bytes(r, (size_t)c->nparams * 4);
        c->str = (const char *)rd_bytes(r, c->slen);
        rd_align4(r, base);
        if (r->err) return false;
    }
    return true;
}

static void read_sound(reader_t *r, sound_ref_t *s) {
    rd_str(r, s->name, sizeof(s->name));
    s->vol = rd_u8(r); s->tempo = rd_u8(r); s->pan = rd_u8(r);
}

bool gamedata_load(const char *rel) {
    size_t len = 0;
    uint8_t *buf = (uint8_t *)plat_load_file(rel, &len, GAME_MAX_BYTES);
    if (!buf) { os_log("gamedata: cannot load %s", rel); return false; }
    memset(&G, 0, sizeof(G));
    G.blob = buf;
    reader_t r;
    rd_init(&r, buf, len);
    const uint8_t *m = rd_bytes(&r, 4);
    if (!m || memcmp(m, "OSGD", 4) || rd_u16(&r) != 1) { os_log("gamedata: bad header"); return false; }
    G.n_switches = rd_u16(&r); G.n_vars = rd_u16(&r);
    if (G.n_switches >= MAX_SWITCHES || G.n_vars >= MAX_VARIABLES) { os_log("gamedata: too many switches/vars"); return false; }
    rd_str(&r, G.system_name, NAME_LEN); rd_str(&r, G.system2_name, NAME_LEN);
    rd_str(&r, G.title_name, NAME_LEN); rd_str(&r, G.gameover_name, NAME_LEN);
    read_sound(&r, &G.se_cursor); read_sound(&r, &G.se_decision); read_sound(&r, &G.se_cancel); read_sound(&r, &G.se_buzzer);
    rd_str(&r, G.title_music, NAME_LEN);
    G.transition_out = rd_u8(&r); G.transition_in = rd_u8(&r); G.message_stretch = rd_u8(&r);

    G.n_actors = rd_u16(&r);
    if (G.n_actors > MAX_ACTORS) { os_log("gamedata: too many actors"); return false; }
    for (int i = 1; i <= G.n_actors; i++) {
        actor_def_t *a = &G.actors[i];
        rd_str(&r, a->name, NAME_LEN); rd_str(&r, a->charset, NAME_LEN); a->char_index = rd_u8(&r);
        rd_str(&r, a->face, NAME_LEN); a->face_index = rd_u8(&r);
    }
    G.n_items = rd_u16(&r);
    if (G.n_items > MAX_ITEMS) { os_log("gamedata: too many items"); return false; }
    for (int i = 1; i <= G.n_items; i++) {
        rd_str(&r, G.item_names[i], NAME_LEN); G.item_type[i] = rd_u8(&r); G.item_switch[i] = rd_u16(&r);
        rd_str(&r, G.item_desc[i], sizeof(G.item_desc[i]));
    }
    G.n_skills = rd_u16(&r);
    if (G.n_skills > MAX_SKILLS) { os_log("gamedata: too many skills"); return false; }
    for (int i = 1; i <= G.n_skills; i++) {
        rd_str(&r, G.skill_names[i], NAME_LEN); G.skill_type[i] = rd_u8(&r); G.skill_switch[i] = rd_u16(&r);
        rd_str(&r, G.skill_desc[i], sizeof(G.skill_desc[i]));
    }
    G.n_menu_commands = rd_u8(&r);
    if (G.n_menu_commands > 8) { os_log("gamedata: bad menu"); return false; }
    for (int i = 0; i < G.n_menu_commands; i++) G.menu_commands[i] = rd_u8(&r);
    rd_str(&r, G.term_items, 32); rd_str(&r, G.term_skills, 32); rd_str(&r, G.term_equip, 32);
    rd_str(&r, G.term_quit, 32); rd_str(&r, G.term_quit_msg, 96); rd_str(&r, G.term_yes, 32); rd_str(&r, G.term_no, 32);

    G.n_chipsets = rd_u16(&r);
    if (G.n_chipsets > MAX_CHIPSETS) { os_log("gamedata: too many chipsets"); return false; }
    for (int i = 1; i <= G.n_chipsets; i++) {
        chipset_def_t *c = &G.chipsets[i];
        rd_str(&r, c->file, NAME_LEN); c->anim_type = rd_u8(&r); c->anim_speed = rd_u8(&r);
        const uint8_t *pl = rd_bytes(&r, 162), *pu = rd_bytes(&r, 144);
        if (pl) memcpy(c->pass_lower, pl, 162);
        if (pu) memcpy(c->pass_upper, pu, 144);
    }
    G.n_common = rd_u16(&r);
    if (G.n_common > MAX_COMMON_EVENTS) { os_log("gamedata: too many common events"); return false; }
    for (int i = 1; i <= G.n_common; i++) {
        common_event_t *ce = &G.common[i];
        rd_str(&r, ce->name, NAME_LEN); ce->trigger = rd_u8(&r); ce->switch_flag = rd_u8(&r); ce->switch_id = rd_u16(&r);
        if (!read_cmdlist(&r, buf, &ce->list)) { os_log("gamedata: bad CE %d", i); return false; }
    }
    G.n_mapinfo = rd_u16(&r);
    if (G.n_mapinfo > MAX_MAPINFO) { os_log("gamedata: too many maps"); return false; }
    for (int i = 0; i < G.n_mapinfo; i++) {
        mapinfo_t *mi = &G.mapinfo[i];
        mi->id = rd_u16(&r); rd_str(&r, mi->name, NAME_LEN); mi->parent = rd_u16(&r);
        mi->music_type = rd_u8(&r); rd_str(&r, mi->music, NAME_LEN); mi->converted = rd_u8(&r);
    }
    G.start_map = rd_u16(&r); G.start_x = rd_u16(&r); G.start_y = rd_u16(&r);
    G.n_assets = rd_u16(&r);
    if (G.n_assets > MAX_ASSETS) { os_log("gamedata: too many assets"); return false; }
    for (int i = 0; i < G.n_assets; i++) {
        asset_t *a = &G.assets[i];
        a->cat = rd_u8(&r); rd_str(&r, a->name, NAME_LEN); rd_str(&r, a->path, sizeof(a->path));
    }
    if (r.err) { os_log("gamedata: truncated/corrupt"); return false; }
    os_log("gamedata: %d switches, %d vars, %d CEs, %d maps, %d assets",
           G.n_switches, G.n_vars, G.n_common, G.n_mapinfo, G.n_assets);
    return true;
}

const asset_t *asset_find(int cat, const char *name) {
    if (!name || !name[0]) return NULL;
    for (int i = 0; i < G.n_assets; i++)
        if (G.assets[i].cat == cat && os_stricmp(G.assets[i].name, name) == 0) return &G.assets[i];
    return NULL;
}

const mapinfo_t *mapinfo_find(int id) {
    for (int i = 0; i < G.n_mapinfo; i++) if (G.mapinfo[i].id == id) return &G.mapinfo[i];
    return NULL;
}

/* ------------------------------------------------------------------ maps */
void mapdata_free(mapdata_t *m) {
    if (m->events) {
        for (int i = 0; i < m->nevents; i++) {
            event_def_t *e = &m->events[i];
            for (int p = 0; p < e->npages; p++) { free(e->pages[p].route); free(e->pages[p].list.cmds); }
            free(e->pages);
        }
        free(m->events);
    }
    free(m->lower); free(m->upper); free(m->blob);
    memset(m, 0, sizeof(*m));
}

bool mapdata_load(int id, mapdata_t *m) {
    memset(m, 0, sizeof(*m));
    char path[32];
    snprintf(path, sizeof(path), "map/%04d.map", id);
    size_t len = 0;
    uint8_t *buf = (uint8_t *)plat_load_file(path, &len, MAP_MAX_BYTES);
    if (!buf) { os_log("map %d: missing", id); return false; }
    m->blob = buf; m->blob_len = len;
    reader_t r;
    rd_init(&r, buf, len);
    const uint8_t *mg = rd_bytes(&r, 4);
    if (!mg || memcmp(mg, "OSMP", 4)) goto bad;
    uint16_t mver = rd_u16(&r);
    if (mver < 1 || mver > 2) goto bad;
    m->w = rd_u16(&r); m->h = rd_u16(&r); m->chipset_id = rd_u16(&r);
    m->scroll_type = rd_u8(&r); m->parallax_flag = rd_u8(&r);
    rd_str(&r, m->parallax_name, NAME_LEN);
    m->px_loop_x = rd_u8(&r); m->px_loop_y = rd_u8(&r); m->px_auto_x = rd_u8(&r); m->px_sx = rd_i16(&r);
    m->px_auto_y = rd_u8(&r); m->px_sy = rd_i16(&r);
    if (r.err || !m->w || !m->h || m->w > MAX_MAP_W || m->h > MAX_MAP_H || m->chipset_id > G.n_chipsets) goto bad;
    size_t cells = (size_t)m->w * m->h;
    const uint8_t *lo = rd_bytes(&r, cells * 2), *up = rd_bytes(&r, cells * 2);
    m->lower_q = rd_bytes(&r, cells * 32);
    m->upper_q = rd_bytes(&r, cells * 8);
    if (r.err) goto bad;
    m->lower = (uint16_t *)malloc(cells * 2); m->upper = (uint16_t *)malloc(cells * 2);
    if (!m->lower || !m->upper) goto bad;
    for (size_t i = 0; i < cells; i++) {
        m->lower[i] = (uint16_t)(lo[i * 2] | (lo[i * 2 + 1] << 8));
        m->upper[i] = (uint16_t)(up[i * 2] | (up[i * 2 + 1] << 8));
    }
    m->nevents = rd_u16(&r);
    if (m->nevents > MAX_EVENTS) goto bad;
    m->events = (event_def_t *)calloc(m->nevents ? m->nevents : 1, sizeof(event_def_t));
    if (!m->events) goto bad;
    for (int i = 0; i < m->nevents; i++) {
        event_def_t *e = &m->events[i];
        e->id = rd_u16(&r); e->x = rd_i16(&r); e->y = rd_i16(&r); rd_str(&r, e->name, NAME_LEN);
        e->npages = rd_u16(&r);
        if (r.err || e->npages > MAX_PAGES) goto bad;
        e->pages = (page_def_t *)calloc(e->npages ? e->npages : 1, sizeof(page_def_t));
        if (!e->pages) goto bad;
        for (int p = 0; p < e->npages; p++) {
            page_def_t *pg = &e->pages[p];
            pg->cond_flags = rd_u8(&r); pg->switch_a = rd_u16(&r); pg->switch_b = rd_u16(&r); pg->var_id = rd_u16(&r);
            pg->var_value = rd_i32(&r); pg->item_id = rd_u16(&r); pg->compare_op = rd_u8(&r);
            pg->actor_id = mver >= 2 ? rd_u16(&r) : 0;
            rd_str(&r, pg->charset, NAME_LEN);
            pg->char_index = rd_u8(&r); pg->direction = rd_u8(&r); pg->pattern = rd_u8(&r); pg->translucent = rd_u8(&r);
            pg->move_type = rd_u8(&r); pg->move_freq = rd_u8(&r); pg->trigger = rd_u8(&r); pg->layer = rd_u8(&r);
            pg->overlap_forbidden = rd_u8(&r); pg->anim_type = rd_u8(&r); pg->move_speed = rd_u8(&r);
            pg->route_repeat = rd_u8(&r); pg->route_skippable = rd_u8(&r); pg->route_len = rd_u16(&r);
            if (r.err || pg->route_len > 1024) goto bad;
            if (pg->route_len) {
                pg->route = (movecmd_t *)calloc(pg->route_len, sizeof(movecmd_t));
                if (!pg->route) goto bad;
                for (int k = 0; k < pg->route_len; k++) {
                    movecmd_t *mc = &pg->route[k];
                    mc->code = rd_u8(&r); mc->a = rd_i32(&r); mc->b = rd_i32(&r); mc->c = rd_i32(&r);
                    rd_str(&r, mc->s, NAME_LEN);
                }
            }
            if (!read_cmdlist(&r, buf, &pg->list)) goto bad;
        }
    }
    if (r.err) goto bad;
    return true;
bad:
    os_log("map %d: corrupt or out of bounds", id);
    mapdata_free(m);
    return false;
}
