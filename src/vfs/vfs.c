/* SPDX-License-Identifier: GPL-3.0-or-later
 * Virtual filesystem implementation - see vfs.h for the layering contract.
 * Fixed-size tables and pools; no heap allocation. */
#include "vfs.h"
#include "../util.h"
#include <ctype.h>

typedef enum { K_FILE = 0, K_DIR = 1, K_TOMB = 2 } kind_t;

typedef struct {
    bool used;
    kind_t kind;
    vfs_type_t type;
    char path[VFS_MAX_PATH + 1];
    uint32_t off, len;  /* into the layer pool */
    uint32_t mtime;
} entry_t;

typedef struct {
    entry_t e[VFS_MAX_ENTRIES];
    uint8_t *pool;
    uint32_t pool_cap;
    uint32_t pool_used;
} layer_t;

static uint8_t s_pool_session[VFS_SESSION_POOL];
static uint8_t s_pool_persist[VFS_PERSIST_POOL];
static uint8_t s_pool_base[VFS_BASE_POOL];
static layer_t L[3] = {
    {{{0}}, s_pool_session, VFS_SESSION_POOL, 0},
    {{{0}}, s_pool_persist, VFS_PERSIST_POOL, 0},
    {{{0}}, s_pool_base, VFS_BASE_POOL, 0},
};
static uint32_t s_clock = 1;
static vfs_observer_t s_obs;

void vfs_set_observer(vfs_observer_t cb) { s_obs = cb; }
static int notify(const char *op, const char *path, int r) {
    if (s_obs) s_obs(op, path, r);
    os_diag("vfs %s %s -> %s", op, path ? path : "(null)", vfs_strerror(r));
    return r;
}

const char *vfs_strerror(int err) {
    switch (err) {
    case VFS_OK: return "ok";
    case VFS_EINVAL: return "invalid path or argument";
    case VFS_ENOENT: return "not found";
    case VFS_EEXIST: return "already exists";
    case VFS_ENOSPC: return "no space";
    case VFS_ETOOBIG: return "too big";
    case VFS_ENOTDIR: return "not a directory";
    case VFS_EISDIR: return "is a directory";
    case VFS_ENOTEMPTY: return "directory not empty";
    case VFS_ECORRUPT: return "corrupt data";
    case VFS_EROFS: return "read-only layer";
    default: return "unknown error";
    }
}

static void layer_clear(layer_t *l) {
    memset(l->e, 0, sizeof(l->e));
    l->pool_used = 0;
}
void vfs_reset(void) { for (int i = 0; i < 3; i++) layer_clear(&L[i]); s_clock = 1; }
void vfs_clear_layer(vfs_layer_t layer) { if (layer <= VFS_LAYER_BASE) layer_clear(&L[layer]); }

static bool name_char_ok(char c) {
    return isalnum((unsigned char)c) || c == ' ' || c == '.' || c == '_' || c == '-' || c == '(' || c == ')';
}

/* Absolute path, '/'-separated, no empty/./.. components, bounded length & depth.
 * "/" itself is valid (the root directory). */
bool vfs_path_valid(const char *path) {
    if (!path || path[0] != '/') return false;
    size_t n = strlen(path);
    if (n > VFS_MAX_PATH) return false;
    if (n == 1) return true;
    if (path[n - 1] == '/') return false;
    int depth = 0;
    const char *p = path + 1;
    while (*p) {
        const char *s = p;
        while (*p && *p != '/') { if (!name_char_ok(*p)) return false; p++; }
        size_t len = (size_t)(p - s);
        if (len == 0 || len > VFS_MAX_NAME) return false;
        if ((len == 1 && s[0] == '.') || (len == 2 && s[0] == '.' && s[1] == '.')) return false;
        if (s[0] == ' ' || s[len - 1] == ' ') return false;
        if (++depth > VFS_MAX_DEPTH) return false;
        if (*p == '/') p++;
    }
    return true;
}

