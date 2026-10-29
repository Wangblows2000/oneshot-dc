/* SPDX-License-Identifier: GPL-3.0-or-later
 * Host unit tests: VFS, save storage, persistence, bridge semantics.
 * Destructive/interrupted storage cases use a synthetic RAM backend. */
#include "../src/common.h"
#include "../src/vfs/vfs.h"
#include "../src/save/save.h"
#include "../src/events/bridge.h"
#include "../src/game/game.h"
#include "../src/main.h"
#include "../src/platform/host.h"
#include "../src/desktop/desktop.h"
#include "../src/util.h"
#include <stdio.h>
#include <stdlib.h>

static int s_pass, s_fail;
#define CHECK(cond, name) do { if (cond) { s_pass++; } else { s_fail++; printf("FAIL: %s (%s:%d)\n", name, __FILE__, __LINE__); } } while (0)

/* ------------------------------------------------------------ RAM storage backend with fault injection */
typedef enum { F_NONE, F_NO_DEVICE, F_FULL, F_TORN, F_WRITE_ERROR, F_READ_GARBAGE } fault_t;
static struct { char name[16]; uint8_t data[SAVE_HEADER_LEN + SAVE_MAX_PAYLOAD]; size_t len; bool used; } RS[4];
static fault_t s_fault;
static int s_free = 64 * 1024;
static int s_writes;

static store_result_t r_probe(int *fb) { if (s_fault == F_NO_DEVICE) return STORE_NO_DEVICE; *fb = s_fault == F_FULL ? 0 : s_free; return STORE_OK; }
static int r_find(const char *n) { for (int i = 0; i < 4; i++) if (RS[i].used && !strcmp(RS[i].name, n)) return i; return -1; }
static store_result_t r_write(const char *n, const void *b, size_t l) {
    if (s_fault == F_NO_DEVICE) return STORE_NO_DEVICE;
    if (s_fault == F_FULL) return STORE_FULL;
    if (s_fault == F_WRITE_ERROR) return STORE_IO_ERROR;
    int i = r_find(n);
    if (i < 0) for (i = 0; i < 4 && RS[i].used; i++) {}
    if (i >= 4) return STORE_FULL;
    RS[i].used = true; os_strlcpy(RS[i].name, n, 16);
    size_t w = s_fault == F_TORN ? l / 3 : l;
    memcpy(RS[i].data, b, w); RS[i].len = w;
    s_writes++;
    return STORE_OK;
}
static store_result_t r_read(const char *n, void *b, size_t cap, size_t *l) {
    if (s_fault == F_NO_DEVICE) return STORE_NO_DEVICE;
    int i = r_find(n);
    if (i < 0) return STORE_NOT_FOUND;
    size_t k = RS[i].len < cap ? RS[i].len : cap;
    memcpy(b, RS[i].data, k);
    if (s_fault == F_READ_GARBAGE && k > 30) ((uint8_t *)b)[30] ^= 0x55;
    *l = k;
    return STORE_OK;
}
static store_result_t r_del(const char *n) {
    if (s_fault == F_NO_DEVICE) return STORE_NO_DEVICE;
    int i = r_find(n); if (i < 0) return STORE_NOT_FOUND; RS[i].used = false; return STORE_OK;
}
static const save_backend_t RAMB = {r_probe, r_write, r_read, r_del};
static void ram_reset(void) { memset(RS, 0, sizeof(RS)); s_fault = F_NONE; s_free = 64 * 1024; s_writes = 0; }

