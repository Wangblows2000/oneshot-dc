/* SPDX-License-Identifier: GPL-3.0-or-later
 * RPG Maker 2003 event interpreter (subset driven by the commands that the
 * original OneShot data actually uses - see tools/command_coverage.py). */
#include "interp.h"
#include "../game/banim.h"
#include "bridge.h"
#include "../dialogue/message.h"
#include "../audio/audio.h"
#include "../renderer/gfx.h"
#include "../ui/ui.h"
#include "../game/transition.h"
#include "../platform/platform.h"
#include "../util.h"
#include <stdio.h>
#include <stdlib.h>

#define MAX_DEPTH 24
#define MAX_PAR_EVENTS 48
#define MAX_PARALLEL (MAX_PAR_EVENTS + MAX_COMMON_EVENTS)
#define CMDS_PER_FRAME 10000

typedef struct {
    const cmdlist_t *list;
    uint32_t index;
    int event_id;           /* owner for "this event" (0 = none) */
    int8_t branch[64];      /* pending choice result per indent */
} frame_t;

typedef enum { W_NONE, W_MESSAGE, W_ROUTES, W_KEY, W_KEY_ANY, W_TELEPORT, W_BRIDGE, W_UI, W_TRANSITION, W_SCREEN } wait_t;

typedef struct {
    bool used;
    frame_t st[MAX_DEPTH];
    int sp;
    int wait_frames;
    wait_t wait;
    int key_var;
    uint32_t key_mask;     /* keys accepted by KeyInputProc */
    bool key_numbers, key_shift;
    int msg_kind;          /* 0 text, 1 choice, 2 number */
    int msg_var;
    bool parallel;
    character_t *owner;    /* for map parallel events */
    int owner_page;
    int ce_id;             /* for parallel common events */
    uint32_t guard_held;
} interp_t;

static interp_t s_main;
static interp_t s_par[MAX_PARALLEL];
static uint32_t s_executed;
static unsupported_t s_unsup[64];
static int s_nunsup;
static uint32_t s_prev_buttons, s_pressed, s_held;

static void note_unsupported(uint16_t code) {
    for (int i = 0; i < s_nunsup; i++) if (s_unsup[i].code == code) { s_unsup[i].count++; return; }
    if (s_nunsup < 64) { s_unsup[s_nunsup].code = code; s_unsup[s_nunsup].count = 1; s_nunsup++; }
    os_log("interpreter: command %u not supported yet (skipped)", code);
}
int interp_unsupported(unsupported_t *out, int max) {
    int n = s_nunsup < max ? s_nunsup : max;
    memcpy(out, s_unsup, sizeof(unsupported_t) * (size_t)n);
    return s_nunsup;
}
uint32_t interp_commands_executed(void) { return s_executed; }

static void it_reset(interp_t *it) { memset(it, 0, sizeof(*it)); }

static bool it_push(interp_t *it, const cmdlist_t *l, int event_id) {
    if (!l) return false;
    if (it->sp >= MAX_DEPTH) { os_log("interpreter: call stack overflow (ignored call)"); return false; }
    frame_t *f = &it->st[it->sp++];
    memset(f, 0, sizeof(*f));
    memset(f->branch, -1, sizeof(f->branch));
    f->list = l; f->index = 0; f->event_id = event_id;
    return true;
}

void interp_init(void) {
    it_reset(&s_main);
    for (int i = 0; i < MAX_PARALLEL; i++) it_reset(&s_par[i]);
    s_nunsup = 0;
    s_executed = 0;
}
void interp_clear_map(void) {
    it_reset(&s_main);
    for (int i = 0; i < MAX_PARALLEL; i++) it_reset(&s_par[i]);
}
void interp_clear_all(void) { interp_init(); }
void interp_clear_parallels(void) { for (int i = 0; i < MAX_PARALLEL; i++) it_reset(&s_par[i]); }
void interp_on_map_changed(void) {
    /* the running event's owner no longer exists on the new map */
    for (int i = 0; i < s_main.sp; i++) s_main.st[i].event_id = -1;
    s_main.owner = NULL;
}

bool interp_busy(void) { return s_main.sp > 0 || msg_active(); }
bool interp_blocks_events(void) { return s_main.sp > 0; }

static const event_def_t *event_def(int id) {
    const mapdata_t *m = map_data();
    if (!m) return NULL;
    for (int i = 0; i < m->nevents; i++) if (m->events[i].id == id) return &m->events[i];
    return NULL;
}

/* test/diagnostic hooks: which event page / common events ran (no text needed) */
static int s_last_ev, s_last_page;
static uint16_t s_ce_calls[MAX_COMMON_EVENTS + 1];
void interp_test_last_page(int *ev, int *page) { *ev = s_last_ev; *page = s_last_page; }
int interp_test_ce_calls(int id, bool reset) {
    if (id < 1 || id > MAX_COMMON_EVENTS) return 0;
    int n = s_ce_calls[id]; if (reset) s_ce_calls[id] = 0; return n;
}
static void note_ce(int id) { if (id >= 1 && id <= MAX_COMMON_EVENTS && s_ce_calls[id] < 0xFFFF) s_ce_calls[id]++; }

static int s_watch_map, s_watch_id;
static bool s_watch_started;
void interp_watch_event(int map, int id) { s_watch_map = map; s_watch_id = id; s_watch_started = false; }
bool interp_watch_started(void) { return s_watch_started; }

