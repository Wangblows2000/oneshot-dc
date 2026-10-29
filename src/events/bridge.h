/* SPDX-License-Identifier: GPL-3.0-or-later
 *
 * OneShot (2014) host-operation compatibility bridge.
 *
 * VERIFIED from the game data (RPG_RT.ldb, common events 1-14):
 *   An event sets variable 1 ("func") to a function number, puts arguments in
 *   variables 2-5 ("arg1".."arg4"), then executes ChangeSystemSFX(cursor) with
 *   the dummy sound "_func".  The modified RPG_RT.exe intercepts that and writes
 *   a result into variable 6 ("return").  Map 1 uses "_init" the same way as a
 *   start-up handshake (var1 is pre-set to 8008135; if it is unchanged the game
 *   shows the "error" picture).
 *
 * The original native implementations live inside an obfuscated executable and
 * were NOT reverse engineered.  Every replacement below is a documented
 * Dreamcast equivalent derived from how the events use the results.
 * See docs/COMPATIBILITY.md for the full registry.
 */
#ifndef OS_BRIDGE_H
#define OS_BRIDGE_H
#include "../common.h"

enum {
    FN_GUESS_NAME = 0, FN_SET_NAME = 1, FN_SET_NAME_ENTRY = 2, FN_SHAKE_WINDOW = 3, FN_SET_WALLPAPER = 4,
    FN_MESSAGE_BOX = 5, FN_LEAVE_WINDOW = 6, FN_SAVE = 7, FN_WRITE_ITEM = 8, FN_LOAD = 9, FN_READ_ITEM = 10,
    FN_DOCUMENT = 11, FN_END = 12, FN_SET_CLOSE_ENABLED = 13, FN_COUNT = 14
};

#define VAR_FUNC 1
#define VAR_ARG1 2
#define VAR_RETURN 6
#define VAR_ENDING 7
#define INIT_SENTINEL 8008135

typedef struct {
    int id;
    const char *name;              /* common event name in RPG_RT.ldb */
    const char *original_purpose;  /* inferred from event usage */
    const char *dreamcast;         /* replacement behaviour */
    const char *failure;           /* failure behaviour */
    const char *status;            /* implemented / partial / unresolved */
} bridge_entry_t;

extern const bridge_entry_t BRIDGE_REGISTRY[FN_COUNT];

void bridge_init(void);
void bridge_new_session(void);
/* Called by the interpreter on ChangeSystemSFX "_func"/"_init".  Returns true
 * when finished; false while an asynchronous UI step (dialog, document notice,
 * name entry, save) is still running - the interpreter waits and polls. */
bool bridge_invoke(bool is_init);
bool bridge_poll(void);
void bridge_tick(void);           /* once per frame: completes a pending save stream */
const char *bridge_player_name(void);
void bridge_set_sandbox(bool on);  /* debug cutscene playback: suppress every persistent effect */
bool bridge_sandboxed(void);
void bridge_set_player_name(const char *n);
bool bridge_close_enabled(void);
int bridge_last_func(void);
int bridge_last_result(void);

/* Persistent meta state (survives simulated restarts / power cycles when saved). */
typedef struct {
    int32_t ending;          /* original var 7 "ENDING" */
    char player_name[NAME_LEN];
    bool name_known;
    char wallpaper[32];
    bool close_enabled;
    uint32_t launches;       /* number of (simulated) game launches */
} bridge_meta_t;
extern bridge_meta_t BM;

/* Save payload helpers (persist.c) */
typedef enum { PERSIST_OK = 0, PERSIST_SESSION_ONLY, PERSIST_FAILED } persist_status_t;
persist_status_t persist_commit(char *why, size_t cap);
int persist_restore(void);       /* 0 none, 1 loaded, 2 loaded but name unknown, <0 error */
bool persist_have_snapshot(void);
const char *persist_last_error(void);
bool persist_storage_available(void);
size_t persist_last_payload_size(void);          /* bytes of the last committed payload */
int persist_vmu_blocks_per_copy(size_t payload); /* VMU blocks one save copy (slot) occupies */
int bridge_commit_count(void);                   /* commits attempted so far (game saves, endings) */
persist_status_t bridge_last_commit(const char **why);
void persist_probe_storage(void);

#endif
