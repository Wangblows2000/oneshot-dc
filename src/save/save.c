/* SPDX-License-Identifier: GPL-3.0-or-later */
#include "save.h"
#include "../util.h"

static save_backend_t s_default = {plat_store_probe, plat_store_write, plat_store_read, plat_store_delete};
static const save_backend_t *B = &s_default;

void save_set_backend(const save_backend_t *b) { B = b ? b : &s_default; }

const char *save_strerror(save_result_t r) {
    switch (r) {
    case SAVE_OK: return "saved";
    case SAVE_NONE: return "no save data";
    case SAVE_NO_DEVICE: return "no memory card (VMU) found";
    case SAVE_FULL: return "not enough free blocks on the memory card";
    case SAVE_IO_ERROR: return "memory card read/write error";
    case SAVE_VERIFY_FAILED: return "memory card write could not be verified";
    case SAVE_CORRUPT: return "save data is damaged";
    case SAVE_UNSUPPORTED: return "save data is from an unsupported version";
    case SAVE_TOO_BIG: return "save data too large";
    }
    return "unknown";
}

static void put32(uint8_t *p, uint32_t v) { p[0] = v; p[1] = v >> 8; p[2] = v >> 16; p[3] = v >> 24; }
static uint32_t get32(const uint8_t *p) { return p[0] | (p[1] << 8) | ((uint32_t)p[2] << 16) | ((uint32_t)p[3] << 24); }

save_result_t save_validate_image(const void *img, size_t len, uint32_t *seq, const uint8_t **payload, size_t *plen) {
    const uint8_t *b = (const uint8_t *)img;
    if (len < SAVE_HEADER_LEN || memcmp(b, "OSSV", 4) != 0) return SAVE_CORRUPT;
    uint16_t ver = (uint16_t)(b[4] | (b[5] << 8));
    uint16_t hl = (uint16_t)(b[6] | (b[7] << 8));
    if (ver > SAVE_VERSION) return SAVE_UNSUPPORTED;
    if (ver < 1 || hl != SAVE_HEADER_LEN) return SAVE_CORRUPT;
    uint32_t s = get32(b + 8), pl = get32(b + 12), crc = get32(b + 16);
    if (pl > SAVE_MAX_PAYLOAD || (size_t)SAVE_HEADER_LEN + pl > len) return SAVE_CORRUPT;
    if (os_crc32(b + SAVE_HEADER_LEN, pl) != crc) return SAVE_CORRUPT;
    if (seq) *seq = s;
    if (payload) *payload = b + SAVE_HEADER_LEN;
    if (plen) *plen = pl;
    return SAVE_OK;
}

static uint8_t s_img[SAVE_HEADER_LEN + SAVE_MAX_PAYLOAD];
static uint8_t s_chk[SAVE_HEADER_LEN + SAVE_MAX_PAYLOAD];

typedef struct { save_result_t st; uint32_t seq; store_result_t io; } slot_info_t;

static slot_info_t probe_slot(const char *name) {
    slot_info_t si = {SAVE_NONE, 0, STORE_OK};
    size_t n = 0;
    store_result_t r = B->read(name, s_chk, sizeof(s_chk), &n);
    si.io = r;
    if (r == STORE_NOT_FOUND) { si.st = SAVE_NONE; return si; }
    if (r == STORE_NO_DEVICE) { si.st = SAVE_NO_DEVICE; return si; }
    if (r != STORE_OK) { si.st = SAVE_CORRUPT; return si; }
    si.st = save_validate_image(s_chk, n, &si.seq, NULL, NULL);
    return si;
}

