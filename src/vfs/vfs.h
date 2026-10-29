/* SPDX-License-Identifier: GPL-3.0-or-later
 *
 * Sandboxed virtual filesystem for OneShot's "outside the game" mechanics.
 *
 * It never touches a real host filesystem.  Three layers, highest priority first:
 *
 *   1. SESSION overlay     RAM only; lost on power-off; survives simulated restarts
 *                          only if the transition manager says so.
 *   2. PERSISTENT overlay  RAM mirror of what is committed to the save (VMU).
 *   3. BASE                immutable files loaded from the disc (read-only).
 *
 * Writes and deletes only ever touch an overlay.  Deleting a file that exists
 * in a lower layer writes a TOMBSTONE into the chosen overlay so the lower copy
 * stays hidden.  Lookups walk SESSION -> PERSISTENT -> BASE and stop at the
 * first entry (file, directory or tombstone) for the path.
 */
#ifndef OS_VFS_H
#define OS_VFS_H
#include "../common.h"

#define VFS_MAX_PATH 96
#define VFS_MAX_NAME 32
#define VFS_MAX_DEPTH 8
#define VFS_MAX_FILE 4096           /* largest single virtual file */
#define VFS_MAX_ENTRIES 64          /* per layer */
#define VFS_SESSION_POOL (32 * 1024)
#define VFS_PERSIST_POOL (6 * 1024) /* bounded by the VMU save budget */
#define VFS_BASE_POOL (64 * 1024)

typedef enum {
    VFS_OK = 0,
    VFS_EINVAL = -1,    /* malformed path / argument */
    VFS_ENOENT = -2,
    VFS_EEXIST = -3,
    VFS_ENOSPC = -4,    /* layer pool or entry table full */
    VFS_ETOOBIG = -5,
    VFS_ENOTDIR = -6,
    VFS_EISDIR = -7,
    VFS_ENOTEMPTY = -8,
    VFS_ECORRUPT = -9,
    VFS_EROFS = -10,    /* attempt to target the immutable base layer */
} vfs_err_t;

typedef enum { VFS_LAYER_SESSION = 0, VFS_LAYER_PERSISTENT = 1, VFS_LAYER_BASE = 2 } vfs_layer_t;
typedef enum { VFS_T_TEXT = 0, VFS_T_IMAGE = 1, VFS_T_BINARY = 2 } vfs_type_t;

typedef struct {
    char name[VFS_MAX_NAME + 1];
    bool is_dir;
    vfs_type_t type;
    uint32_t size;
    uint32_t mtime;     /* logical clock, deterministic */
    vfs_layer_t layer;  /* layer the visible entry comes from */
} vfs_meta_t;

void vfs_reset(void);                         /* empty all layers */
void vfs_clear_layer(vfs_layer_t layer);      /* e.g. discard session at power-off */
int vfs_load_base(const void *buf, size_t len); /* base image built by tools/mkvfs.py */

bool vfs_path_valid(const char *path);

int vfs_create_file(const char *path, const void *data, size_t len, vfs_type_t type, vfs_layer_t layer);
int vfs_read_file(const char *path, void *buf, size_t cap, size_t *len_out);
bool vfs_file_exists(const char *path);
int vfs_delete_file(const char *path, vfs_layer_t layer);
int vfs_create_directory(const char *path, vfs_layer_t layer);
int vfs_list_directory(const char *path, vfs_meta_t *out, int max, int *count_out);
int vfs_get_file_metadata(const char *path, vfs_meta_t *out);

/* Persistent overlay (de)serialisation for the save file. */
int vfs_serialize_persistent(void *buf, size_t cap, size_t *len_out);
int vfs_deserialize_persistent(const void *buf, size_t len);
/* Move every session entry under `path` (or all if NULL) into the persistent layer. */
int vfs_promote_to_persistent(const char *path);

uint32_t vfs_used_bytes(vfs_layer_t layer);
const char *vfs_strerror(int err);

/* Observer: desktop/diagnostics get notified of changes. */
typedef void (*vfs_observer_t)(const char *op, const char *path, int result);
void vfs_set_observer(vfs_observer_t cb);

#endif
