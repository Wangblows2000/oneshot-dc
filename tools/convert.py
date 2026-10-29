#!/usr/bin/env python3
"""Offline converter: original RPG Maker 2003 OneShot data -> compact Dreamcast data.

SPDX-License-Identifier: GPL-3.0-or-later

Usage:
    python3 tools/convert.py --game <original game dir> --out <cd data dir> [--maps 1-6] [--no-audio]

Never writes into --game.  Produces (all little-endian, documented in docs/FORMATS.md):
    game.dat              database, map tree, asset manifest
    map/NNNN.map          one file per converted map
    img/<cat>/NNN.img     8-bit indexed images + RGB555 palette
    se/NNN.wav            Yamaha-ADPCM mono 22.05 kHz sound effects (KOS snd_sfx)
    bgm/NNN.wav           16-bit PCM stereo 22.05 kHz music (libwav streaming)
    font.bin              6x12 bitmap font, 256 glyphs (public-domain X11 misc-fixed)
    manifest.json         human-readable manifest used by tools/validate.py
"""
from __future__ import annotations

import argparse
import gzip
import json
import os
import struct
import subprocess
import sys
import zlib

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import lcf  # noqa: E402
import autotile  # noqa: E402

GAME_MAGIC = b"OSGD"
MAP_MAGIC = b"OSMP"
MAP_FORMAT_VERSION = 2   # 2: page condition actor id (u16) after compare_op
IMG_MAGIC = b"OSI1"
FORMAT_VERSION = 1

MAX_STR = 255
IMAGE_DIRS = {  # RM folder -> category id
    "ChipSet": 1, "CharSet": 2, "FaceSet": 3, "Picture": 4, "Panorama": 5,
    "System": 6, "System2": 7, "Title": 8, "GameOver": 9, "Battle": 10,
}
AUDIO_DIRS = {"Sound": 20, "Music": 21}
CAT_NAMES = {v: k for k, v in {**IMAGE_DIRS, **AUDIO_DIRS}.items()}


class W:
    """Little-endian writer with bounds-checked helpers."""

    def __init__(self):
        self.b = bytearray()

    def u8(self, v):
        if not 0 <= v <= 0xFF:
            raise ValueError(f"u8 out of range: {v}")
        self.b += struct.pack("<B", v)

    def u16(self, v):
        if not 0 <= v <= 0xFFFF:
            raise ValueError(f"u16 out of range: {v}")
        self.b += struct.pack("<H", v)

    def i16(self, v):
        self.b += struct.pack("<h", max(-32768, min(32767, v)))

    def i32(self, v):
        self.b += struct.pack("<i", v)

    def u32(self, v):
        self.b += struct.pack("<I", v)

    def raw(self, data):
        self.b += data

    def string(self, s: str):
        enc = s.encode(lcf.TEXT_ENCODING, errors="replace")
        if len(enc) > MAX_STR:
            raise ValueError(f"string too long ({len(enc)}): {s[:40]!r}")
        self.u8(len(enc))
        self.b += enc

    def align4(self):
        while len(self.b) % 4:
            self.b += b"\0"


def write_cmdlist(w: W, cmds):
    """Command list: u32 count, then per command (4-byte aligned):
       u16 code, u16 indent, u16 nparams, u16 strlen, i32 params[n], bytes str, pad4."""
    w.align4()
    w.u32(len(cmds))
    for c in cmds:
        s = c.string.encode(lcf.TEXT_ENCODING, errors="replace")
        if len(c.params) > 0xFFFF or len(s) > 0xFFFF:
            raise ValueError("command too large")
        w.u16(c.code); w.u16(c.indent); w.u16(len(c.params)); w.u16(len(s))
        for p in c.params:
            w.i32(p)
        w.raw(s)
        w.align4()


# --------------------------------------------------------------------- images
def read_xyz(path):
    d = open(path, "rb").read()
    if d[:4] != b"XYZ1":
        raise ValueError(f"{path}: not XYZ1")
    w, h = struct.unpack_from("<HH", d, 4)
    raw = zlib.decompress(d[8:])
    if len(raw) != 768 + w * h:
        raise ValueError(f"{path}: bad XYZ payload size")
    return w, h, raw[:768], raw[768:]


