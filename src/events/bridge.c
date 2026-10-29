/* SPDX-License-Identifier: GPL-3.0-or-later
 * Dreamcast replacements for OneShot's native host operations. */
#include "bridge.h"
#include "../game/game.h"
#include "../game/transition.h"
#include "../vfs/vfs.h"
#include "../ui/ui.h"
#include "../desktop/desktop.h"
#include "../audio/audio.h"
#include <stdio.h>

bridge_meta_t BM;

const bridge_entry_t BRIDGE_REGISTRY[FN_COUNT] = {
    {FN_GUESS_NAME, "func_GuessName", "Guess the player's real name from the host computer (used by the Prophet).",
     "Copies the virtual computer's account name (Settings app, default 'Player') into actor 2.",
     "No account name -> actor 2 keeps its current name; the Prophet still asks for a name.", "implemented (approximation)"},
    {FN_SET_NAME, "func_SetName", "Substitute the player's name for the _PlayerName_ placeholder in the next message.",
     "The message renderer always replaces the placeholder with the stored player name.",
     "Unknown name -> actor 2 name is used.", "implemented"},
    {FN_SET_NAME_ENTRY, "func_SetNameEntry", "Store the name typed in EnterHeroName as the player's name.",
     "Stores actor 2's name as the persistent player name (saved with the next commit).",
     "Empty name -> rejected by the on-screen keyboard (cannot confirm empty input).", "implemented"},
    {FN_SHAKE_WINDOW, "func_ShakeWindow", "Shake the game window on the Windows desktop.",
     "Shakes the whole screen for about one second (non-photosensitive path only, as in the data).",
     "None.", "implemented (approximation)"},
    {FN_SET_WALLPAPER, "func_SetWallpaper", "arg1=1: replace the desktop wallpaper with a clue image; arg1=0: restore it. return=1 on success.",
     "Changes the virtual desktop wallpaper (persisted).  The clue wallpaper is generated at conversion time from the solution event M28E36.",
     "If the clue image is missing: return=0 and a clue file is placed on the virtual Desktop instead (the data's own fallback branch).",
     "implemented (clue art is a reconstruction)"},
    {FN_MESSAGE_BOX, "func_MessageBox", "Show a native Windows message box; arg1 selects the text; return=1 for OK/Yes.",
     "Modal virtual system dialog; waits for the controller.  Texts are substitutes (originals are inside the obfuscated EXE).",
     "Unknown id -> generic OK dialog, return=1.", "implemented (substitute text)"},
    {FN_LEAVE_WINDOW, "func_LeaveWindow", "Move the game window off screen at the ending.",
     "Plays a window-leaving animation, then a simulated game close; the next launch boots through Map 1.",
     "None.", "implemented (approximation)"},
    {FN_SAVE, "func_Save", "Begin the one-slot save: snapshot switches/variables (incl. memorised location vars 21-23).",
     "Snapshots switches/variables in RAM; the write happens when the item+skill streams end.",
     "See WriteItem.", "implemented"},
    {FN_WRITE_ITEM, "func_WriteItem", "Stream item ids then note(skill) ids into the save; 0 terminates each list.",
     "Collects ids; after the second terminator commits to the VMU (two-slot, CRC-verified) and keeps a RAM copy.",
     "No VMU / full / write error -> progress kept in RAM for this power session and the player is told honestly.",
     "implemented"},
    {FN_LOAD, "func_Load", "Load the save. return=0 none, 1 loaded, 2 loaded but the player's name is unknown.",
     "Reads the newest valid VMU slot (or the RAM copy), restores switches/variables, prepares item/note streams.",
     "Corrupt/unsupported save -> return=0 (new game) after an explanatory dialog; data on the card is not deleted.",
     "implemented"},
    {FN_READ_ITEM, "func_ReadItem", "Return the next streamed id (items, 0, notes, 0).",
     "Returns ids from the restored lists.", "Exhausted -> 0.", "implemented"},
    {FN_DOCUMENT, "func_Document", "Write a document containing the 6-digit safe code (var 29) into the player's Documents folder.",
     "Creates a text file in the virtual /Documents folder (persistent layer).",
     "VFS full -> return=0 and a diagnostic; the safe stays unsolvable until space is available (never auto-solved).",
     "implemented"},
    {FN_END, "func_End", "Record which ending was reached (arg1 3 or 4) so later launches change.",
     "Sets the persistent ENDING value (arg1-1, matching Map 1's checks) and commits immediately.",
     "Commit failure -> value kept in RAM; the player is warned that a power-off will forget it.",
     "implemented (mapping inferred)"},
    {FN_SET_CLOSE_ENABLED, "func_SetCloseEnabled", "Allow/forbid closing the game window.",
     "Controls whether the virtual computer's 'Close OneShot' action is available.", "None.", "implemented"},
};