/* ------------------------------------------------------------ VFS */
static void build_base(void) {
    /* synthetic base image: /Documents, /Documents/base.txt */
    uint8_t b[512]; size_t n = 0;
#define PUT(x) b[n++] = (uint8_t)(x)
    memcpy(b, "OSVF", 4); n = 4; PUT(1); PUT(0); PUT(3); PUT(0);
    const char *d = "/Documents"; PUT(1); PUT(2); PUT(strlen(d)); memcpy(b + n, d, strlen(d)); n += strlen(d); PUT(0); PUT(0); PUT(0); PUT(0);
    const char *p = "/Documents/base.txt"; const char *t = "original";
    PUT(0); PUT(0); PUT(strlen(p)); memcpy(b + n, p, strlen(p)); n += strlen(p); PUT(strlen(t)); PUT(0); PUT(0); PUT(0); memcpy(b + n, t, strlen(t)); n += strlen(t);
    const char *e = "/Empty"; PUT(1); PUT(2); PUT(strlen(e)); memcpy(b + n, e, strlen(e)); n += strlen(e); PUT(0); PUT(0); PUT(0); PUT(0);
#undef PUT
    vfs_reset();
    CHECK(vfs_load_base(b, n) == VFS_OK, "base image loads");
}

static void test_vfs(void) {
    build_base();
    char buf[64]; size_t n;
    /* path validation */
    CHECK(!vfs_path_valid("Documents/x"), "relative path rejected");
    CHECK(!vfs_path_valid("/Documents/../x"), "traversal rejected");
    CHECK(!vfs_path_valid("/a//b"), "empty component rejected");
    CHECK(!vfs_path_valid("/a/"), "trailing slash rejected");
    CHECK(!vfs_path_valid("/a\\b"), "backslash rejected");
    CHECK(!vfs_path_valid("/a/b/c/d/e/f/g/h/i"), "depth limit");
    CHECK(vfs_create_file("/../etc/passwd", "x", 1, VFS_T_TEXT, VFS_LAYER_SESSION) == VFS_EINVAL, "create traversal fails");
    /* base layer is immutable */
    CHECK(vfs_create_file("/Documents/new.txt", "x", 1, VFS_T_TEXT, VFS_LAYER_BASE) == VFS_EROFS, "write to base refused");
    CHECK(vfs_delete_file("/Documents/base.txt", VFS_LAYER_BASE) == VFS_EROFS, "delete from base refused");
    /* create in session */
    CHECK(vfs_create_file("/Documents/clue.txt", "123456", 6, VFS_T_TEXT, VFS_LAYER_SESSION) == VFS_OK, "create clue");
    CHECK(vfs_file_exists("/Documents/clue.txt"), "clue visible");
    CHECK(vfs_file_exists("/documents/CLUE.TXT"), "case-insensitive lookup");
    CHECK(vfs_read_file("/Documents/clue.txt", buf, sizeof(buf), &n) == VFS_OK && n == 6 && !memcmp(buf, "123456", 6), "read clue");
    CHECK(vfs_read_file("/Documents/clue.txt", buf, 3, &n) == VFS_ETOOBIG && n == 0, "short buffer -> no partial read");
    CHECK(vfs_create_file("/Nope/x.txt", "x", 1, VFS_T_TEXT, VFS_LAYER_SESSION) == VFS_ENOENT, "missing parent");
    CHECK(vfs_create_file("/Documents/clue.txt/x", "x", 1, VFS_T_TEXT, VFS_LAYER_SESSION) == VFS_ENOTDIR, "file as parent");
    /* override base file without touching base */
    CHECK(vfs_create_file("/Documents/base.txt", "changed", 7, VFS_T_TEXT, VFS_LAYER_PERSISTENT) == VFS_OK, "override base");
    vfs_read_file("/Documents/base.txt", buf, sizeof(buf), &n); buf[n] = 0;
    CHECK(!strcmp(buf, "changed"), "override visible");
    vfs_meta_t m; vfs_get_file_metadata("/Documents/base.txt", &m);
    CHECK(m.layer == VFS_LAYER_PERSISTENT, "override comes from persistent layer");
    /* delete -> tombstone, base copy never reappears */
    CHECK(vfs_delete_file("/Documents/base.txt", VFS_LAYER_PERSISTENT) == VFS_OK, "delete overridden base file");
    CHECK(!vfs_file_exists("/Documents/base.txt"), "deleted file stays deleted");
    CHECK(vfs_read_file("/Documents/base.txt", buf, sizeof(buf), &n) == VFS_ENOENT, "read deleted -> ENOENT");
    CHECK(vfs_delete_file("/Documents/base.txt", VFS_LAYER_PERSISTENT) == VFS_ENOENT, "double delete fails");
    /* listing: deterministic, sorted, tombstones hidden */
    vfs_meta_t list[8]; int cnt = 0;
    CHECK(vfs_list_directory("/Documents", list, 8, &cnt) == VFS_OK && cnt == 1 && !strcmp(list[0].name, "clue.txt"), "listing hides tombstone");
    vfs_create_file("/Documents/a.txt", "a", 1, VFS_T_TEXT, VFS_LAYER_SESSION);
    vfs_list_directory("/Documents", list, 8, &cnt);
    CHECK(cnt == 2 && !strcmp(list[0].name, "a.txt") && !strcmp(list[1].name, "clue.txt"), "listing sorted");
    CHECK(vfs_list_directory("/Documents/a.txt", list, 8, &cnt) == VFS_ENOTDIR, "list a file -> ENOTDIR");
    CHECK(vfs_delete_file("/Documents", VFS_LAYER_SESSION) == VFS_ENOTEMPTY, "non-empty dir delete refused");
    CHECK(vfs_create_directory("/Documents", VFS_LAYER_SESSION) == VFS_EEXIST, "mkdir existing");
    CHECK(vfs_create_directory("/Documents/Sub", VFS_LAYER_SESSION) == VFS_OK, "mkdir");
    /* size limits */
    static char big[VFS_MAX_FILE + 1];
    memset(big, 'x', sizeof(big));
    CHECK(vfs_create_file("/Documents/big.txt", big, sizeof(big), VFS_T_TEXT, VFS_LAYER_SESSION) == VFS_ETOOBIG, "oversized file refused");
    /* persistent layer exhaustion: failure must not destroy previous content */
    vfs_create_file("/Documents/keep.txt", "keep", 4, VFS_T_TEXT, VFS_LAYER_PERSISTENT);
    int r = VFS_OK, k = 0;
    char nm[48];
    while (r == VFS_OK && k < 200) { snprintf(nm, sizeof(nm), "/Documents/f%03d.bin", k++); r = vfs_create_file(nm, big, VFS_MAX_FILE, VFS_T_BINARY, VFS_LAYER_PERSISTENT); }
    CHECK(r == VFS_ENOSPC, "persistent pool reports ENOSPC");
    CHECK(!vfs_file_exists(nm), "failed create leaves no file behind");
    vfs_read_file("/Documents/keep.txt", buf, sizeof(buf), &n);
    CHECK(n == 4 && !memcmp(buf, "keep", 4), "earlier file intact after ENOSPC");
    CHECK(vfs_create_file("/Documents/keep.txt", big, VFS_MAX_FILE, VFS_T_TEXT, VFS_LAYER_PERSISTENT) == VFS_ENOSPC, "overwrite that cannot fit fails");
    vfs_read_file("/Documents/keep.txt", buf, sizeof(buf), &n);
    CHECK(n == 4 && !memcmp(buf, "keep", 4), "failed overwrite keeps old contents");
    /* serialise persistent layer and restore */
    build_base();
    vfs_create_file("/Documents/p.txt", "persist", 7, VFS_T_TEXT, VFS_LAYER_PERSISTENT);
    vfs_delete_file("/Documents/base.txt", VFS_LAYER_PERSISTENT);
    vfs_create_file("/Documents/s.txt", "session", 7, VFS_T_TEXT, VFS_LAYER_SESSION);
    static uint8_t ser[16384]; size_t sl = 0;
    CHECK(vfs_serialize_persistent(ser, sizeof(ser), &sl) == VFS_OK, "serialise persistent");
    build_base(); /* simulate power cycle: session + persistent gone, base reloaded */
    CHECK(vfs_file_exists("/Documents/base.txt") && !vfs_file_exists("/Documents/p.txt"), "fresh boot state");
    CHECK(vfs_deserialize_persistent(ser, sl) == VFS_OK, "restore persistent");
    CHECK(vfs_file_exists("/Documents/p.txt"), "persistent file survives power cycle");
    CHECK(!vfs_file_exists("/Documents/base.txt"), "tombstone survives power cycle");
    CHECK(!vfs_file_exists("/Documents/s.txt"), "session file does not survive power cycle");
    /* corrupt serialised data never half-applies */
    ser[10] ^= 0xFF;
    CHECK(vfs_deserialize_persistent(ser, sl) != VFS_OK, "corrupt persistent blob rejected");
    CHECK(vfs_file_exists("/Documents/p.txt"), "rejected blob left current state intact");
    CHECK(vfs_deserialize_persistent(ser, 5) == VFS_ECORRUPT, "truncated blob rejected");
}