def rgb555(r, g, b):
    return 0x8000 | ((r >> 3) << 10) | ((g >> 3) << 5) | (b >> 3)


def convert_image(src, dst):
    w, h, pal, px = read_xyz(src)
    out = W()
    out.raw(IMG_MAGIC); out.u16(w); out.u16(h)
    for i in range(256):
        out.u16(rgb555(pal[i * 3], pal[i * 3 + 1], pal[i * 3 + 2]))
    out.raw(px)
    os.makedirs(os.path.dirname(dst), exist_ok=True)
    open(dst, "wb").write(out.b)
    return w, h


# ---------------------------------------------------------------------- audio
def convert_audio(src, dst, music: bool):
    os.makedirs(os.path.dirname(dst), exist_ok=True)
    if music:   # streamed with libwav/snd_stream: 16-bit PCM stereo is the proven path
        args = ["-ac", "2", "-ar", "22050", "-c:a", "pcm_s16le"]
    else:       # KOS snd_sfx accepts ffmpeg's Yamaha ADPCM (format 0x20), 4:1 smaller in sound RAM
        args = ["-ac", "1", "-ar", "22050", "-c:a", "adpcm_yamaha"]
    cmd = ["ffmpeg", "-loglevel", "error", "-y", "-i", src, *args, dst]
    r = subprocess.run(cmd, capture_output=True, text=True)
    if r.returncode != 0:
        raise RuntimeError(f"ffmpeg failed for {src}: {r.stderr.strip()}")


# ----------------------------------------------------------------------- font
CP1252_FALLBACK = {0x80: "E", 0x82: ",", 0x84: '"', 0x85: ".", 0x8B: "<", 0x91: "'", 0x92: "'",
                   0x93: '"', 0x94: '"', 0x95: "*", 0x96: "-", 0x97: "-", 0x9B: ">", 0x99: "T"}


def build_font(dst):
    pcf = "/usr/share/fonts/X11/misc/6x12-ISO8859-1.pcf.gz"
    bdf = subprocess.run(["pcf2bdf", "/dev/stdin"], input=gzip.open(pcf).read(),
                         capture_output=True, check=True).stdout.decode("latin-1")
    glyphs = {}
    cur = None
    rows = None
    for line in bdf.splitlines():
        if line.startswith("ENCODING"):
            cur = int(line.split()[1])
        elif line.startswith("BBX"):
            bw, bh, bx, by = map(int, line.split()[1:])
            bbx = (bw, bh, bx, by)
        elif line == "BITMAP":
            rows = []
        elif line == "ENDCHAR":
            if cur is not None and 0 <= cur < 256:
                bw, bh, bx, by = bbx
                # font ascent 10, descent 2 for 6x12: place rows into a 12-row cell
                cell = [0] * 12
                top = 10 - (bh + by)
                for i, r in enumerate(rows):
                    y = top + i
                    if 0 <= y < 12:
                        v = int(r, 16) >> (len(r) * 4 - 8)  # left-aligned 8 bits
                        cell[y] = (v >> bx) & 0xFC if bx >= 0 else v & 0xFC
                glyphs[cur] = cell
            rows = None
        elif rows is not None:
            rows.append(line.strip())
    for k, ch in CP1252_FALLBACK.items():
        glyphs[k] = glyphs.get(ord(ch), [0] * 12)
    out = bytearray()
    for i in range(256):
        out += bytes(glyphs.get(i, [0] * 12))
    open(dst, "wb").write(out)
    return len([g for g in glyphs if any(glyphs[g])])