static const struct { int id; const char *title; const char *text; bool yesno; } MSGBOX[] = {
    {0, "OneShot", "Connection established.", false},
    {1, "OneShot", "Do you understand?", true},
    {2, "OneShot", "Should I repeat myself?", true},
    {3, "OneShot", "Do you know where to find\nthe Documents folder?", true},
    {4, "OneShot", "...", false},
    {5, "OneShot", "...", false},
    {7, "OneShot", "...", false},
};

static int s_last_func = -1, s_last_result;
static bool s_commit_pending;
static int s_pending_func = -1;
/* Debug-menu cutscene playback: nothing may leave RAM (no VMU commit, no VFS
 * write, no relaunch).  Game/bridge RAM state is restored by the debug menu. */
static bool s_sandbox;
void bridge_set_sandbox(bool on) { s_sandbox = on; if (on) os_diag("bridge: sandbox on"); }
bool bridge_sandboxed(void) { return s_sandbox; }

void bridge_init(void) {
    memset(&BM, 0, sizeof(BM));
    os_strlcpy(BM.wallpaper, "default", sizeof(BM.wallpaper));
    BM.close_enabled = true;
}

void bridge_new_session(void) { s_pending_func = -1; }

const char *bridge_player_name(void) {
    if (BM.name_known && BM.player_name[0]) return BM.player_name;
    const char *a2 = actor_name(2);
    return (a2[0] && strcmp(a2, "TPlayerNameT") != 0) ? a2 : desktop_account_name();
}
void bridge_set_player_name(const char *n) {
    os_strlcpy(BM.player_name, n, sizeof(BM.player_name));
    BM.name_known = n && n[0];
}
bool bridge_close_enabled(void) { return BM.close_enabled; }
int bridge_last_func(void) { return s_last_func; }
int bridge_last_result(void) { return s_last_result; }

static void ret(int v) { game_set_var(VAR_RETURN, v); s_last_result = v; }

/* ---- save streams (func_Save / WriteItem / Load / ReadItem) ---- */
typedef struct {
    bool active;
    int list;          /* 0 items, 1 skills, 2 done */
    uint16_t ids[2][MAX_ITEMS];
    int n[2];
} stream_t;
static stream_t s_write, s_read;
static int s_read_pos[2];
extern bool persist_snapshot_game(void);
extern void persist_set_lists(const uint16_t *items, int ni, const uint16_t *skills, int ns);
extern void persist_get_lists(uint16_t *items, int *ni, uint16_t *skills, int *ns);

static int s_commits;
static persist_status_t s_last_commit = PERSIST_OK;
static char s_last_commit_why[128];
int bridge_commit_count(void) { return s_commits; }
persist_status_t bridge_last_commit(const char **why) { if (why) *why = s_last_commit_why; return s_last_commit; }

static bool start_commit(const char *why_ctx) {
    char why[128];
    persist_status_t st = persist_commit(why, sizeof(why));
    s_commits++; s_last_commit = st; os_strlcpy(s_last_commit_why, why, sizeof(s_last_commit_why));
    if (st == PERSIST_OK) { os_diag("commit (%s): ok", why_ctx); return true; }
    ui_notice_storage(st == PERSIST_SESSION_ONLY, why);
    return false; /* wait for the notice to be dismissed */
}