/* ------------------------------------------------------------ save storage */
static void test_save(void) {
    save_set_backend(&RAMB);
    ram_reset();
    uint8_t p[64], out[SAVE_MAX_PAYLOAD]; size_t n; uint32_t seq;
    for (int i = 0; i < 64; i++) p[i] = (uint8_t)i;
    CHECK(save_load(out, sizeof(out), &n, &seq) == SAVE_NONE, "empty card -> SAVE_NONE");
    CHECK(save_commit(p, 64, &seq) == SAVE_OK && seq == 1, "first commit");
    p[0] = 99;
    CHECK(save_commit(p, 64, &seq) == SAVE_OK && seq == 2, "second commit goes to other slot");
    CHECK(save_load(out, sizeof(out), &n, &seq) == SAVE_OK && seq == 2 && out[0] == 99, "loads newest");
    /* torn write: device says OK but data is truncated -> must NOT report success, old save intact */
    s_fault = F_TORN; p[0] = 7;
    CHECK(save_commit(p, 64, &seq) == SAVE_VERIFY_FAILED, "torn write detected (no false success)");
    s_fault = F_NONE;
    CHECK(save_load(out, sizeof(out), &n, &seq) == SAVE_OK && out[0] == 99, "last good save survives torn write");
    /* several restarts/commits in succession alternate slots */
    for (int i = 0; i < 5; i++) { p[0] = (uint8_t)(10 + i); save_commit(p, 64, &seq); }
    CHECK(save_load(out, sizeof(out), &n, &seq) == SAVE_OK && out[0] == 14, "repeated commits keep newest");
    /* no device */
    s_fault = F_NO_DEVICE;
    CHECK(save_commit(p, 64, &seq) == SAVE_NO_DEVICE, "no VMU -> SAVE_NO_DEVICE");
    CHECK(save_load(out, sizeof(out), &n, &seq) == SAVE_NO_DEVICE, "no VMU on load");
    /* full card */
    ram_reset(); s_fault = F_FULL;
    CHECK(save_commit(p, 64, &seq) == SAVE_FULL, "full card -> SAVE_FULL");
    s_fault = F_NONE;
    CHECK(save_load(out, sizeof(out), &n, &seq) == SAVE_NONE, "failed save wrote nothing");
    /* write error */
    s_fault = F_WRITE_ERROR;
    CHECK(save_commit(p, 64, &seq) == SAVE_IO_ERROR, "write error reported");
    /* corruption */
    ram_reset();
    save_commit(p, 64, &seq);
    s_fault = F_READ_GARBAGE;
    CHECK(save_load(out, sizeof(out), &n, &seq) == SAVE_CORRUPT, "bit flip detected by CRC");
    s_fault = F_NONE;
    RS[0].len = 10;
    CHECK(save_load(out, sizeof(out), &n, &seq) == SAVE_CORRUPT, "truncated save detected");
    /* unsupported version is never overwritten */
    ram_reset();
    save_commit(p, 64, &seq);
    RS[0].data[4] = 9; /* version 9 */
    CHECK(save_load(out, sizeof(out), &n, &seq) == SAVE_UNSUPPORTED, "newer version reported");
    int writes = s_writes;
    CHECK(save_commit(p, 64, &seq) == SAVE_UNSUPPORTED && s_writes == writes, "unsupported save left untouched");
    /* too big */
    CHECK(save_commit(p, SAVE_MAX_PAYLOAD + 1, &seq) == SAVE_TOO_BIG, "oversized payload refused");
    /* validator rejects garbage */
    CHECK(save_validate_image("hello", 5, NULL, NULL, NULL) == SAVE_CORRUPT, "garbage image rejected");
    save_set_backend(NULL);
}