# --------------------------------------------------------------------- manifest
class Manifest:
    def __init__(self, game, out, audio):
        self.game, self.out, self.audio = game, out, audio
        self.entries = {}  # (cat, lowername) -> dict
        self.missing = []

    def scan(self):
        """Index every asset the original game ships (case-insensitive, as on Windows)."""
        for d, cat in {**IMAGE_DIRS, **AUDIO_DIRS}.items():
            p = os.path.join(self.game, d)
            if not os.path.isdir(p):
                continue
            for i, f in enumerate(sorted(os.listdir(p))):
                base, ext = os.path.splitext(f)
                key = (cat, base.lower())
                if key in self.entries:
                    continue
                self.entries[key] = dict(cat=cat, name=base, src=os.path.join(p, f), ext=ext.lower(),
                                         path=None, used=False)

    def convert_all(self):
        counters = {}
        for key in sorted(self.entries, key=lambda k: (k[0], k[1])):
            e = self.entries[key]
            n = counters.get(e["cat"], 0)
            counters[e["cat"]] = n + 1
            if e["cat"] in IMAGE_DIRS.values():
                if e["ext"] != ".xyz":
                    continue
                rel = f"img/{e['cat']:02d}/{n:03d}.img"
                e["w"], e["h"] = convert_image(e["src"], os.path.join(self.out, rel))
                e["path"] = rel
            else:
                rel = f"{'bgm' if e['cat'] == 21 else 'se'}/{n:03d}.wav"
                if self.audio:
                    convert_audio(e["src"], os.path.join(self.out, rel), e["cat"] == 21)
                e["path"] = rel

    def ref(self, cat, name, where):
        if not name or name == "(OFF)":
            return
        k = (cat, name.lower())
        if k in self.entries:
            self.entries[k]["used"] = True
        else:
            self.missing.append(dict(cat=CAT_NAMES[cat], name=name, where=where))

    def write(self, w: W):
        es = [e for e in self.entries.values() if e["path"]]
        es.sort(key=lambda e: (e["cat"], e["name"].lower()))
        w.u16(len(es))
        for e in es:
            w.u8(e["cat"]); w.string(e["name"].lower()); w.string(e["path"])
        return es


# ------------------------------------------------------- reference collection
# Event command codes that reference assets by name (verified in the data).
REF_CODES = {
    10130: 3,   # ChangeFaceGraphic -> FaceSet
    10630: 2,   # ChangeSpriteAssociation -> CharSet
    10680: 6,   # ChangeSystemGraphics -> System
    11110: 4,   # ShowPicture -> Picture
    11510: 21,  # PlayBGM -> Music
    11550: 20,  # PlaySound -> Sound
    10670: 20,  # ChangeSystemSFX -> Sound
    11720: 5,   # ChangePBG -> Panorama
}


def collect_refs(man: Manifest, cmds, where):
    for c in cmds:
        cat = REF_CODES.get(c.code)
        if cat:
            man.ref(cat, c.string, where)
        if c.code == 11330:  # MoveEvent: inline move commands may carry graphics/sounds
            pass