static int find(layer_t *l, const char *path) {
    for (int i = 0; i < VFS_MAX_ENTRIES; i++)
        if (l->e[i].used && os_stricmp(l->e[i].path, path) == 0) return i;
    return -1;
}

/* Visible entry for path across layers (session first). */
static entry_t *lookup(const char *path, vfs_layer_t *layer_out) {
    for (int li = 0; li < 3; li++) {
        int i = find(&L[li], path);
        if (i >= 0) {
            if (layer_out) *layer_out = (vfs_layer_t)li;
            return &L[li].e[i];
        }
    }
    return NULL;
}

static bool is_dir_visible(const char *path) {
    if (strcmp(path, "/") == 0) return true;
    entry_t *e = lookup(path, NULL);
    return e && e->kind == K_DIR;
}

static void parent_of(const char *path, char *out) {
    os_strlcpy(out, path, VFS_MAX_PATH + 1);
    char *s = strrchr(out, '/');
    if (s == out) out[1] = 0; else if (s) *s = 0;
}

/* Compact a layer's pool (called when space runs out). */
static void compact(layer_t *l) {
    static uint8_t tmp[VFS_SESSION_POOL > VFS_BASE_POOL ? VFS_SESSION_POOL : VFS_BASE_POOL];
    uint32_t used = 0;
    for (int i = 0; i < VFS_MAX_ENTRIES; i++) {
        entry_t *e = &l->e[i];
        if (!e->used || e->kind != K_FILE || !e->len) continue;
        memcpy(tmp + used, l->pool + e->off, e->len);
        e->off = used;
        used += e->len;
    }
    memcpy(l->pool, tmp, used);
    l->pool_used = used;
}

static int put(layer_t *l, const char *path, kind_t kind, vfs_type_t type, const void *data, uint32_t len) {
    int i = find(l, path);
    if (i < 0) {
        for (i = 0; i < VFS_MAX_ENTRIES && l->e[i].used; i++) {}
        if (i == VFS_MAX_ENTRIES) return VFS_ENOSPC;
    }
    if (len) {
        /* Temporarily exclude the old copy so compaction can reclaim it; restore it
         * unchanged if the new data still does not fit (a failed write must not
         * destroy the previous file contents). */
        uint32_t old_len = l->e[i].used ? l->e[i].len : 0;
        bool was_used = l->e[i].used;
        if (l->pool_used + len > l->pool_cap) {
            if (was_used) l->e[i].used = false;
            uint8_t save[VFS_MAX_FILE];
            if (was_used && old_len) memcpy(save, l->pool + l->e[i].off, old_len);
            compact(l);
            if (was_used) {
                l->e[i].used = true;
                if (l->pool_used + len > l->pool_cap && old_len) {
                    /* new data will not fit: put the old contents back */
                    memcpy(l->pool + l->pool_used, save, old_len);
                    l->e[i].off = l->pool_used;
                    l->pool_used += old_len;
                }
            }
        }
        if (l->pool_used + len > l->pool_cap) {
            if (!was_used) memset(&l->e[i], 0, sizeof(entry_t));
            return VFS_ENOSPC;
        }
    }
    entry_t *e = &l->e[i];
    e->used = true;
    e->kind = kind;
    e->type = type;
    os_strlcpy(e->path, path, sizeof(e->path));
    e->off = l->pool_used;
    e->len = len;
    if (len) { memcpy(l->pool + e->off, data, len); l->pool_used += len; }
    e->mtime = s_clock++;
    return VFS_OK;
}

static int check_parent(const char *path) {
    char par[VFS_MAX_PATH + 1];
    parent_of(path, par);
    if (!is_dir_visible(par)) {
        entry_t *pe = lookup(par, NULL);
        return (pe && pe->kind == K_FILE) ? VFS_ENOTDIR : VFS_ENOENT;
    }
    return VFS_OK;
}