static bool doc_write(void) {
    int code = (int)game_var(29);
    char text[256];
    snprintf(text, sizeof(text), "%06d\n", code < 0 ? 0 : code % 1000000);
    if (!vfs_file_exists("/Documents")) vfs_create_directory("/Documents", VFS_LAYER_PERSISTENT);
    int r = vfs_create_file(DOC_CLUE_PATH, text, strlen(text), VFS_T_TEXT, VFS_LAYER_PERSISTENT);
    if (r == VFS_OK) desktop_mark_new(DOC_CLUE_PATH);
    return r == VFS_OK;
}

void bridge_check_integrity(void) {
    /* Recovery without solving: if the story says the clue document was
     * created (switch 91) but the safe is not open (switch 92) and the file is
     * gone (e.g. older save, wiped RAM), recreate it deterministically from
     * var 29.  The player still has to find and read it. */
    if (game_sw(91) && !game_sw(92) && !vfs_file_exists(DOC_CLUE_PATH)) {
        os_log("integrity: clue document missing after load -> regenerated from saved state");
        doc_write();
    }
}

bool bridge_invoke(bool is_init) {
    if (is_init) {
        /* Handshake.  Map 1 first clears every switch, then writes a fixed
         * on/off pattern into switches 1-20 and magic numbers into variables
         * 1-4 (8008135, 3133710, 1010101, 1234567) - a signature the modified
         * RPG_RT.exe uses to find the game's switch/variable memory.  If var1
         * is still 8008135 afterwards the game shows the "error" picture.
         * Replacement: acknowledge by clearing the signature (switches 1-20 and
         * variables 1-4 return to the cleared state the event set up). */
        for (int i = 1; i <= 20; i++) game_set_sw(i, false);
        game_set_var(1, 0); game_set_var(2, 0); game_set_var(3, 0); game_set_var(4, 0);
        int32_t ending = BM.ending;
        if (ending == 0 && BM.launches > 1 && !persist_have_snapshot()) ending = 1; /* inferred: deja vu */
        game_set_var(VAR_ENDING, ending);
        os_diag("bridge _init: ending=%d launches=%u", (int)ending, (unsigned)BM.launches);
        return true;
    }
    int fn = (int)game_var(VAR_FUNC);
    int a1 = (int)game_var(VAR_ARG1);
    s_last_func = fn;
    os_diag("bridge func %d (%s) arg1=%d", fn, (fn >= 0 && fn < FN_COUNT) ? BRIDGE_REGISTRY[fn].name : "?", a1);
    switch (fn) {
    case FN_GUESS_NAME: {
        const char *acc = desktop_account_name();
        if (acc[0]) os_strlcpy(GS.actor[2].name, acc, NAME_LEN);
        return true;
    }
    case FN_SET_NAME: return true;
    case FN_SET_NAME_ENTRY: bridge_set_player_name(GS.actor[2].name); return true;
    case FN_SHAKE_WINDOW: GS.shake_power = 6; GS.shake_speed = 8; GS.shake_frames = 60; return true;
    case FN_SET_WALLPAPER: {
        if (s_sandbox) { ret(1); return true; }          /* debug playback: desktop untouched */
        if (a1 == 1) {
            if (desktop_set_wallpaper("clue")) { ret(1); return true; }
            /* the data's fallback: "I've placed an item in the most easily accessible place" */
            ret(desktop_place_clue_file() ? 0 : 0);
            return true;
        }
        desktop_set_wallpaper("default");
        ret(1);
        return true;
    }
    case FN_MESSAGE_BOX: {
        const char *title = "OneShot", *text = "...";
        bool yn = false;
        for (size_t i = 0; i < sizeof(MSGBOX) / sizeof(MSGBOX[0]); i++)
            if (MSGBOX[i].id == a1) { title = MSGBOX[i].title; text = MSGBOX[i].text; yn = MSGBOX[i].yesno; }
        ui_msgbox_open(title, text, yn);
        s_pending_func = fn;
        return false;
    }
    case FN_LEAVE_WINDOW: ui_leave_window_start(); s_pending_func = fn; return false;
    case FN_SAVE:
        if (s_sandbox) { memset(&s_write, 0, sizeof(s_write)); return true; }   /* no save snapshot */
        persist_snapshot_game();
        memset(&s_write, 0, sizeof(s_write));
        s_write.active = true;
        return true;
    case FN_WRITE_ITEM:
        /* VERIFIED usage (CE26 "SAVE GAME"): item ids are written inside their
         * branches, then a 0; the note (skill) checks call WriteItem OUTSIDE
         * their branches, so a missing note re-sends the previous argument
         * (often 0).  So: items until the first 0, then a de-duplicated SET of
         * note ids with zeros ignored.  The commit happens once the event has
         * finished streaming (bridge_tick, next frame). */
        if (s_sandbox) return true;                      /* debug playback: nothing is saved */
        if (!s_write.active) { os_log("bridge: WriteItem without Save (ignored)"); return true; }
        if (s_write.list == 0) {
            if (a1 == 0) s_write.list = 1;
            else if (s_write.n[0] < MAX_ITEMS) s_write.ids[0][s_write.n[0]++] = (uint16_t)a1;
        } else if (a1 > 0) {
            bool dup = false;
            for (int i = 0; i < s_write.n[1]; i++) dup |= s_write.ids[1][i] == a1;
            if (!dup && s_write.n[1] < MAX_ITEMS) s_write.ids[1][s_write.n[1]++] = (uint16_t)a1;
        }
        s_commit_pending = true;
        return true;
    case FN_LOAD: {
        int r = persist_restore();
        if (r < 0) { ui_notice_load_failed(persist_last_error()); ret(0); s_pending_func = fn; return false; }
        if (r > 0) {
            memset(&s_read, 0, sizeof(s_read));
            persist_get_lists(s_read.ids[0], &s_read.n[0], s_read.ids[1], &s_read.n[1]);
            s_read_pos[0] = s_read_pos[1] = 0;
            s_read.active = true;
            bridge_check_integrity();
        }
        ret(r);
        return true;
    }
    case FN_READ_ITEM: {
        if (!s_read.active) { ret(0); return true; }
        int l = s_read.list;
        if (l < 2 && s_read_pos[l] < s_read.n[l]) { ret(s_read.ids[l][s_read_pos[l]++]); return true; }
        s_read.list++;
        if (s_read.list >= 2) s_read.active = false;
        ret(0);
        return true;
    }
    case FN_DOCUMENT:
        if (s_sandbox) { ret(1); return true; }          /* debug playback: no file written */
        ret(doc_write() ? 1 : 0);
        return true;
    case FN_END:
        BM.ending = a1 - 1;
        if (s_sandbox) { os_log("bridge: ending %d NOT recorded (debug playback)", (int)BM.ending); return true; }
        os_log("bridge: ending recorded = %d", (int)BM.ending);
        if (!start_commit("ending")) { s_pending_func = fn; return false; }
        return true;
    case FN_SET_CLOSE_ENABLED: BM.close_enabled = a1 != 0; return true;
    default:
        os_log("bridge: unknown function %d (no effect, return unchanged)", fn);
        return true;
    }
}

/* Called once per frame after the interpreters ran: finishes a save stream. */
void bridge_tick(void) {
    if (!s_commit_pending) return;
    s_commit_pending = false;
    s_write.active = false;
    persist_set_lists(s_write.ids[0], s_write.n[0], s_write.ids[1], s_write.n[1]);
    os_log("bridge: game save (%d items, %d notes)", s_write.n[0], s_write.n[1]);
    start_commit("game save");
}

bool bridge_poll(void) {
    if (s_pending_func < 0) return true;
    if (ui_modal_active()) return false;
    if (s_pending_func == FN_MESSAGE_BOX) ret(ui_msgbox_result());
    if (s_pending_func == FN_LEAVE_WINDOW && !s_sandbox) trans_request(TR_RELAUNCH);
    s_pending_func = -1;
    return true;
}
