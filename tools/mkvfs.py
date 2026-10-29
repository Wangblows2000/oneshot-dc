#!/usr/bin/env python3
"""Build the immutable base layer of the virtual filesystem (vfs_base.dat) and
the generated clue wallpaper.

SPDX-License-Identifier: GPL-3.0-or-later

Usage: mkvfs.py --game GAME_DIR --out DATA_DIR

The clue wallpaper is NOT copied from the original game (the original image is
embedded in the obfuscated RPG_RT.exe and was not extracted).  It is drawn here
from the solution that the game's own event (Map 28, event 36 "puzzle solve
check") tests, so it is derived from the user's data at build time and no
solution is stored in this repository.

Base layout (all read-only on the disc):
    /Desktop/            files the game places on the desktop
    /Documents/          where the safe-code document is written
    /Pictures/
    /Desktop/Read me.txt Dreamcast controls for the virtual computer
"""
import argparse
import json
import os
import struct
import sys

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import lcf  # noqa: E402

README = """This is your computer.

Files placed here by OneShot live on
the virtual computer, outside the game.

Controls
  D-pad   move between icons / files
  A       open
  B       close window / go back
  L / R   switch between windows
  Y + pad move the focused window
  START   return to the game

Nothing here is saved unless the game
saves to a memory card (VMU).
"""


def find_pixel_solution(game):
    """Return ({(col,row): on}, cols, rows, where).

    The solution comes from the event that sets switch 103 (it tests switches
    121-150).  The GRID LAYOUT comes from the floor panels themselves: the map
    events that toggle each of those switches, placed at their real tile
    positions (Map 28: 5 columns x 6 rows, numbered row by row)."""
    sol_raw, where, panels = None, None, {}
    for fn in sorted(os.listdir(game)):
        if not (fn.lower().startswith("map") and fn.lower().endswith(".lmu")):
            continue
        m = lcf.load_map(os.path.join(game, fn))
        for ev in m.events:
            for pg in ev.pages:
                cmds = pg.commands
                for c in cmds:   # a floor panel: a touch event that switches one of 121-150
                    if c.code == 10210 and c.params[1] == c.params[2] and 121 <= c.params[1] <= 150 and pg.trigger == 1:
                        panels[c.params[1]] = (ev.x, ev.y)
                for i, c in enumerate(cmds):
                    if sol_raw is None and c.code == 10210 and c.params[1] <= 103 <= c.params[2] and c.params[3] == 0:
                        cand = {cc.params[1]: cc.params[2] == 0 for cc in cmds[:i]
                                if cc.code == 12010 and cc.params[0] == 0 and 121 <= cc.params[1] <= 150}
                        if len(cand) == 30:
                            sol_raw, where = cand, f"{fn} event {ev.id}"
    if sol_raw is None or len(panels) != 30:
        return None, 0, 0, None
    x0 = min(x for x, _ in panels.values()); y0 = min(y for _, y in panels.values())
    cols = max(x for x, _ in panels.values()) - x0 + 1; rows = max(y for _, y in panels.values()) - y0 + 1
    sol = {(panels[sw][0] - x0, panels[sw][1] - y0): on for sw, on in sol_raw.items()}
    return sol, cols, rows, where


def rgb555(r, g, b):
    return 0x8000 | ((r >> 3) << 10) | ((g >> 3) << 5) | (b >> 3)


def write_img(path, w, h, pal, px):
    out = bytearray(b"OSI1") + struct.pack("<HH", w, h)
    pal = (pal + [(0, 0, 0)] * 256)[:256]
    for r, g, b in pal:
        out += struct.pack("<H", rgb555(r, g, b))
    out += bytes(px)
    os.makedirs(os.path.dirname(path), exist_ok=True)
    open(path, "wb").write(out)


