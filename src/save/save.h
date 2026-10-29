/* SPDX-License-Identifier: GPL-3.0-or-later
 *
 * Versioned, checksummed, two-slot save storage.
 *
 * File layout (little-endian):
 *   "OSSV"  u16 version  u16 header_len(24)  u32 sequence  u32 payload_len  u32 crc32(payload)  u32 reserved
 *   payload
 *
 * Commit protocol: write to the slot that does NOT hold the newest valid save,
 * read it back, verify the CRC, and only then report success.  The previous
 * valid slot is never touched, so an interrupted write leaves it intact.
 */
#ifndef OS_SAVE_H
#define OS_SAVE_H
#include "../common.h"
#include "../platform/platform.h"

#define SAVE_VERSION 1
#define SAVE_HEADER_LEN 24
#define SAVE_MAX_PAYLOAD (12 * 1024)
#define SAVE_SLOT_A "ONESHOT_A"
#define SAVE_SLOT_B "ONESHOT_B"

typedef enum {
    SAVE_OK = 0,
    SAVE_NONE,          /* no save on the device */
    SAVE_NO_DEVICE,     /* no writable storage (no VMU) */
    SAVE_FULL,          /* not enough free space */
    SAVE_IO_ERROR,
    SAVE_VERIFY_FAILED, /* write appeared to succeed but read-back mismatched */
    SAVE_CORRUPT,       /* data present but no slot validates */
    SAVE_UNSUPPORTED,   /* newer/unknown version: left untouched */
    SAVE_TOO_BIG,
} save_result_t;

typedef struct {
    store_result_t (*probe)(int *free_bytes);
    store_result_t (*write)(const char *name, const void *buf, size_t len);
    store_result_t (*read)(const char *name, void *buf, size_t cap, size_t *len);
    store_result_t (*del)(const char *name);
} save_backend_t;

void save_set_backend(const save_backend_t *b); /* NULL -> platform default (VMU/host file) */
save_result_t save_commit(const void *payload, size_t len, uint32_t *seq_out);
save_result_t save_load(void *payload, size_t cap, size_t *len_out, uint32_t *seq_out);
save_result_t save_erase_all(void); /* explicit "new game" request only */
const char *save_strerror(save_result_t r);

/* Exposed for tests: validate a raw slot image. */
save_result_t save_validate_image(const void *img, size_t len, uint32_t *seq, const uint8_t **payload, size_t *plen);

#endif