# ------------------------------------------------------------------- animations
def write_animations(db, man: "Manifest", path):
    """anim.dat: the database's battle animations, which events show on the map
    (ShowBattleAnimation).  Layout in docs/FORMATS.md."""
    anims = lcf.read_array(db.raw_chunks.get(0x13, b"\0"))
    w = W()
    w.raw(b"OSAN"); w.u16(1); w.u16(len(anims))
    for aid in sorted(anims):
        a = anims[aid]
        fname = lcf.c_str(a, 0x02)
        man.ref(10, fname, f"database.animation{aid}")
        frames = lcf.read_array(a[0x0C]) if a.get(0x0C) else {}
        tims = lcf.read_array(a[0x06]) if a.get(0x06) else {}
        w.u16(aid); w.string(fname[:60]); w.u8(lcf.c_int(a, 0x09, 0)); w.u8(lcf.c_int(a, 0x0A, 2))
        w.u8(1 if lcf.c_int(a, 0x03, 0) else 0)
        w.u16(len(frames))
        for fi in sorted(frames):
            cells = lcf.read_array(frames[fi][0x01]) if frames[fi].get(0x01) else {}
            cl = [cells[k] for k in sorted(cells)]
            w.u8(min(len(cl), 255))
            for c in cl[:255]:
                w.u8(1 if lcf.c_int(c, 0x01, 1) else 0); w.u8(lcf.c_int(c, 0x02, 0) & 0xFF)
                w.i16(lcf.c_int(c, 0x03, 0)); w.i16(lcf.c_int(c, 0x04, 0)); w.u16(max(0, lcf.c_int(c, 0x05, 100)))
                for cid, dflt in ((0x06, 100), (0x07, 100), (0x08, 100), (0x09, 100)):
                    w.u8(max(0, min(200, lcf.c_int(c, cid, dflt))))
                w.u8(max(0, min(100, lcf.c_int(c, 0x0A, 0))))
        w.u16(len(tims))
        for ti in sorted(tims):
            t = tims[ti]
            se = lcf.read_chunks(lcf.Reader(t[0x02])) if t.get(0x02) else {}
            sname = lcf.c_str(se, 1, "(OFF)")
            man.ref(20, sname, f"database.animation{aid}.timing")
            w.u16(lcf.c_int(t, 0x01, 0)); w.string(sname[:60])
            w.u8(lcf.c_int(se, 3, 100)); w.u8(lcf.c_int(se, 4, 100)); w.u8(lcf.c_int(se, 5, 50))
            w.u8(lcf.c_int(t, 0x03, 0))
            for cid in (0x04, 0x05, 0x06, 0x07):
                w.u8(max(0, min(31, lcf.c_int(t, cid, 31))))
            w.u8(lcf.c_int(t, 0x08, 0))
    open(path, "wb").write(bytes(w.b))
    return len(anims)


# ------------------------------------------------------------------- database
def write_database(db, maps, order, start, man: Manifest, n_maps_written):
    w = W()
    w.raw(GAME_MAGIC); w.u16(FORMAT_VERSION)
    w.u16(max(db.switches) if db.switches else 0)
    w.u16(max(db.variables) if db.variables else 0)
    # system
    s = db.system
    for name, cat in ((s["system"], 6), (s["system2"], 7), (s["title"], 8), (s["gameover"], 9)):
        man.ref(cat, name, "database.system")
        w.string(name)
    for k in ("cursor", "decision", "cancel", "buzzer"):
        snd = s["sounds"][k]
        nm = lcf.c_str(snd, 1, "(OFF)")
        man.ref(20, nm, f"database.system.{k}")
        w.string(nm); w.u8(lcf.c_int(snd, 3, 100)); w.u8(lcf.c_int(snd, 4, 100)); w.u8(lcf.c_int(snd, 5, 50))
    tm = s["title_music"]
    man.ref(21, lcf.c_str(tm, 1, "(OFF)"), "database.system.title_music")
    w.string(lcf.c_str(tm, 1, "(OFF)"))
    w.u8(s["transition_out"] & 0xFF); w.u8(s["transition_in"] & 0xFF); w.u8(s["message_stretch"] & 0xFF)
    # actors
    n = max(db.actors) if db.actors else 0
    w.u16(n)
    for i in range(1, n + 1):
        a = db.actors.get(i, dict(name="", charset="", char_index=0, face="", face_index=0))
        man.ref(2, a["charset"], f"actor{i}"); man.ref(3, a["face"], f"actor{i}")
        w.string(a["name"]); w.string(a["charset"]); w.u8(a["char_index"]); w.string(a["face"]); w.u8(a["face_index"])
    # items: name, type, switch, description
    n = max(db.items) if db.items else 0
    w.u16(n)
    for i in range(1, n + 1):
        it = db.item_info.get(i, dict(name="", desc="", type=0, switch_id=1))
        w.string(it["name"]); w.u8(it["type"] & 0xFF); w.u16(it["switch_id"]); w.string(it["desc"][:95])
    # skills ("Notes" in OneShot): name, type, switch, description
    n = max(db.skills) if db.skills else 0
    w.u16(n)
    for i in range(1, n + 1):
        sk = db.skills.get(i, dict(name="", desc="", type=0, switch_id=1))
        w.string(sk["name"]); w.u8(sk["type"] & 0xFF); w.u16(sk["switch_id"]); w.string(sk["desc"][:95])
    mc = s["menu_commands"][:8]
    w.u8(len(mc))
    for c in mc:
        w.u8(c)
    for k in (0x6A, 0x6B, 0x6C, 0x70, 0x97, 0x98, 0x99):
        w.string(db.terms.get(k, ""))
    # chipsets
    n = max(db.chipsets)
    w.u16(n)
    for i in range(1, n + 1):
        c = db.chipsets.get(i)
        if c is None:
            c = dict(file="", anim_type=0, anim_speed=0, pass_lower=[15] * 162, pass_upper=[15] * 144)
        man.ref(1, c["file"], f"chipset{i}")
        w.string(c["file"]); w.u8(c["anim_type"]); w.u8(c["anim_speed"])
        pl = (c["pass_lower"] + [15] * 162)[:162]
        pu = (c["pass_upper"] + [15] * 144)[:144]
        w.raw(bytes(pl)); w.raw(bytes(pu))
    # common events
    n = max(db.common_events)
    w.u16(n)
    for i in range(1, n + 1):
        ce = db.common_events.get(i, dict(name="", trigger=5, switch_flag=False, switch_id=1, commands=[]))
        collect_refs(man, ce["commands"], f"CE{i}")
        w.string(ce["name"]); w.u8(ce["trigger"]); w.u8(1 if ce["switch_flag"] else 0); w.u16(ce["switch_id"])
        write_cmdlist(w, ce["commands"])
    # map tree
    ids = sorted(k for k in maps if k > 0)
    w.u16(len(ids))
    for k in ids:
        mi = maps[k]
        man.ref(21, mi.music if mi.music_type == 2 else "", f"maptree{k}")
        w.u16(k); w.string(mi.name); w.u16(mi.parent); w.u8(mi.music_type & 0xFF); w.string(mi.music)
        w.u8(1 if k in n_maps_written else 0)
    w.u16(start["map"]); w.u16(start["x"]); w.u16(start["y"])
    return w