def draw_wallpaper(sol, cols, rows, path):
    """160x120 original wallpaper: dusk gradient with the lamp grid laid out like
    the floor panels (cols x rows, north at the top)."""
    w, h = 160, 120
    pal = [(0, 0, 0)]
    for i in range(32):  # gradient 1..32
        pal.append((18 + i * 2, 12 + i, 40 + i * 3))
    pal += [(250, 214, 92), (60, 50, 90), (255, 255, 255)]  # 33 lit, 34 unlit, 35 frame
    px = bytearray(w * h)
    for y in range(h):
        for x in range(w):
            px[y * w + x] = 1 + min(31, y * 32 // h)
    cell = min(14, (w - 16) // cols, (h - 16) // rows)
    gx, gy = (w - cols * cell) // 2, (h - rows * cell) // 2
    for x in range(gx - 3, gx + cols * cell + 2):
        for y in (gy - 3, gy + rows * cell + 1):
            px[y * w + x] = 35
    for y in range(gy - 3, gy + rows * cell + 2):
        for x in (gx - 3, gx + cols * cell + 1):
            px[y * w + x] = 35
    for (c, r), on in sol.items():
        for y in range(gy + r * cell + 2, gy + r * cell + cell - 2):
            for x in range(gx + c * cell + 2, gx + c * cell + cell - 2):
                px[y * w + x] = 33 if on else 34
    write_img(path, w, h, pal, px)
    return dict(cols=cols, rows=rows, cell=cell, gx=gx, gy=gy)


def vfs_entry(kind, typ, path, data=b""):
    p = path.encode("ascii")
    assert len(p) < 256 and len(data) <= 4096
    return struct.pack("<BBB", kind, typ, len(p)) + p + struct.pack("<I", len(data)) + data


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--game", required=True)
    ap.add_argument("--out", required=True)
    a = ap.parse_args()
    ents = [
        vfs_entry(1, 2, "/Desktop"), vfs_entry(1, 2, "/Documents"), vfs_entry(1, 2, "/Pictures"),
        vfs_entry(0, 0, "/Desktop/Read me.txt", README.encode("cp1252")),
    ]
    blob = b"OSVF" + struct.pack("<HH", 1, len(ents)) + b"".join(ents)
    open(os.path.join(a.out, "vfs_base.dat"), "wb").write(blob)

    sol, cols, rows, where = find_pixel_solution(a.game)
    mf_path = os.path.join(a.out, "manifest.json")
    mf = json.load(open(mf_path))
    mf["assets"] = [x for x in mf["assets"] if x["name"] != "~wall_clue"]
    if sol:
        rel = "img/04/wall_clue.img"
        geo = draw_wallpaper(sol, cols, rows, os.path.join(a.out, rel))
        mf["assets"].append(dict(cat="Picture", name="~wall_clue", path=rel, used=True, w=160, h=120,
                                 generated_from=where, grid=geo))
        print(f"clue wallpaper generated from {where}: {cols} x {rows} grid laid out like the floor panels")
    else:
        print("WARNING: pixel-puzzle solution event not found; clue wallpaper not generated")
    json.dump(mf, open(mf_path, "w"), indent=1)
    # append the generated asset to game.dat's manifest table
    gd_path = os.path.join(a.out, "game.dat")
    gd = bytearray(open(gd_path, "rb").read())
    # manifest table is the last section: u16 count then entries
    # rebuild it from manifest.json to keep a single source of truth
    cats = {"ChipSet": 1, "CharSet": 2, "FaceSet": 3, "Picture": 4, "Panorama": 5, "System": 6, "System2": 7,
            "Title": 8, "GameOver": 9, "Battle": 10, "Sound": 20, "Music": 21}
    old_n = len([x for x in mf["assets"] if x["name"] != "~wall_clue"])
    # locate the table: scan backwards is fragile; convert.py records the offset
    off = mf.get("manifest_offset")
    if off is None:
        sys.exit("manifest_offset missing from manifest.json (re-run convert.py)")
    tbl = bytearray()
    es = sorted(mf["assets"], key=lambda e: (cats[e["cat"]], e["name"].lower()))
    tbl += struct.pack("<H", len(es))
    for e in es:
        n = e["name"].lower().encode("cp1252"); p = e["path"].encode("ascii")
        tbl += struct.pack("<BB", cats[e["cat"]], len(n)) + n + struct.pack("<B", len(p)) + p
    open(gd_path, "wb").write(gd[:off] + tbl)
    print(f"vfs_base.dat: {len(blob)} bytes, {len(ents)} entries; game.dat manifest: {len(es)} assets (was {old_n})")


if __name__ == "__main__":
    main()
