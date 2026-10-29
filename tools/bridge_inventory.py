#!/usr/bin/env python3
"""Build the external-dependency inventory: every call of OneShot's func_* bridge
(common events 1-14) and the _init handshake, with arguments, merged with the
runtime registry (docs/generated/bridge_registry.json) and test coverage.

SPDX-License-Identifier: GPL-3.0-or-later
Usage: bridge_inventory.py GAME_DIR REGISTRY_JSON OUT_JSON
"""
import glob
import json
import os
import sys

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import lcf  # noqa: E402

TESTS = {
    0: ["(manual) Prophet dialogue, Map 16 event 7"],
    1: ["tests/scripts/safe_puzzle.txt (placeholder substitution in safe dialogue)"],
    2: ["tests/scripts/restart_persistence.txt (name entry after reload)"],
    3: [],
    4: ["tests/scripts/restart_persistence.txt (wallpaper survives relaunch)"],
    5: ["tests/scripts/safe_puzzle.txt (modal Yes/No dialog)"],
    6: [],
    7: ["tests/unit_tests.c test_persist", "tests/scripts/restart_persistence.txt"],
    8: ["tests/unit_tests.c test_persist (zero/duplicate handling)"],
    9: ["tests/unit_tests.c test_persist", "tests/scripts/restart_persistence.txt", "tests/scripts/no_vmu.txt"],
    10: ["tests/unit_tests.c test_persist"],
    11: ["tests/scripts/safe_puzzle.txt", "tests/unit_tests.c test_persist (full VFS -> failure)"],
    12: ["tests/unit_tests.c test_persist (ending survives power cycle)"],
    13: ["(manual) ending sequence, Maps 60/61"],
}


def main():
    game, reg_path, out = sys.argv[1:4]
    reg = {e["func"]: e for e in json.load(open(reg_path))}
    db = lcf.load_database(os.path.join(game, "RPG_RT.ldb"))
    sites = {i: [] for i in range(14)}

    def scan(tag, cmds):
        for i, c in enumerate(cmds):
            if c.code == 12330 and c.params[:1] == [0] and 1 <= c.params[1] <= 14:
                fn = c.params[1] - 1
                arg = None
                for p in reversed(cmds[max(0, i - 6):i]):
                    if p.code == 10220 and p.params[1] == 2 and p.params[2] == 2 and p.params[4] == 0:
                        arg = p.params[5]
                        break
                ret = any(cc.code == 12010 and cc.params[:2] == [1, 6] for cc in cmds[i + 1:i + 4])
                sites[fn].append(dict(where=tag, arg1=arg, return_checked=ret))

    for k, ce in db.common_events.items():
        if k > 14:
            scan(f"CE{k} {ce['name']}", ce["commands"])
    init_sites = []
    for p in sorted(glob.glob(os.path.join(game, "Map*.lmu"))):
        mid = int(os.path.basename(p)[3:7])
        for ev in lcf.load_map(p).events:
            for pg in ev.pages:
                scan(f"Map{mid} event {ev.id} '{ev.name}' page {pg.index}", pg.commands)
                for c in pg.commands:
                    if c.code == 10670 and c.string == "_init":
                        init_sites.append(f"Map{mid} event {ev.id} '{ev.name}' page {pg.index}")
    inv = []
    for fn in range(14):
        e = dict(reg[fn])
        s = sites[fn]
        # collapse the long CE26/CE27 streams
        e["call_sites"] = s if len(s) <= 12 else s[:6] + [dict(where=f"... {len(s) - 6} more", arg1=None, return_checked=False)]
        e["call_site_count"] = len(s)
        e["tests"] = TESTS.get(fn, [])
        e["verified_from_data"] = True
        e["original_native_code_inspected"] = False
        inv.append(e)
    inv.append(dict(func="_init", name="_init handshake", call_sites=[dict(where=w) for w in init_sites],
                    original_purpose="Start-up handshake: switches 1-20 and vars 1-4 hold a signature; var1 must change from 8008135.",
                    dreamcast_replacement="Clears the signature (switches 1-20, vars 1-4), sets var7 ENDING from persistent meta.",
                    failure_behaviour="If not handled, Map 1 shows the original 'error' picture (game unplayable) - so it is always handled.",
                    status="implemented (inferred)", tests=["tests/scripts/opening.txt"], verified_from_data=True,
                    original_native_code_inspected=False))
    json.dump(inv, open(out, "w"), indent=1)
    for e in inv:
        print(f"{str(e['func']):>5} {e['name']:22s} sites={len(e['call_sites']):3d}  {e['status']}")


if __name__ == "__main__":
    main()
