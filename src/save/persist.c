/* SPDX-License-Identifier: GPL-3.0-or-later
 * Save payload (TLV) for OneShot's one-slot save + persistent meta + VFS.
 *
 *   tag 1 switches bitset | tag 2 variables (i32) | tag 3 item ids | tag 4 note ids
 *   tag 5 meta (ending, player name, wallpaper, close flag, launches)
 *   tag 6 VFS persistent overlay | tag 7 flags (bit0: game snapshot present)
 *
 * A RAM copy of the last payload is always kept so a simulated restart works
 * without a VMU ("session-only" persistence). */
#include "save.h"
#include "../events/bridge.h"
#include "../game/game.h"
#include "../vfs/vfs.h"
#include "../util.h"
#include <stdio.h>

static struct {
    bool have_game;
    uint8_t sw[MAX_SWITCHES / 8];
    int32_t var[MAX_VARIABLES];
    uint16_t items[MAX_ITEMS], skills[MAX_ITEMS];
    int ni, ns;
} SNAP;

static uint8_t s_ram_payload[SAVE_MAX_PAYLOAD];
static size_t s_ram_len;
static bool s_ram_valid;
static char s_last_error[128];
static bool s_storage_ok;

const char *persist_last_error(void) { return s_last_error; }
size_t persist_last_payload_size(void) { return s_ram_valid ? s_ram_len : 0; }
/* vmu_pkg: 128-byte header + one 512-byte icon, then the OSSV header and payload */
int persist_vmu_blocks_per_copy(size_t payload) { return (int)((128 + 512 + SAVE_HEADER_LEN + payload + 511) / 512); }
bool persist_have_snapshot(void) { return SNAP.have_game; }
bool persist_storage_available(void) { return s_storage_ok; }

void persist_probe_storage(void) {
    int fb = 0;
    store_result_t r = plat_store_probe(&fb);
    s_storage_ok = r == STORE_OK;
    os_log("storage: %s (%s, %d bytes free)", s_storage_ok ? "available" : "unavailable", plat_store_describe(), fb);
}

bool persist_snapshot_game(void) {
    memset(SNAP.sw, 0, sizeof(SNAP.sw));
    for (int i = 1; i < MAX_SWITCHES; i++) if (GS.sw[i]) SNAP.sw[i / 8] |= (uint8_t)(1 << (i % 8));
    memcpy(SNAP.var, GS.var, sizeof(SNAP.var));
    SNAP.have_game = true;
    return true;
}
void persist_set_lists(const uint16_t *items, int ni, const uint16_t *skills, int ns) {
    SNAP.ni = clampi(ni, 0, MAX_ITEMS); SNAP.ns = clampi(ns, 0, MAX_ITEMS);
    memcpy(SNAP.items, items, sizeof(uint16_t) * (size_t)SNAP.ni);
    memcpy(SNAP.skills, skills, sizeof(uint16_t) * (size_t)SNAP.ns);
}
void persist_get_lists(uint16_t *items, int *ni, uint16_t *skills, int *ns) {
    memcpy(items, SNAP.items, sizeof(uint16_t) * (size_t)SNAP.ni); *ni = SNAP.ni;
    memcpy(skills, SNAP.skills, sizeof(uint16_t) * (size_t)SNAP.ns); *ns = SNAP.ns;
}

/* ---- TLV writer/reader ---- */
typedef struct { uint8_t *b; size_t n, cap; bool err; } wbuf_t;
static void wput(wbuf_t *w, const void *d, size_t n) {
    if (w->err || w->n + n > w->cap) { w->err = true; return; }
    memcpy(w->b + w->n, d, n); w->n += n;
}
static void wu16(wbuf_t *w, uint16_t v) { uint8_t b[2] = {(uint8_t)v, (uint8_t)(v >> 8)}; wput(w, b, 2); }
static void wu32(wbuf_t *w, uint32_t v) { uint8_t b[4] = {(uint8_t)v, (uint8_t)(v >> 8), (uint8_t)(v >> 16), (uint8_t)(v >> 24)}; wput(w, b, 4); }
static void tag(wbuf_t *w, uint16_t t, const void *d, uint32_t len) { wu16(w, t); wu32(w, len); wput(w, d, len); }