/* ------------------------------------------------------------ persistence through the bridge contract */
extern bool persist_snapshot_game(void);
extern void persist_reset_ram(void);
extern int persist_boot_meta(void);

static void test_persist(const char *data) {
    save_set_backend(&RAMB);
    ram_reset();
    host_set_data_root(data);
    CHECK(game_init(), "game data loads for persistence tests");
    /* fake a game state, run the original save protocol via the bridge */
    game_set_sw(91, true); game_set_var(29, 424242); game_set_var(21, 20);
    GS.items[1] = 1;
    game_set_var(VAR_FUNC, FN_SAVE); bridge_invoke(false);
    game_set_var(VAR_FUNC, FN_WRITE_ITEM); game_set_var(VAR_ARG1, 1); bridge_invoke(false);
    game_set_var(VAR_ARG1, 0); bridge_invoke(false);
    game_set_var(VAR_ARG1, 3); bridge_invoke(false);
    game_set_var(VAR_ARG1, 0); bridge_invoke(false);
    game_set_var(VAR_ARG1, 0); bridge_invoke(false);   /* missing note re-sends 0 (as CE26 does) */
    game_set_var(VAR_ARG1, 5); bridge_invoke(false);
    game_set_var(VAR_ARG1, 5); bridge_invoke(false);   /* duplicate */
    CHECK(s_writes == 0, "nothing written while the event is still streaming");
    bridge_tick();
    CHECK(s_writes >= 1, "save written to card");
    /* document puzzle: write clue -> persistent VFS */
    game_set_var(VAR_FUNC, FN_DOCUMENT); bridge_invoke(false);
    CHECK(game_var(VAR_RETURN) == 1 && vfs_file_exists(DOC_CLUE_PATH), "func_Document creates clue file");
    char buf[32]; size_t n;
    vfs_read_file(DOC_CLUE_PATH, buf, sizeof(buf), &n); buf[n] = 0;
    CHECK(!strncmp(buf, "424242", 6), "clue contains the code from var 29");
    /* ending is committed immediately */
    game_set_var(VAR_FUNC, FN_END); game_set_var(VAR_ARG1, 3);
    CHECK(bridge_invoke(false) && BM.ending == 2, "func_End records ending (arg-1)");
    /* simulated power cycle: wipe RAM state, reload from card */
    game_new(); persist_reset_ram(); vfs_clear_layer(VFS_LAYER_SESSION); vfs_clear_layer(VFS_LAYER_PERSISTENT);
    memset(&BM, 0, sizeof(BM));
    CHECK(persist_boot_meta() == 0 && BM.ending == 2, "ending survives power cycle");
    CHECK(vfs_file_exists(DOC_CLUE_PATH), "clue document survives power cycle");
    game_set_var(VAR_FUNC, FN_LOAD); bridge_invoke(false);
    CHECK(game_var(VAR_RETURN) == 2, "load returns 2 when the player's name is unknown");
    CHECK(game_sw(91) && game_var(29) == 424242 && game_var(21) == 20, "switches/variables restored");
    game_set_var(VAR_FUNC, FN_READ_ITEM); bridge_invoke(false);
    CHECK(game_var(VAR_RETURN) == 1, "first item id streamed back");
    bridge_invoke(false); CHECK(game_var(VAR_RETURN) == 0, "item list terminator");
    bridge_invoke(false); CHECK(game_var(VAR_RETURN) == 3, "note id streamed back");
    bridge_invoke(false); CHECK(game_var(VAR_RETURN) == 5, "later note survives repeated zeros (dedup)");
    bridge_invoke(false); CHECK(game_var(VAR_RETURN) == 0, "note list terminator");
    /* integrity recovery: clue deleted while the safe is still locked -> regenerated (not solved) */
    vfs_delete_file(DOC_CLUE_PATH, VFS_LAYER_PERSISTENT);
    game_set_var(VAR_FUNC, FN_LOAD); bridge_invoke(false);
    CHECK(vfs_file_exists(DOC_CLUE_PATH) && !game_sw(92), "missing clue regenerated; puzzle not auto-solved");
    /* no VMU: commit is session-only, never reported as saved */
    s_fault = F_NO_DEVICE;
    char why[128];
    CHECK(persist_commit(why, sizeof(why)) == PERSIST_SESSION_ONLY, "no VMU -> session-only, not success");
    game_set_var(VAR_FUNC, FN_LOAD); bridge_invoke(false);
    CHECK(game_var(VAR_RETURN) >= 1, "RAM copy allows a simulated restart without a VMU");
    /* corrupt card at load */
    s_fault = F_NONE; persist_reset_ram();
    for (int i = 0; i < 4; i++) if (RS[i].used) RS[i].data[40] ^= 0xFF;
    game_set_var(VAR_FUNC, FN_LOAD);
    bool sync = bridge_invoke(false);
    CHECK(!sync && game_var(VAR_RETURN) == 0, "corrupt save -> notice + new game (return 0)");
    /* failed operation must not advance: Document with full VFS returns 0 */
    vfs_clear_layer(VFS_LAYER_PERSISTENT);
    static char big[VFS_MAX_FILE];
    char nm[48]; int k = 0;
    vfs_create_directory("/Documents", VFS_LAYER_PERSISTENT);
    for (int sz = VFS_MAX_FILE; sz >= 1; sz /= 2)   /* fill the pool completely */
        while (k < 63) { snprintf(nm, sizeof(nm), "/Documents/fill%02d", k); if (vfs_create_file(nm, big, (size_t)sz, VFS_T_BINARY, VFS_LAYER_PERSISTENT) != VFS_OK) break; k++; }
    vfs_delete_file(DOC_CLUE_PATH, VFS_LAYER_PERSISTENT);
    game_set_var(VAR_FUNC, FN_DOCUMENT); bridge_invoke(false);
    CHECK(game_var(VAR_RETURN) == 0 && !vfs_file_exists(DOC_CLUE_PATH), "failed document write reports failure");
    save_set_backend(NULL);
}

static void json_str(const char *s) {
    putchar('"');
    for (; *s; s++) { if (*s == '"' || *s == '\\') putchar('\\'); putchar(*s); }
    putchar('"');
}

int main(int argc, char **argv) {
    if (argc > 1 && !strcmp(argv[1], "--registry")) {   /* single source of truth for docs */
        printf("[\n");
        for (int i = 0; i < FN_COUNT; i++) {
            const bridge_entry_t *e = &BRIDGE_REGISTRY[i];
            printf(" {\"func\": %d, \"common_event\": %d, \"name\": ", e->id, e->id + 1); json_str(e->name);
            printf(", \"original_purpose\": "); json_str(e->original_purpose);
            printf(", \"dreamcast_replacement\": "); json_str(e->dreamcast);
            printf(", \"failure_behaviour\": "); json_str(e->failure);
            printf(", \"status\": "); json_str(e->status);
            printf("}%s\n", i + 1 < FN_COUNT ? "," : "");
        }
        printf("]\n");
        return 0;
    }
    test_vfs();
    test_save();
    if (argc > 1) test_persist(argv[1]);
    printf("unit tests: %d passed, %d failed\n", s_pass, s_fail);
    return s_fail ? 1 : 0;
}