bool interp_start_event(character_t *ev) {
    if (!ev || !ev->active || ev->page < 0 || s_main.sp > 0) return false;
    const event_def_t *d = event_def(ev->id);
    if (!d) return false;
    const page_def_t *p = &d->pages[ev->page];
    if (p->trigger == TRIG_PARALLEL) return false;
    if (p->list.count == 0) return false;
    it_reset(&s_main);
    /* RPG Maker turns the event toward the hero when triggered (unless fixed). */
    if (p->trigger == TRIG_ACTION || p->trigger == TRIG_TOUCH || p->trigger == TRIG_COLLISION) {
        if (!ev->dir_fixed && !ev->tile_sprite) {
            int dx = GS.player.x - ev->x, dy = GS.player.y - ev->y;
            if (dx || dy) char_turn(ev, abs(dx) > abs(dy) ? (dx > 0 ? DIR_RIGHT : DIR_LEFT) : (dy > 0 ? DIR_DOWN : DIR_UP));
        }
    }
    it_push(&s_main, &p->list, ev->id);
    s_main.owner = ev;
    os_diag("start event %d page %d (trigger %d)", ev->id, ev->page + 1, p->trigger);
    s_last_ev = ev->id; s_last_page = ev->page + 1;
    if (s_watch_id && ev->id == s_watch_id && GS.map_id == s_watch_map) s_watch_started = true;
    return true;
}

/* Debug playback: run one specific page of an event on the main interpreter
 * (the page RPG Maker would pick can differ when several pages qualify). */
bool interp_start_event_page(character_t *ev, int page) {
    if (!ev || page < 0 || s_main.sp > 0) return false;
    const event_def_t *d = event_def(ev->id);
    if (!d || page >= d->npages || d->pages[page].list.count == 0) return false;
    it_reset(&s_main);
    it_push(&s_main, &d->pages[page].list, ev->id);
    s_main.owner = ev;
    if (s_watch_id && ev->id == s_watch_id && GS.map_id == s_watch_map) s_watch_started = true;
    os_diag("start event %d page %d (debug playback)", ev->id, page + 1);
    return true;
}

void interp_start_common(int ce_id) {
    if (ce_id < 1 || ce_id > G.n_common) return;
    it_reset(&s_main);
    it_push(&s_main, &G.common[ce_id].list, 0);
}

bool interp_start_auto_common(void) {
    if (s_main.sp > 0) return false;
    for (int c = 1; c <= G.n_common; c++) {
        const common_event_t *ce = &G.common[c];
        if (ce->trigger != 3 || !ce->list.count) continue;
        if (ce->switch_flag && !game_sw(ce->switch_id)) continue;
        it_reset(&s_main);
        it_push(&s_main, &ce->list, 0);
        os_diag("start common event %d (autorun)", c);
        note_ce(c);
        return true;
    }
    return false;
}

void interp_event_page_changed(character_t *ev) {
    /* parallel interpreters are rebuilt by interp_refresh_parallels() */
    (void)ev;
}

void interp_refresh_parallels(void) {
    /* map events: slots [0, MAX_PAR_EVENTS) allocated on demand */
    for (int k = 0; k < MAX_PAR_EVENTS; k++) {
        interp_t *it = &s_par[k];
        if (!it->used) continue;
        character_t *e = it->owner;
        bool keep = e && e->active && e->trigger == TRIG_PARALLEL && e->page == it->owner_page;
        if (!keep) it_reset(it);
    }
    for (int i = 0; i < GS.nev; i++) {
        character_t *e = &GS.ev[i];
        if (!(e->active && e->trigger == TRIG_PARALLEL)) continue;
        bool have = false;
        for (int k = 0; k < MAX_PAR_EVENTS && !have; k++) have = s_par[k].used && s_par[k].owner == e;
        if (have) continue;
        int k = 0;
        while (k < MAX_PAR_EVENTS && s_par[k].used) k++;
        if (k == MAX_PAR_EVENTS) { os_log("interpreter: too many parallel events (event %d ignored)", e->id); continue; }
        it_reset(&s_par[k]);
        s_par[k].used = true; s_par[k].parallel = true; s_par[k].owner = e; s_par[k].owner_page = e->page;
    }
    /* parallel common events (trigger 4): active when no switch condition or the switch is ON */
    for (int c = 1; c <= G.n_common && c <= MAX_COMMON_EVENTS; c++) {
        interp_t *it = &s_par[MAX_PAR_EVENTS + c - 1];
        const common_event_t *ce = &G.common[c];
        bool want = ce->trigger == 4 && (!ce->switch_flag || game_sw(ce->switch_id)) && ce->list.count;
        if (!want) { if (it->used) it_reset(it); continue; }
        if (it->used) continue;
        it_reset(it);
        it->used = true; it->parallel = true; it->ce_id = c;
    }
}

/* ------------------------------------------------------------- helpers */
static frame_t *top(interp_t *it) { return it->sp ? &it->st[it->sp - 1] : NULL; }
static int P(const cmd_t *c, int i) { return i < c->nparams ? c->params[i] : 0; }
static void cmd_string(const cmd_t *c, char *out, size_t cap) {
    size_t n = c->slen < cap - 1 ? c->slen : cap - 1;
    memcpy(out, c->str, n); out[n] = 0;
}
static int val_or_var(int mode, int v) { return mode ? game_var(v) : v; }