int vfs_create_file(const char *path, const void *data, size_t len, vfs_type_t type, vfs_layer_t layer) {
    if (!vfs_path_valid(path) || strcmp(path, "/") == 0 || (len && !data) || type > VFS_T_BINARY)
        return notify("create", path, VFS_EINVAL);
    if (layer == VFS_LAYER_BASE) return notify("create", path, VFS_EROFS);
    if (layer > VFS_LAYER_BASE) return notify("create", path, VFS_EINVAL);
    if (len > VFS_MAX_FILE) return notify("create", path, VFS_ETOOBIG);
    int r = check_parent(path);
    if (r) return notify("create", path, r);
    entry_t *ex = lookup(path, NULL);
    if (ex && ex->kind == K_DIR) return notify("create", path, VFS_EISDIR);
    return notify("create", path, put(&L[layer], path, K_FILE, type, data, (uint32_t)len));
}

int vfs_read_file(const char *path, void *buf, size_t cap, size_t *len_out) {
    if (len_out) *len_out = 0;
    if (!vfs_path_valid(path) || (cap && !buf)) return VFS_EINVAL;
    vfs_layer_t li;
    entry_t *e = lookup(path, &li);
    if (!e || e->kind == K_TOMB) return notify("read", path, VFS_ENOENT);
    if (e->kind == K_DIR) return notify("read", path, VFS_EISDIR);
    if (e->len > cap) return notify("read", path, VFS_ETOOBIG); /* never a partial/out-of-bounds read */
    if (e->len) memcpy(buf, L[li].pool + e->off, e->len);
    if (len_out) *len_out = e->len;
    return notify("read", path, VFS_OK);
}

bool vfs_file_exists(const char *path) {
    if (!vfs_path_valid(path)) return false;
    entry_t *e = lookup(path, NULL);
    return e && e->kind == K_FILE;
}

static bool dir_has_children(const char *path) {
    vfs_meta_t tmp[1];
    int n = 0;
    vfs_list_directory(path, tmp, 1, &n);
    return n > 0;
}

int vfs_delete_file(const char *path, vfs_layer_t layer) {
    if (!vfs_path_valid(path) || strcmp(path, "/") == 0) return notify("delete", path, VFS_EINVAL);
    if (layer == VFS_LAYER_BASE) return notify("delete", path, VFS_EROFS);
    if (layer > VFS_LAYER_BASE) return notify("delete", path, VFS_EINVAL);
    vfs_layer_t vis;
    entry_t *e = lookup(path, &vis);
    if (!e || e->kind == K_TOMB) return notify("delete", path, VFS_ENOENT);
    if (e->kind == K_DIR && dir_has_children(path)) return notify("delete", path, VFS_ENOTEMPTY);
    /* Is there anything below the target layer that would reappear? */
    bool lower_exists = false;
    for (int li = (int)layer + 1; li < 3; li++) {
        int i = find(&L[li], path);
        if (i >= 0 && L[li].e[i].kind != K_TOMB) { lower_exists = true; break; }
    }
    /* Remove from target layer and every layer above it. */
    for (int li = 0; li <= (int)layer; li++) {
        int i = find(&L[li], path);
        if (i >= 0) memset(&L[li].e[i], 0, sizeof(entry_t));
    }
    int r = VFS_OK;
    if (lower_exists) r = put(&L[layer], path, K_TOMB, VFS_T_BINARY, NULL, 0);
    return notify("delete", path, r);
}

int vfs_create_directory(const char *path, vfs_layer_t layer) {
    if (!vfs_path_valid(path) || strcmp(path, "/") == 0) return notify("mkdir", path, VFS_EINVAL);
    if (layer == VFS_LAYER_BASE) return notify("mkdir", path, VFS_EROFS);
    if (layer > VFS_LAYER_BASE) return notify("mkdir", path, VFS_EINVAL);
    int r = check_parent(path);
    if (r) return notify("mkdir", path, r);
    entry_t *e = lookup(path, NULL);
    if (e && e->kind != K_TOMB) return notify("mkdir", path, VFS_EEXIST);
    return notify("mkdir", path, put(&L[layer], path, K_DIR, VFS_T_BINARY, NULL, 0));
}