static size_t build_payload(uint8_t *buf, size_t cap) {
    wbuf_t w = {buf, 0, cap, false};
    wput(&w, "OSPL", 4); wu16(&w, 1);
    if (SNAP.have_game) {
        tag(&w, 1, SNAP.sw, sizeof(SNAP.sw));
        uint8_t tmp[MAX_VARIABLES * 4];
        for (int i = 0; i < MAX_VARIABLES; i++) { uint32_t v = (uint32_t)SNAP.var[i]; tmp[i * 4] = v; tmp[i * 4 + 1] = v >> 8; tmp[i * 4 + 2] = v >> 16; tmp[i * 4 + 3] = v >> 24; }
        tag(&w, 2, tmp, sizeof(tmp));
        uint8_t l[MAX_ITEMS * 2];
        for (int i = 0; i < SNAP.ni; i++) { l[i * 2] = (uint8_t)SNAP.items[i]; l[i * 2 + 1] = SNAP.items[i] >> 8; }
        tag(&w, 3, l, (uint32_t)SNAP.ni * 2);
        for (int i = 0; i < SNAP.ns; i++) { l[i * 2] = (uint8_t)SNAP.skills[i]; l[i * 2 + 1] = SNAP.skills[i] >> 8; }
        tag(&w, 4, l, (uint32_t)SNAP.ns * 2);
    }
    uint8_t meta[4 + 1 + NAME_LEN + 1 + 1 + 32 + 1 + 4];
    size_t m = 0;
    uint32_t e = (uint32_t)BM.ending;
    meta[m++] = e; meta[m++] = e >> 8; meta[m++] = e >> 16; meta[m++] = e >> 24;
    size_t nl = strlen(BM.player_name); meta[m++] = (uint8_t)nl; memcpy(meta + m, BM.player_name, nl); m += nl;
    meta[m++] = BM.name_known;
    size_t wl = strlen(BM.wallpaper); meta[m++] = (uint8_t)wl; memcpy(meta + m, BM.wallpaper, wl); m += wl;
    meta[m++] = BM.close_enabled;
    meta[m++] = BM.launches; meta[m++] = BM.launches >> 8; meta[m++] = BM.launches >> 16; meta[m++] = BM.launches >> 24;
    tag(&w, 5, meta, (uint32_t)m);
    static uint8_t vb[VFS_PERSIST_POOL + 64 * (VFS_MAX_PATH + 8) + 16];
    size_t vl = 0;
    if (vfs_serialize_persistent(vb, sizeof(vb), &vl) == VFS_OK) tag(&w, 6, vb, (uint32_t)vl);
    else w.err = true;
    uint8_t flags = SNAP.have_game ? 1 : 0;
    tag(&w, 7, &flags, 1);
    return w.err ? 0 : w.n;
}

static int apply_payload(const uint8_t *p, size_t len, bool apply_game) {
    reader_t r;
    rd_init(&r, p, len);
    const uint8_t *mg = rd_bytes(&r, 4);
    if (!mg || memcmp(mg, "OSPL", 4) || rd_u16(&r) != 1) return -1;
    /* validate everything first */
    bool have_game = false, name_known = false, close_en = true;
    int32_t ending = 0;
    uint32_t launches = 0;
    char name[NAME_LEN] = "", wall[32] = "default";
    const uint8_t *sw = NULL, *var = NULL, *it = NULL, *sk = NULL, *vfsb = NULL;
    uint32_t itl = 0, skl = 0, vfsl = 0;
    while (r.p < r.end) {
        uint16_t t = rd_u16(&r);
        uint32_t l = rd_u32(&r);
        const uint8_t *d = rd_bytes(&r, l);
        if (r.err) return -1;
        switch (t) {
        case 1: if (l != MAX_SWITCHES / 8) return -1; sw = d; break;
        case 2: if (l != MAX_VARIABLES * 4) return -1; var = d; break;
        case 3: if (l % 2 || l / 2 > MAX_ITEMS) return -1; it = d; itl = l; break;
        case 4: if (l % 2 || l / 2 > MAX_ITEMS) return -1; sk = d; skl = l; break;
        case 5: {
            reader_t mr; rd_init(&mr, d, l);
            ending = rd_i32(&mr); rd_str(&mr, name, sizeof(name)); name_known = rd_u8(&mr);
            rd_str(&mr, wall, sizeof(wall)); close_en = rd_u8(&mr); launches = rd_u32(&mr);
            if (mr.err) return -1;
            break;
        }
        case 6: vfsb = d; vfsl = l; break;
        case 7: if (l != 1) return -1; have_game = (d[0] & 1) != 0; break;
        default: break; /* unknown tags from newer minor versions are ignored */
        }
    }
    if (have_game && (!sw || !var || !it || !sk)) return -1;
    if (vfsb && vfs_deserialize_persistent(vfsb, vfsl) != VFS_OK) return -1;
    /* commit to memory */
    BM.ending = ending; os_strlcpy(BM.player_name, name, sizeof(BM.player_name)); BM.name_known = name_known;
    os_strlcpy(BM.wallpaper, wall, sizeof(BM.wallpaper)); BM.close_enabled = close_en; BM.launches = launches;
    SNAP.have_game = have_game;
    if (have_game) {
        memcpy(SNAP.sw, sw, sizeof(SNAP.sw));
        for (int i = 0; i < MAX_VARIABLES; i++) SNAP.var[i] = (int32_t)(var[i * 4] | (var[i * 4 + 1] << 8) | (var[i * 4 + 2] << 16) | ((uint32_t)var[i * 4 + 3] << 24));
        SNAP.ni = (int)itl / 2; SNAP.ns = (int)skl / 2;
        for (int i = 0; i < SNAP.ni; i++) SNAP.items[i] = (uint16_t)(it[i * 2] | (it[i * 2 + 1] << 8));
        for (int i = 0; i < SNAP.ns; i++) SNAP.skills[i] = (uint16_t)(sk[i * 2] | (sk[i * 2 + 1] << 8));
        if (apply_game) {
            for (int i = 1; i < MAX_SWITCHES; i++) GS.sw[i] = (SNAP.sw[i / 8] >> (i % 8)) & 1;
            memcpy(GS.var, SNAP.var, sizeof(GS.var));
            GS.need_refresh = true;
        }
    }
    return have_game ? 1 : 0;
}