# ------------------------------------------------------------------------ maps
def write_map(mid, m: lcf.MapUnit, man: Manifest):
    w = W()
    w.raw(MAP_MAGIC); w.u16(MAP_FORMAT_VERSION)
    w.u16(m.width); w.u16(m.height); w.u16(m.chipset_id); w.u8(m.scroll_type)
    w.u8(1 if m.parallax_flag else 0)
    man.ref(5, m.parallax_name if m.parallax_flag else "", f"map{mid}.parallax")
    w.string(m.parallax_name if m.parallax_flag else "")
    w.u8(int(m.parallax_loop_x)); w.u8(int(m.parallax_loop_y))
    w.u8(int(m.parallax_auto_x)); w.i16(m.parallax_sx); w.u8(int(m.parallax_auto_y)); w.i16(m.parallax_sy)
    for t in m.lower:
        w.u16(t & 0xFFFF)
    for t in m.upper:
        w.u16(t & 0xFFFF)
    bad = 0
    for t in m.lower:
        for f in range(autotile.FRAMES):
            q = autotile.resolve_lower(t, f)
            if q is None:
                bad += 1
                q = [autotile.EMPTY] * 4
            for (x, y) in q:
                w.u8(x); w.u8(y)
    for t in m.upper:
        q = autotile.resolve_upper(t) or [autotile.EMPTY] * 4
        for (x, y) in q:
            w.u8(x); w.u8(y)
    w.u16(len(m.events))
    for ev in m.events:
        w.u16(ev.id); w.i16(ev.x); w.i16(ev.y); w.string(ev.name)
        w.u16(len(ev.pages))
        for p in ev.pages:
            man.ref(2, p.charset, f"map{mid}.ev{ev.id}.p{p.index}")
            w.u8(p.cond_flags); w.u16(p.switch_a); w.u16(p.switch_b); w.u16(p.variable_id)
            w.i32(p.variable_value); w.u16(p.item_id); w.u8(p.compare_op & 0xFF); w.u16(p.actor_id)
            w.string(p.charset); w.u8(p.char_index & 0xFF); w.u8(p.direction & 0xFF); w.u8(p.pattern & 0xFF)
            w.u8(int(p.translucent)); w.u8(p.move_type & 0xFF); w.u8(p.move_freq & 0xFF); w.u8(p.trigger & 0xFF)
            w.u8(p.layer & 0xFF); w.u8(int(p.overlap_forbidden)); w.u8(p.anim_type & 0xFF); w.u8(p.move_speed & 0xFF)
            r = p.route
            w.u8(int(r.repeat)); w.u8(int(r.skippable)); w.u16(len(r.cmds))
            for mc in r.cmds:
                w.u8(mc.code); w.i32(mc.a); w.i32(mc.b); w.i32(mc.c); w.string(mc.s)
            collect_refs(man, p.commands, f"map{mid}.ev{ev.id}.p{p.index}")
            write_cmdlist(w, p.commands)
    return w, bad