/* Skip forward to the next command with one of the codes at exactly this indent. */
static bool skip_to(frame_t *f, int indent, uint16_t a, uint16_t b) {
    for (uint32_t i = f->index + 1; i < f->list->count; i++) {
        const cmd_t *c = &f->list->cmds[i];
        if (c->indent < indent) return false;
        if (c->indent == indent && (c->code == a || c->code == b)) { f->index = i; return true; }
    }
    return false;
}

/* ------------------------------------------------------------- conditions */
static bool eval_condition(interp_t *it, const cmd_t *c) {
    switch (P(c, 0)) {
    case 0: return game_sw(P(c, 1)) == (P(c, 2) == 0);
    case 1: {
        int32_t a = game_var(P(c, 1)), b = val_or_var(P(c, 2), P(c, 3));
        switch (P(c, 4)) {
        case 0: return a == b; case 1: return a >= b; case 2: return a <= b;
        case 3: return a > b; case 4: return a < b; case 5: return a != b;
        }
        return false;
    }
    case 4: {
        int id = P(c, 1);
        int have = (id >= 1 && id <= MAX_ITEMS) ? GS.items[id] : 0;
        for (int a = 1; a <= MAX_ACTORS && id >= 1; a++) for (int s = 0; s < 5; s++) if (GS.actor[a].equip[s] == id) have++;
        return P(c, 2) == 0 ? have > 0 : have == 0;
    }
    case 5: {
        int a = P(c, 1);
        if (a < 1 || a > MAX_ACTORS) return false;
        switch (P(c, 2)) {
        case 0: for (int i = 0; i < GS.party_n; i++) if (GS.party[i] == a) return true; return false;
        case 1: { char s[NAME_LEN]; cmd_string(c, s, sizeof(s)); return strcmp(s, GS.actor[a].name) == 0; }
        case 4: { int sk = P(c, 3); return sk >= 1 && sk <= MAX_SKILLS && GS.actor[a].skills[sk]; }
        case 5: { int item = P(c, 3); for (int s = 0; s < 5; s++) if (GS.actor[a].equip[s] == item) return true; return false; }
        default: os_diag("actor condition %d unsupported", P(c, 2)); return false;
        }
    }
    case 6: {
        character_t *ch = map_char(P(c, 1), top(it)->event_id);
        return ch && ch->facing == P(c, 2);
    }
    case 8: return it == &s_main && s_main.owner && s_main.owner->trigger == TRIG_ACTION;
    default:
        os_diag("conditional type %d unsupported -> false", P(c, 0));
        return false;
    }
}

/* ------------------------------------------------------------- variables */
static int32_t operand(interp_t *it, const cmd_t *c) {
    int t = P(c, 4), a = P(c, 5), b = P(c, 6);
    switch (t) {
    case 0: return a;
    case 1: return game_var(a);
    case 2: return game_var(game_var(a));
    case 3: return os_rand_range(a, b);
    case 4: return (a >= 1 && a <= MAX_ITEMS) ? (b == 0 ? GS.items[a] : 0) : 0;
    case 5: return 0; /* actor stats: unused by OneShot */
    case 6: {
        character_t *ch = map_char(a, top(it)->event_id);
        if (!ch) return 0;
        switch (b) {
        case 0: return GS.map_id;
        case 1: return ch->x;
        case 2: return ch->y;
        case 3: { static const int d2k[4] = {8, 6, 2, 4}; return d2k[ch->facing & 3]; }
        case 4: return (ch->rx - GS.display_x) * TILE / REAL_TILE + 8;
        case 5: return (ch->ry - GS.display_y) * TILE / REAL_TILE + 16;
        }
        return 0;
    }
    case 7:
        if (a == 2) return GS.party_n;
        return 0;
    default: return 0;
    }
}

static void control_vars(interp_t *it, const cmd_t *c) {
    int lo, hi;
    if (P(c, 0) == 0) lo = hi = P(c, 1);
    else if (P(c, 0) == 1) { lo = P(c, 1); hi = P(c, 2); }
    else lo = hi = game_var(P(c, 1));
    if (hi - lo > MAX_VARIABLES) hi = lo + MAX_VARIABLES;
    for (int v = lo; v <= hi; v++) {
        int32_t x = operand(it, c), cur = game_var(v), r = cur;
        switch (P(c, 3)) {
        case 0: r = x; break;
        case 1: r = cur + x; break;
        case 2: r = cur - x; break;
        case 3: r = cur * x; break;
        case 4: r = x ? cur / x : cur; break;
        case 5: r = x ? cur % x : 0; break;
        }
        if (r > 9999999) r = 9999999;
        if (r < -9999999) r = -9999999;
        game_set_var(v, r);
    }
    GS.need_refresh = true;
}