save_result_t save_commit(const void *payload, size_t len, uint32_t *seq_out) {
    if (len > SAVE_MAX_PAYLOAD) return SAVE_TOO_BIG;
    int free_bytes = 0;
    store_result_t pr = B->probe(&free_bytes);
    if (pr == STORE_NO_DEVICE) return SAVE_NO_DEVICE;
    if (pr != STORE_OK) return SAVE_IO_ERROR;

    slot_info_t a = probe_slot(SAVE_SLOT_A), b = probe_slot(SAVE_SLOT_B);
    if (a.st == SAVE_NO_DEVICE || b.st == SAVE_NO_DEVICE) return SAVE_NO_DEVICE;
    if (a.st == SAVE_UNSUPPORTED || b.st == SAVE_UNSUPPORTED) return SAVE_UNSUPPORTED; /* never overwrite */

    /* Pick target: an invalid/empty slot first, else the older valid one. */
    int ti;
    uint32_t newest = 0;
    bool av = a.st == SAVE_OK, bv = b.st == SAVE_OK;
    if (av) newest = a.seq;
    if (bv && b.seq > newest) newest = b.seq;
    if (!av) ti = 0;
    else if (!bv) ti = 1;
    else ti = (a.seq <= b.seq) ? 0 : 1;
    const char *target = ti == 0 ? SAVE_SLOT_A : SAVE_SLOT_B;
    bool target_exists = (ti == 0) ? a.st != SAVE_NONE : b.st != SAVE_NONE;

    size_t total = SAVE_HEADER_LEN + len;
    if (!target_exists && (size_t)free_bytes < total) return SAVE_FULL;

    uint32_t seq = newest + 1;
    memcpy(s_img, "OSSV", 4);
    s_img[4] = SAVE_VERSION & 0xFF; s_img[5] = SAVE_VERSION >> 8;
    s_img[6] = SAVE_HEADER_LEN; s_img[7] = 0;
    put32(s_img + 8, seq);
    put32(s_img + 12, (uint32_t)len);
    put32(s_img + 16, os_crc32(payload, len));
    put32(s_img + 20, 0);
    memcpy(s_img + SAVE_HEADER_LEN, payload, len);

    store_result_t wr = B->write(target, s_img, total);
    if (wr == STORE_NO_DEVICE) return SAVE_NO_DEVICE;
    if (wr == STORE_FULL) return SAVE_FULL;
    if (wr != STORE_OK) return SAVE_IO_ERROR;

    /* Read back and verify before claiming success. */
    size_t n = 0;
    if (B->read(target, s_chk, sizeof(s_chk), &n) != STORE_OK) return SAVE_VERIFY_FAILED;
    uint32_t vs = 0;
    if (save_validate_image(s_chk, n, &vs, NULL, NULL) != SAVE_OK || vs != seq) return SAVE_VERIFY_FAILED;
    if (seq_out) *seq_out = seq;
    os_diag("save committed to %s seq=%u len=%u", target, (unsigned)seq, (unsigned)len);
    return SAVE_OK;
}

save_result_t save_load(void *payload, size_t cap, size_t *len_out, uint32_t *seq_out) {
    if (len_out) *len_out = 0;
    int fb;
    store_result_t pr = B->probe(&fb);
    if (pr == STORE_NO_DEVICE) return SAVE_NO_DEVICE;
    if (pr != STORE_OK) return SAVE_IO_ERROR;
    const char *names[2] = {SAVE_SLOT_A, SAVE_SLOT_B};
    uint32_t best_seq = 0;
    int best = -1;
    bool any_present = false, any_unsupported = false;
    for (int i = 0; i < 2; i++) {
        slot_info_t si = probe_slot(names[i]);
        if (si.st != SAVE_NONE) any_present = true;
        if (si.st == SAVE_UNSUPPORTED) any_unsupported = true;
        if (si.st == SAVE_OK && (best < 0 || si.seq > best_seq)) { best = i; best_seq = si.seq; }
    }
    if (best < 0) return !any_present ? SAVE_NONE : any_unsupported ? SAVE_UNSUPPORTED : SAVE_CORRUPT;
    size_t n = 0;
    if (B->read(names[best], s_img, sizeof(s_img), &n) != STORE_OK) return SAVE_IO_ERROR;
    const uint8_t *p;
    size_t pl;
    uint32_t s;
    if (save_validate_image(s_img, n, &s, &p, &pl) != SAVE_OK) return SAVE_CORRUPT;
    if (pl > cap) return SAVE_TOO_BIG;
    memcpy(payload, p, pl);
    if (len_out) *len_out = pl;
    if (seq_out) *seq_out = s;
    return SAVE_OK;
}

save_result_t save_erase_all(void) {
    store_result_t a = B->del(SAVE_SLOT_A), b = B->del(SAVE_SLOT_B);
    if (a == STORE_NO_DEVICE || b == STORE_NO_DEVICE) return SAVE_NO_DEVICE;
    if ((a != STORE_OK && a != STORE_NOT_FOUND) || (b != STORE_OK && b != STORE_NOT_FOUND)) return SAVE_IO_ERROR;
    return SAVE_OK;
}