def parse_range(s):
    out = set()
    for part in s.split(","):
        if "-" in part:
            a, b = part.split("-")
            out |= set(range(int(a), int(b) + 1))
        elif part:
            out.add(int(part))
    return out


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--game", required=True)
    ap.add_argument("--out", required=True)
    ap.add_argument("--maps", default="all", help="e.g. 1-6 or all")
    ap.add_argument("--no-audio", action="store_true")
    a = ap.parse_args()

    game = os.path.abspath(a.game)
    out = os.path.abspath(a.out)
    if out.startswith(game + os.sep) or out == game:
        sys.exit("refusing to write inside the original game directory")
    os.makedirs(out, exist_ok=True)

    db = lcf.load_database(os.path.join(game, "RPG_RT.ldb"))
    maps, order, start = lcf.load_maptree(os.path.join(game, "RPG_RT.lmt"))
    man = Manifest(game, out, not a.no_audio)
    man.scan()

    wanted = None if a.maps == "all" else parse_range(a.maps)
    written = set()
    os.makedirs(os.path.join(out, "map"), exist_ok=True)
    report = dict(maps={}, bad_tiles=0)
    for fn in sorted(os.listdir(game)):
        if not (fn.lower().startswith("map") and fn.lower().endswith(".lmu")):
            continue
        mid = int(fn[3:7])
        if wanted is not None and mid not in wanted:
            continue
        m = lcf.load_map(os.path.join(game, fn))
        w, bad = write_map(mid, m, man)
        open(os.path.join(out, "map", f"{mid:04d}.map"), "wb").write(w.b)
        written.add(mid)
        report["maps"][mid] = dict(w=m.width, h=m.height, events=len(m.events), bytes=len(w.b))
        report["bad_tiles"] += bad

    gw = write_database(db, maps, order, start, man, written)
    n_anims = write_animations(db, man, os.path.join(out, "anim.dat"))
    print(f"battle animations: {n_anims}")
    man.convert_all()
    manifest_offset = len(gw.b)
    es = man.write(gw)
    open(os.path.join(out, "game.dat"), "wb").write(gw.b)
    glyphs = build_font(os.path.join(out, "font.bin"))

    mf = dict(format_version=FORMAT_VERSION, maps=sorted(written), start=start, manifest_offset=manifest_offset,
              assets=[dict(cat=CAT_NAMES[e["cat"]], name=e["name"], path=e["path"], used=e["used"],
                           **({"w": e["w"], "h": e["h"]} if "w" in e else {})) for e in es],
              missing_refs=man.missing, glyphs=glyphs, audio=not a.no_audio, report=report)
    json.dump(mf, open(os.path.join(out, "manifest.json"), "w"), indent=1)
    print(f"maps: {len(written)}  assets: {len(es)}  missing refs: {len(man.missing)}  "
          f"bad tiles: {report['bad_tiles']}  glyphs: {glyphs}  game.dat: {len(gw.b)} bytes")
    if man.missing:
        for m in man.missing[:20]:
            print("  MISSING", m)


if __name__ == "__main__":
    main()