/* ------------------------------------------------------------- pictures */
static void show_picture(const cmd_t *c) {
    int id = P(c, 0);
    if (id < 1 || id > MAX_PICTURES) return;
    picture_t *p = &GS.pic[id];
    memset(p, 0, sizeof(*p));
    p->shown = true;
    cmd_string(c, p->name, sizeof(p->name));
    p->x = p->tx = (float)val_or_var(P(c, 1), P(c, 2));
    p->y = p->ty = (float)val_or_var(P(c, 1), P(c, 3));
    p->fixed_to_map = P(c, 4) > 0;
    p->mag = p->tmag = (float)P(c, 5);
    p->trans_top = p->ttop = (float)P(c, 6);
    p->keyed = P(c, 7) > 0;
    p->r = p->tr = P(c, 8); p->g = p->tg = P(c, 9); p->b = p->tb = P(c, 10); p->sat = p->tsat = P(c, 11);
    p->effect = P(c, 12);
    p->effect_power = P(c, 12) ? P(c, 13) : 0;
    p->trans_bot = p->tbot = c->nparams > 14 ? (float)P(c, 14) : p->trans_top;
    gfx_tex(CAT_PICTURE, p->name, p->keyed, LIFE_ROOM); /* preload */
}

static int move_picture(const cmd_t *c) {
    int id = P(c, 0);
    if (id < 1 || id > MAX_PICTURES) return 0;
    picture_t *p = &GS.pic[id];
    p->tx = (float)val_or_var(P(c, 1), P(c, 2));
    p->ty = (float)val_or_var(P(c, 1), P(c, 3));
    p->tmag = (float)P(c, 5);
    p->ttop = (float)P(c, 6);
    p->tr = P(c, 8); p->tg = P(c, 9); p->tb = P(c, 10); p->tsat = P(c, 11);
    p->tbot = c->nparams > 16 ? (float)P(c, 16) : p->ttop;
    p->duration = P(c, 14) * 6;
    if (p->duration <= 0) {
        p->x = p->tx; p->y = p->ty; p->mag = p->tmag; p->trans_top = p->ttop; p->trans_bot = p->tbot;
        p->r = p->tr; p->g = p->tg; p->b = p->tb; p->sat = p->tsat;
    }
    return P(c, 15) ? p->duration : 0;
}

/* ------------------------------------------------------------- move route */
static int decode_route(const cmd_t *c, movecmd_t *out, int max) {
    int n = 0;
    for (int i = 4; i < c->nparams && n < max;) {
        movecmd_t *m = &out[n++];
        memset(m, 0, sizeof(*m));
        m->code = (uint8_t)P(c, i++);
        if (m->code == 32 || m->code == 33) m->a = P(c, i++);
        else if (m->code == 34 || m->code == 35) {
            int len = P(c, i++);
            int k = 0;
            for (; k < len && i < c->nparams; k++, i++) if (k < NAME_LEN - 1) m->s[k] = (char)P(c, i);
            m->s[k < NAME_LEN ? k : NAME_LEN - 1] = 0;
            m->a = P(c, i++);
            if (m->code == 35) { m->b = P(c, i++); m->c = P(c, i++); }
        }
    }
    return n;
}

/* ------------------------------------------------------------- messages */
static bool start_message(interp_t *it, frame_t *f) {
    if (msg_active()) return false;
    msg_begin();
    const cmd_t *c = &f->list->cmds[f->index];
    msg_add_line(c->str, c->slen);
    while (f->index + 1 < f->list->count && f->list->cmds[f->index + 1].code == 20110) {
        f->index++;
        msg_add_line(f->list->cmds[f->index].str, f->list->cmds[f->index].slen);
    }
    it->msg_kind = 0;
    /* A following ShowChoice / InputNumber shares the window if it fits. */
    if (f->index + 1 < f->list->count) {
        const cmd_t *n = &f->list->cmds[f->index + 1];
        if (n->code == 10140) {
            char labels[4][64];
            int cnt = 0;
            char s[200];
            cmd_string(n, s, sizeof(s));
            char *tok = s;
            while (tok && cnt < 4) {
                char *sl = strchr(tok, '/');
                if (sl) *sl = 0;
                os_strlcpy(labels[cnt++], tok, 64);
                tok = sl ? sl + 1 : NULL;
            }
            if (msg_line_count() + cnt <= MSG_MAX_LINES) {
                f->index++;
                msg_set_choices(labels, cnt, P(n, 0));
                for (int i = 0; i < cnt; i++) msg_add_line(labels[i], (int)strlen(labels[i]));
                it->msg_kind = 1;
            }
        } else if (n->code == 10150 && msg_line_count() < MSG_MAX_LINES) {
            f->index++;
            msg_set_number(P(n, 0));
            it->msg_kind = 2;
            it->msg_var = P(n, 1);
        }
    }
    msg_show();
    it->wait = W_MESSAGE;
    return true;
}

/* ------------------------------------------------------------- main step */
typedef enum { R_NEXT, R_STAY, R_JUMPED } res_t;

