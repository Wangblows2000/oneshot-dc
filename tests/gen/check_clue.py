#!/usr/bin/env python3
"""Check the generated clue wallpaper against the Alula floor: same grid shape as the
panels (columns x rows) and the same lit cells in the same orientation.
Reads build/cdroot/data (converted) and build/generated/alula_grid.json."""
import json, os, struct, sys
root = os.path.join(os.path.dirname(__file__), '..', '..')
data = sys.argv[1] if len(sys.argv) > 1 else os.path.join(root, 'build/cdroot/data')
grid = json.load(open(os.path.join(root, 'build/generated/alula_grid.json')))
mf = json.load(open(os.path.join(data, 'manifest.json')))
clue = next(a for a in mf['assets'] if a['name'] == '~wall_clue')
g = clue['grid']
d = open(os.path.join(data, clue['path']), 'rb').read()
w, h = struct.unpack_from('<HH', d, 4)
px = d[8 + 512:]
fails = 0
if (g['cols'], g['rows']) != (grid['cols'], grid['rows']):
    print(f"FAIL grid shape: wallpaper {g['cols']}x{g['rows']} vs floor {grid['cols']}x{grid['rows']}"); fails += 1
lit = {tuple(x) for x in grid['lit']}
for r in range(grid['rows']):
    row = ''
    for c in range(grid['cols']):
        cx = g['gx'] + c * g['cell'] + g['cell'] // 2; cy = g['gy'] + r * g['cell'] + g['cell'] // 2
        on = px[cy * w + cx] == 33
        row += '#' if on else '.'
        if on != ((c, r) in lit): fails += 1
    print(f"  wallpaper row {r}: {row}   floor row {r}: " + ''.join('#' if (c, r) in lit else '.' for c in range(grid['cols'])))
print(f"clue wallpaper vs floor ({grid['cols']} columns x {grid['rows']} rows): {'PASS' if not fails else 'FAIL'} ({fails} mismatches)")
sys.exit(1 if fails else 0)