persist_status_t persist_commit(char *why, size_t cap) {
    static uint8_t buf[SAVE_MAX_PAYLOAD];
    size_t n = build_payload(buf, sizeof(buf));
    if (!n) { snprintf(why, cap, "save data too large"); os_strlcpy(s_last_error, why, sizeof(s_last_error)); return PERSIST_FAILED; }
    memcpy(s_ram_payload, buf, n); s_ram_len = n; s_ram_valid = true;   /* survives simulated restarts */
    uint32_t seq = 0;
    save_result_t r = save_commit(buf, n, &seq);
    if (r == SAVE_OK) { s_storage_ok = true; why[0] = 0; return PERSIST_OK; }
    snprintf(why, cap, "%s", save_strerror(r));
    os_strlcpy(s_last_error, why, sizeof(s_last_error));
    s_storage_ok = false;
    os_log("persist: commit failed: %s (kept in RAM for this session)", why);
    return PERSIST_SESSION_ONLY;
}

/* Called by func_Load.  0 none, 1 loaded, 2 loaded but name unknown, -1 error */
int persist_restore(void) {
    static uint8_t buf[SAVE_MAX_PAYLOAD];
    size_t n = 0;
    save_result_t r = save_load(buf, sizeof(buf), &n, NULL);
    int res;
    if (r == SAVE_OK) res = apply_payload(buf, n, true);
    else if (s_ram_valid) res = apply_payload(s_ram_payload, s_ram_len, true);
    else if (r == SAVE_NONE || r == SAVE_NO_DEVICE) return 0;
    else { snprintf(s_last_error, sizeof(s_last_error), "%s", save_strerror(r)); return -1; }
    if (res < 0) { snprintf(s_last_error, sizeof(s_last_error), "save data is damaged"); return -1; }
    if (res == 1 && !BM.name_known) return 2;
    if (res == 1 && BM.name_known) os_strlcpy(GS.actor[2].name, BM.player_name, NAME_LEN);
    return res;
}

/* At power-on: load meta + VFS persistent layer (not the game snapshot into GS). */
int persist_boot_meta(void) {
    static uint8_t buf[SAVE_MAX_PAYLOAD];
    size_t n = 0;
    save_result_t r = save_load(buf, sizeof(buf), &n, NULL);
    if (r != SAVE_OK) return (int)r;
    if (apply_payload(buf, n, false) < 0) return (int)SAVE_CORRUPT;
    memcpy(s_ram_payload, buf, n); s_ram_len = n; s_ram_valid = true;
    return 0;
}

void persist_reset_ram(void) { s_ram_valid = false; memset(&SNAP, 0, sizeof(SNAP)); }