static res_t exec(interp_t *it) {
    frame_t *f = top(it);
    const cmd_t *c = &f->list->cmds[f->index];
    s_executed++;
    char s[200];
    switch (c->code) {
    case 0: case 10: case 22011: case 20141: case 12410: case 22410: case 12110: case 12210: return R_NEXT;
    case 10110: return start_message(it, f) ? R_NEXT : R_STAY;
    case 20110: return R_NEXT; /* consumed by start_message */
    case 10120: GS.msg_transparent = P(c, 0); GS.msg_position = P(c, 1); GS.msg_auto_pos = P(c, 2); GS.msg_event_move = P(c, 3); return R_NEXT;
    case 10130: cmd_string(c, GS.face_name, sizeof(GS.face_name)); GS.face_index = P(c, 0); GS.face_right = P(c, 1); GS.face_flip = P(c, 2); return R_NEXT;
    case 10140: { /* ShowChoice without preceding text */
        if (msg_active()) return R_STAY;
        msg_begin();
        char labels[4][64]; int cnt = 0;
        cmd_string(c, s, sizeof(s));
        char *tok = s;
        while (tok && cnt < 4) { char *sl = strchr(tok, '/'); if (sl) *sl = 0; os_strlcpy(labels[cnt++], tok, 64); tok = sl ? sl + 1 : NULL; }
        msg_set_choices(labels, cnt, P(c, 0));
        for (int i = 0; i < cnt; i++) msg_add_line(labels[i], (int)strlen(labels[i]));
        msg_show();
        it->msg_kind = 1; it->wait = W_MESSAGE;
        return R_NEXT;
    }
    case 20140: { /* choice branch */
        int8_t *br = &f->branch[c->indent & 63];
        if (*br == P(c, 0)) { *br = -1; return R_NEXT; }
        if (!skip_to(f, c->indent, 20140, 20141)) return R_NEXT;
        return R_JUMPED;
    }
    case 10150:
        if (msg_active()) return R_STAY;
        msg_begin(); msg_set_number(P(c, 0)); msg_show();
        it->msg_kind = 2; it->msg_var = P(c, 1); it->wait = W_MESSAGE;
        return R_NEXT;
    case 10210: {
        int lo = P(c, 1), hi = P(c, 0) == 0 ? lo : P(c, 2);
        if (P(c, 0) == 2) lo = hi = game_var(P(c, 1));
        if (hi - lo > MAX_SWITCHES) hi = lo + MAX_SWITCHES;
        for (int i = lo; i <= hi; i++) game_set_sw(i, P(c, 3) == 0 ? true : P(c, 3) == 1 ? false : !game_sw(i));
        GS.need_refresh = true;
        interp_refresh_parallels();
        return R_NEXT;
    }
    case 10220: control_vars(it, c); return R_NEXT;
    case 10320: {
        int id = val_or_var(P(c, 1), P(c, 2)), n = val_or_var(P(c, 3), P(c, 4));
        if (id >= 1 && id <= MAX_ITEMS) GS.items[id] = (uint8_t)clampi(GS.items[id] + (P(c, 0) == 0 ? n : -n), 0, 99);
        GS.need_refresh = true;
        return R_NEXT;
    }
    case 10330: {
        int a = val_or_var(P(c, 1), P(c, 2));
        if (a < 1 || a > MAX_ACTORS) return R_NEXT;
        int k = -1;
        for (int i = 0; i < GS.party_n; i++) if (GS.party[i] == a) k = i;
        if (P(c, 0) == 0 && k < 0 && GS.party_n < MAX_PARTY) GS.party[GS.party_n++] = a;
        if (P(c, 0) == 1 && k >= 0) { for (int i = k; i < GS.party_n - 1; i++) GS.party[i] = GS.party[i + 1]; GS.party_n--; }
        return R_NEXT;
    }
    case 10440: { /* ChangeSkills */
        int a = P(c, 0) == 0 ? leader_actor() : P(c, 0) == 1 ? P(c, 1) : game_var(P(c, 1));
        int sk = val_or_var(P(c, 3), P(c, 4));
        if (a >= 1 && a <= MAX_ACTORS && sk >= 1 && sk <= MAX_SKILLS) GS.actor[a].skills[sk] = P(c, 2) == 0;
        return R_NEXT;
    }
    case 10450: { /* ChangeEquipment */
        int a = P(c, 0) == 0 ? leader_actor() : P(c, 0) == 1 ? P(c, 1) : game_var(P(c, 1));
        if (a < 1 || a > MAX_ACTORS) return R_NEXT;
        if (P(c, 2) == 0) {
            int item = val_or_var(P(c, 3), P(c, 4));
            if (item >= 1 && item <= MAX_ITEMS) {
                int t = G.item_type[item];
                int slot = t >= 1 && t <= 5 ? t - 1 : 4;
                if (GS.actor[a].equip[slot] && GS.actor[a].equip[slot] <= MAX_ITEMS) GS.items[GS.actor[a].equip[slot]]++;
                if (GS.items[item]) GS.items[item]--;
                GS.actor[a].equip[slot] = item;
            }
        } else {
            int slot = P(c, 3);
            for (int sl = 0; sl < 5; sl++)
                if (slot == 5 || sl == slot) {
                    int item = GS.actor[a].equip[sl];
                    if (item >= 1 && item <= MAX_ITEMS) GS.items[item]++;
                    GS.actor[a].equip[sl] = 0;
                }
        }
        GS.need_refresh = true;
        return R_NEXT;
    }
    case 10630: {
        int a = P(c, 0);
        if (a >= 1 && a <= MAX_ACTORS) {
            cmd_string(c, GS.actor[a].charset, sizeof(GS.actor[a].charset));
            GS.actor[a].char_index = P(c, 1);
            GS.actor[a].transparent = P(c, 2);
            if (a == leader_actor()) {
                os_strlcpy(GS.player.sprite, GS.actor[a].charset, NAME_LEN);
                GS.player.sprite_index = GS.actor[a].char_index;
                GS.player.opacity = P(c, 2) ? 160 : 255;
            }
        }
        return R_NEXT;
    }
    case 10670: { /* ChangeSystemSFX - also the OneShot bridge trigger */
        cmd_string(c, s, sizeof(s));
        if (P(c, 0) == 0 && (os_stricmp(s, "_func") == 0 || os_stricmp(s, "_init") == 0)) {
            if (!bridge_invoke(os_stricmp(s, "_init") == 0)) { it->wait = W_BRIDGE; }
            return R_NEXT;
        }
        int w = P(c, 0);
        if (w >= 0 && w <= 3) {
            os_strlcpy(GS.sys_se[w].name, s, NAME_LEN);
            GS.sys_se[w].vol = (uint8_t)P(c, 1); GS.sys_se[w].tempo = (uint8_t)P(c, 2); GS.sys_se[w].pan = (uint8_t)P(c, 3);
        }
        return R_NEXT;
    }
    case 10680: cmd_string(c, GS.system_name, sizeof(GS.system_name)); gfx_set_system(GS.system_name); return R_NEXT;
    case 10740: { /* EnterHeroName -> on-screen keyboard */
        int a = P(c, 0);
        if (a < 1 || a > MAX_ACTORS) return R_NEXT;
        ui_osk_open("Enter a name", P(c, 2) ? GS.actor[a].name : "", 12, a);
        it->wait = W_UI;
        return R_NEXT;
    }
    case 10810: { /* Teleport */
        trans_teleport(P(c, 0), P(c, 1), P(c, 2), P(c, 3));
        it->wait = W_TELEPORT;
        return R_NEXT;
    }
    case 10820: game_set_var(P(c, 0), GS.map_id); game_set_var(P(c, 1), GS.player.x); game_set_var(P(c, 2), GS.player.y); return R_NEXT;
    case 10830: trans_teleport(game_var(P(c, 0)), game_var(P(c, 1)), game_var(P(c, 2)), 0); it->wait = W_TELEPORT; return R_NEXT;
    case 10860: {
        character_t *ch = map_char(P(c, 0), f->event_id);
        if (ch) {
            int x = val_or_var(P(c, 1), P(c, 2)), y = val_or_var(P(c, 1), P(c, 3));
            ch->x = clampi(x, 0, map_w() - 1); ch->y = clampi(y, 0, map_h() - 1);
            ch->rx = ch->x * REAL_TILE; ch->ry = ch->y * REAL_TILE;
            if (c->nparams > 4 && P(c, 4) > 0) char_turn(ch, (P(c, 4) - 1) & 3);
        }
        return R_NEXT;
    }
    case 11010: trans_erase_screen(P(c, 0)); it->wait = W_TRANSITION; return R_NEXT;
    case 11020: trans_show_screen(P(c, 0)); it->wait = W_TRANSITION; return R_NEXT;
    case 11030: {
        GS.tint_target[0] = P(c, 0); GS.tint_target[1] = P(c, 1); GS.tint_target[2] = P(c, 2); GS.tint_target[3] = P(c, 3);
        GS.tint_frames = P(c, 4) * 6;
        if (!GS.tint_frames) memcpy(GS.tint, GS.tint_target, sizeof(GS.tint));
        if (P(c, 5)) it->wait_frames = GS.tint_frames;
        return R_NEXT;
    }
    case 11040:
        GS.flash_rgb[0] = P(c, 0) * 255 / 31; GS.flash_rgb[1] = P(c, 1) * 255 / 31; GS.flash_rgb[2] = P(c, 2) * 255 / 31;
        GS.flash_level = P(c, 3) * 255 / 31; GS.flash_frames = P(c, 4) * 6;
        if (P(c, 5)) it->wait_frames = GS.flash_frames;
        return R_NEXT;
    case 11050:
        GS.shake_power = P(c, 0); GS.shake_speed = P(c, 1); GS.shake_frames = P(c, 2) * 6;
        if (P(c, 3)) it->wait_frames = GS.shake_frames;
        return R_NEXT;
    case 11060: os_diag("PanScreen approximated (no-op)"); return R_NEXT;
    case 11110: show_picture(c); return R_NEXT;
    case 11120: it->wait_frames = move_picture(c); return R_NEXT;
    case 11130: { int id = P(c, 0); if (id >= 1 && id <= MAX_PICTURES) GS.pic[id].shown = false; return R_NEXT; }
    case 11210: { /* ShowBattleAnimation: emote balloons over a character (game/banim.c) */
        const character_t *tg = map_char(P(c, 1), f->event_id);
        int dur = banim_start(P(c, 0), tg);
        os_diag("battle animation %d on %d: %d frames", P(c, 0), P(c, 1), dur);
        if (P(c, 2)) it->wait_frames = dur > 0 ? dur : 30;
        return R_NEXT;
    }
    case 11330: {
        character_t *ch = map_char(P(c, 0), f->event_id);
        if (ch) {
            movecmd_t tmp[64];
            int n = decode_route(c, tmp, 64);
            ch->freq = clampi(P(c, 1), 1, 8);
            ch->max_stop_count = ch->freq >= 8 ? 0 : 1 << (9 - ch->freq);
            char_set_route(ch, tmp, n, P(c, 2) != 0, P(c, 3) != 0);
        }
        return R_NEXT;
    }
    case 11340: it->wait = W_ROUTES; return R_NEXT;
    case 11350:
        GS.player.route_forced = false;
        for (int i = 0; i < GS.nev; i++) if (GS.ev[i].route_forced) { GS.ev[i].route_forced = false; GS.ev[i].route = NULL; GS.ev[i].route_len = 0; }
        return R_NEXT;
    case 11410:
        if (c->nparams > 1 && P(c, 1)) { it->wait = W_KEY_ANY; it->guard_held = s_held; }
        else it->wait_frames = P(c, 0) == 0 ? 1 : P(c, 0) * 6;
        return R_NEXT;
    case 11510: cmd_string(c, s, sizeof(s)); audio_bgm_play(s, P(c, 1), P(c, 2)); return R_NEXT;
    case 11520: audio_bgm_fadeout(P(c, 0)); return R_NEXT;
    case 11550: cmd_string(c, s, sizeof(s)); audio_se_play(s, P(c, 0), P(c, 2)); return R_NEXT;
    case 11610: { /* KeyInputProc (RPG Maker 2003 1.08+ layout, 14 params) */
        it->key_var = P(c, 0);
        uint32_t m = 0;
        if (P(c, 3)) m |= BTN_A;
        if (P(c, 4)) m |= BTN_B;
        it->key_numbers = P(c, 5) != 0;
        it->key_shift = c->nparams > 9 ? P(c, 9) != 0 : false;
        if (c->nparams > 13) {
            if (P(c, 10)) m |= BTN_DOWN;
            if (P(c, 11)) m |= BTN_LEFT;
            if (P(c, 12)) m |= BTN_RIGHT;
            if (P(c, 13)) m |= BTN_UP;
        } else if (P(c, 2)) m |= BTN_UP | BTN_DOWN | BTN_LEFT | BTN_RIGHT;
        it->key_mask = m;
        if (P(c, 1)) { game_set_var(it->key_var, 0); it->wait = W_KEY; it->guard_held = s_held; return R_NEXT; }
        /* non-waiting: sample now */
        int code = 0;
        if ((m & BTN_DOWN) && (s_held & BTN_DOWN)) code = 1;
        else if ((m & BTN_LEFT) && (s_held & BTN_LEFT)) code = 2;
        else if ((m & BTN_RIGHT) && (s_held & BTN_RIGHT)) code = 3;
        else if ((m & BTN_UP) && (s_held & BTN_UP)) code = 4;
        else if ((m & BTN_A) && (s_held & BTN_A)) code = 5;
        else if ((m & BTN_B) && (s_held & BTN_B)) code = 6;
        else if (it->key_shift && (s_held & BTN_Y)) code = 7;
        else if (it->key_numbers && (s_held & BTN_X)) code = 10;
        game_set_var(it->key_var, code);
        return R_NEXT;
    }
    case 11720: cmd_string(c, GS.parallax, sizeof(GS.parallax)); return R_NEXT;
    case 11960: GS.menu_enabled = P(c, 0) != 0; return R_NEXT;
    case 12010: {
        if (eval_condition(it, c)) return R_NEXT;
        if (!skip_to(f, c->indent, 22010, 22011)) return R_NEXT;
        return R_NEXT; /* land on Else (enter its body) or EndBranch */
    }
    case 22010: /* reached Else from the true branch: skip to EndBranch */
        skip_to(f, c->indent, 22011, 22011);
        return R_JUMPED;
    case 12120: { /* JumpToLabel */
        for (uint32_t i = 0; i < f->list->count; i++) {
            const cmd_t *l = &f->list->cmds[i];
            if (l->code == 12110 && P(l, 0) == P(c, 0)) { f->index = i; return R_NEXT; }
        }
        return R_NEXT;
    }
    case 12220: { /* BreakLoop: find innermost enclosing Loop, jump past its EndLoop */
        int min_indent = c->indent;
        for (int32_t i = (int32_t)f->index - 1; i >= 0; i--) {
            const cmd_t *l = &f->list->cmds[i];
            if (l->indent < min_indent) {
                min_indent = l->indent;
                if (l->code == 12210) {
                    for (uint32_t k = (uint32_t)i + 1; k < f->list->count; k++) {
                        const cmd_t *e = &f->list->cmds[k];
                        if (e->code == 22210 && e->indent == l->indent) { f->index = k; return R_NEXT; }
                    }
                    break;
                }
            }
        }
        return R_NEXT;
    }
    case 22210: { /* EndLoop: back to the matching Loop */
        for (int32_t i = (int32_t)f->index - 1; i >= 0; i--) {
            const cmd_t *l = &f->list->cmds[i];
            if (l->code == 12210 && l->indent == c->indent) { f->index = (uint32_t)i; return R_NEXT; }
        }
        return R_NEXT;
    }
    case 12310: /* EndEventProcessing */
        f->index = f->list->count; return R_JUMPED;
    case 12320: { /* EraseEvent */
        character_t *ch = map_char(CHAR_THIS, f->event_id);
        if (ch && ch != &GS.player) { ch->erased = true; ch->active = false; }
        return R_NEXT;
    }
    case 12330: { /* CallEvent */
        const cmdlist_t *l = NULL;
        int owner = f->event_id;
        if (P(c, 0) == 0) {
            int id = P(c, 1);
            if (id >= 1 && id <= G.n_common) { l = &G.common[id].list; note_ce(id); }
        } else {
            int eid = P(c, 0) == 1 ? P(c, 1) : game_var(P(c, 1));
            int pg = P(c, 0) == 1 ? P(c, 2) : game_var(P(c, 2));
            if (eid == CHAR_THIS) eid = f->event_id;
            const event_def_t *d = event_def(eid);
            if (d && pg >= 1 && pg <= d->npages) { l = &d->pages[pg - 1].list; owner = eid; }
        }
        f->index++;
        if (l) it_push(it, l, owner);
        return R_JUMPED;
    }
    case 12510: trans_request(TR_TITLE); it->wait = W_TRANSITION; return R_NEXT;
    default:
        note_unsupported(c->code);
        return R_NEXT;
    }
}