static bool is_direct_child(const char *dir, const char *path, const char **name_out) {
    size_t dl = strlen(dir);
    if (strcmp(dir, "/") == 0) dl = 0;
    if (strlen(path) <= dl + 1) return false;
    if (dl && (strncasecmp_portable(path, dir, dl) != 0)) return false;
    if (path[dl] != '/') return false;
    const char *rest = path + dl + 1;
    if (strchr(rest, '/')) return false;
    *name_out = rest;
    return true;
}

int vfs_get_file_metadata(const char *path, vfs_meta_t *out) {
    if (!vfs_path_valid(path) || !out) return VFS_EINVAL;
    memset(out, 0, sizeof(*out));
    if (strcmp(path, "/") == 0) { out->is_dir = true; os_strlcpy(out->name, "/", sizeof(out->name)); return VFS_OK; }
    vfs_layer_t li;
    entry_t *e = lookup(path, &li);
    if (!e || e->kind == K_TOMB) return VFS_ENOENT;
    const char *nm = strrchr(e->path, '/');
    os_strlcpy(out->name, nm ? nm + 1 : e->path, sizeof(out->name));
    out->is_dir = e->kind == K_DIR;
    out->type = e->type;
    out->size = e->len;
    out->mtime = e->mtime;
    out->layer = li;
    return VFS_OK;
}

/* Deterministic listing: union of layers, highest layer wins, sorted by name. */
int vfs_list_directory(const char *path, vfs_meta_t *out, int max, int *count_out) {
    if (count_out) *count_out = 0;
    if (!vfs_path_valid(path) || max < 0 || (max && !out)) return VFS_EINVAL;
    if (!is_dir_visible(path)) {
        entry_t *e = lookup(path, NULL);
        return (e && e->kind == K_FILE) ? VFS_ENOTDIR : VFS_ENOENT;
    }
    int n = 0;
    for (int li = 0; li < 3; li++) {
        for (int i = 0; i < VFS_MAX_ENTRIES; i++) {
            entry_t *e = &L[li].e[i];
            const char *nm;
            if (!e->used || !is_direct_child(path, e->path, &nm)) continue;
            /* Skip if a higher layer already decided this path. */
            vfs_layer_t vis;
            entry_t *v = lookup(e->path, &vis);
            if (v != e || v->kind == K_TOMB) continue;
            if (n < max) {
                vfs_meta_t *m = &out[n];
                vfs_get_file_metadata(e->path, m);
            }
            n++;
        }
    }
    int shown = n < max ? n : max;
    for (int a = 1; a < shown; a++) /* insertion sort by name, dirs first */
        for (int b = a; b > 0; b--) {
            vfs_meta_t *x = &out[b - 1], *y = &out[b];
            bool swap = (x->is_dir == y->is_dir) ? os_stricmp(x->name, y->name) > 0 : (!x->is_dir && y->is_dir);
            if (!swap) break;
            vfs_meta_t t = *x; *x = *y; *y = t;
        }
    if (count_out) *count_out = n;
    return VFS_OK;
}

/* ---------------- base image (built offline by tools/mkvfs.py) ----------------
 * "OSVF" u16 version u16 count; per entry: u8 kind, u8 type, str path, u32 len, bytes */
static int load_entries(layer_t *l, reader_t *r, int count, bool allow_tomb) {
    for (int k = 0; k < count; k++) {
        uint8_t kind = rd_u8(r), type = rd_u8(r);
        char path[VFS_MAX_PATH + 1];
        rd_str(r, path, sizeof(path));
        uint32_t len = rd_u32(r);
        if (r->err || kind > K_TOMB || (!allow_tomb && kind == K_TOMB) || type > VFS_T_BINARY ||
            !vfs_path_valid(path) || len > VFS_MAX_FILE || (kind != K_FILE && len))
            return VFS_ECORRUPT;
        const uint8_t *data = rd_bytes(r, len);
        if (r->err) return VFS_ECORRUPT;
        int e = put(l, path, (kind_t)kind, (vfs_type_t)type, data, len);
        if (e) return e;
    }
    return VFS_OK;
}