static bool waiting(interp_t *it) {
    if (it->wait_frames > 0) { it->wait_frames--; return true; }
    switch (it->wait) {
    case W_NONE: return false;
    case W_MESSAGE:
        if (msg_active()) return true;
        if (it->msg_kind == 1) {
            /* the ShowChoice command index was consumed; record result for its indent */
            frame_t *f = top(it);
            if (f) {
                /* next command is the first ShowChoiceOption, its indent is the choice indent */
                int ind = f->index < f->list->count ? f->list->cmds[f->index].indent : 0;
                f->branch[ind & 63] = (int8_t)msg_choice_result();
            }
        } else if (it->msg_kind == 2) {
            game_set_var(it->msg_var, msg_number_result());
            GS.need_refresh = true;
        }
        it->msg_kind = 0;
        it->wait = W_NONE;
        return false;
    case W_ROUTES:
        if (any_forced_route_running()) return true;
        it->wait = W_NONE; return false;
    case W_KEY: case W_KEY_ANY: {
        it->guard_held &= s_held;
        uint32_t pr = s_pressed & ~it->guard_held;
        if (it->wait == W_KEY_ANY) {
            if (pr & (BTN_A | BTN_B)) { it->wait = W_NONE; return false; }
            return true;
        }
        int code = 0;
        uint32_t m = it->key_mask;
        if ((m & BTN_DOWN) && (pr & BTN_DOWN)) code = 1;
        else if ((m & BTN_LEFT) && (pr & BTN_LEFT)) code = 2;
        else if ((m & BTN_RIGHT) && (pr & BTN_RIGHT)) code = 3;
        else if ((m & BTN_UP) && (pr & BTN_UP)) code = 4;
        else if ((m & BTN_A) && (pr & BTN_A)) code = 5;
        else if ((m & BTN_B) && (pr & BTN_B)) code = 6;
        else if (it->key_shift && (pr & BTN_Y)) code = 7;
        else if (it->key_numbers && (pr & BTN_X)) code = 10;
        if (!code) return true;
        game_set_var(it->key_var, code);
        GS.need_refresh = true;
        it->wait = W_NONE;
        return false;
    }
    case W_TELEPORT: case W_TRANSITION:
        if (trans_busy()) return true;
        it->wait = W_NONE; return false;
    case W_BRIDGE:
        if (!bridge_poll()) return true;
        it->wait = W_NONE; return false;
    case W_UI:
        if (ui_modal_active()) return true;
        it->wait = W_NONE; return false;
    case W_SCREEN: it->wait = W_NONE; return false;
    }
    return false;
}