int vfs_load_base(const void *buf, size_t len) {
    layer_clear(&L[VFS_LAYER_BASE]);
    reader_t r;
    rd_init(&r, buf, len);
    const uint8_t *m = rd_bytes(&r, 4);
    if (!m || memcmp(m, "OSVF", 4) != 0) return VFS_ECORRUPT;
    if (rd_u16(&r) != 1) return VFS_ECORRUPT;
    int n = rd_u16(&r);
    if (n > VFS_MAX_ENTRIES) return VFS_ECORRUPT;
    int e = load_entries(&L[VFS_LAYER_BASE], &r, n, false);
    if (e) layer_clear(&L[VFS_LAYER_BASE]);
    return e;
}

int vfs_serialize_persistent(void *buf, size_t cap, size_t *len_out) {
    layer_t *l = &L[VFS_LAYER_PERSISTENT];
    uint8_t *o = (uint8_t *)buf;
    size_t n = 0;
    int count = 0;
    for (int i = 0; i < VFS_MAX_ENTRIES; i++) count += l->e[i].used;
#define PUT(b) do { if (n >= cap) return VFS_ENOSPC; o[n++] = (uint8_t)(b); } while (0)
    PUT('O'); PUT('S'); PUT('V'); PUT('F'); PUT(1); PUT(0); PUT(count & 0xFF); PUT(count >> 8);
    for (int i = 0; i < VFS_MAX_ENTRIES; i++) {
        entry_t *e = &l->e[i];
        if (!e->used) continue;
        size_t pl = strlen(e->path);
        PUT(e->kind); PUT(e->type); PUT(pl);
        for (size_t k = 0; k < pl; k++) PUT(e->path[k]);
        PUT(e->len); PUT(e->len >> 8); PUT(e->len >> 16); PUT(e->len >> 24);
        if (n + e->len > cap) return VFS_ENOSPC;
        memcpy(o + n, l->pool + e->off, e->len);
        n += e->len;
    }
#undef PUT
    if (len_out) *len_out = n;
    return VFS_OK;
}

int vfs_deserialize_persistent(const void *buf, size_t len) {
    /* Validate into a scratch layer first so a corrupt save never half-applies. */
    static uint8_t scratch_pool[VFS_PERSIST_POOL];
    static layer_t scratch;
    scratch.pool = scratch_pool; scratch.pool_cap = VFS_PERSIST_POOL;
    layer_clear(&scratch);
    reader_t r;
    rd_init(&r, buf, len);
    const uint8_t *m = rd_bytes(&r, 4);
    if (!m || memcmp(m, "OSVF", 4) != 0 || rd_u16(&r) != 1) return VFS_ECORRUPT;
    int n = rd_u16(&r);
    if (n > VFS_MAX_ENTRIES) return VFS_ECORRUPT;
    int e = load_entries(&scratch, &r, n, true);
    if (e) return e;
    if (r.p != r.end) return VFS_ECORRUPT;
    layer_t *p = &L[VFS_LAYER_PERSISTENT];
    memcpy(p->e, scratch.e, sizeof(p->e));
    memcpy(p->pool, scratch.pool, scratch.pool_used);
    p->pool_used = scratch.pool_used;
    return VFS_OK;
}

int vfs_promote_to_persistent(const char *path) {
    layer_t *s = &L[VFS_LAYER_SESSION], *p = &L[VFS_LAYER_PERSISTENT];
    size_t pl = path ? strlen(path) : 0;
    for (int i = 0; i < VFS_MAX_ENTRIES; i++) {
        entry_t *e = &s->e[i];
        if (!e->used) continue;
        if (path && strncasecmp_portable(e->path, path, pl) != 0) continue;
        int r = put(p, e->path, e->kind, e->type, s->pool + e->off, e->len);
        if (r) return notify("promote", e->path, r);
        memset(e, 0, sizeof(*e));
    }
    return VFS_OK;
}

uint32_t vfs_used_bytes(vfs_layer_t layer) { return layer <= VFS_LAYER_BASE ? L[layer].pool_used : 0; }