/* Run one interpreter until it blocks or finishes this frame. */
static void run(interp_t *it) {
    for (int n = 0; n < CMDS_PER_FRAME; n++) {
        if (waiting(it)) return;
        frame_t *f = top(it);
        if (!f) return;
        if (f->index >= f->list->count) {
            it->sp--;
            if (it->sp == 0) {
                if (it == &s_main) s_main.owner = NULL;
                return;
            }
            continue;
        }
        if (GS.teleport_pending && it != &s_main && it->parallel) return; /* parallels pause during map change */
        res_t r = exec(it);
        f = top(it);
        if (!f) return;
        if (r == R_NEXT) f->index++;
        /* R_JUMPED: index already positioned */
        if (it->wait != W_NONE || it->wait_frames > 0) return;
    }
    os_diag("interpreter yielded after %d commands (possible tight loop)", CMDS_PER_FRAME);
}

void interp_update(void) {
    uint32_t b = plat_poll_buttons();
    s_pressed = b & ~s_prev_buttons;
    s_held = b;
    s_prev_buttons = b;

    if (GS.need_refresh) map_refresh_pages();
    run(&s_main);
    for (int i = 0; i < MAX_PARALLEL; i++) {
        interp_t *it = &s_par[i];
        if (!it->used) continue;
        if (it->sp == 0) {
            /* (re)start the loop */
            const cmdlist_t *l = NULL;
            int owner = 0;
            if (it->ce_id) l = &G.common[it->ce_id].list;
            else if (it->owner && it->owner->active) {
                const event_def_t *d = event_def(it->owner->id);
                if (d && it->owner->page >= 0) { l = &d->pages[it->owner->page].list; owner = it->owner->id; }
            }
            if (!l || !l->count) { it->used = false; continue; }
            it_push(it, l, owner);
        }
        run(it);
        if (GS.need_refresh) map_refresh_pages();
    }
}
