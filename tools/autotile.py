"""RPG Maker 2000/2003 tile-ID -> chipset sub-tile resolution (offline).

SPDX-License-Identifier: GPL-3.0-or-later

The two lookup tables and the block-coordinate rules below are adapted from
EasyRPG Player (src/tilemap_layer.cpp, GPL-3.0-or-later, (c) EasyRPG
Project).  They describe the RPG Maker 2000/2003 autotile layout.  This file
re-expresses them in Python for an offline converter; nothing runs on the
Dreamcast except the resulting coordinates.

A 16x16 map cell is drawn as four 8x8 quarters.  For every cell we produce,
per animation frame, four (sub_x, sub_y) coordinates in 8x8 units on the
480x256 chipset (60 x 32 sub-tiles).
"""

N = -1
BLOCK_A_SUBTILES = [
    [[N, N], [N, N]], [[3, N], [N, N]], [[N, 3], [N, N]], [[3, 3], [N, N]],
    [[N, N], [N, 3]], [[3, N], [N, 3]], [[N, 3], [N, 3]], [[3, 3], [N, 3]],
    [[N, N], [3, N]], [[3, N], [3, N]], [[N, 3], [3, N]], [[3, 3], [3, N]],
    [[N, N], [3, 3]], [[3, N], [3, 3]], [[N, 3], [3, 3]], [[3, 3], [3, 3]],
    [[1, N], [1, N]], [[1, 3], [1, N]], [[1, N], [1, 3]], [[1, 3], [1, 3]],
    [[2, 2], [N, N]], [[2, 2], [N, 3]], [[2, 2], [3, N]], [[2, 2], [3, 3]],
    [[N, 1], [N, 1]], [[N, 1], [3, 1]], [[3, 1], [N, 1]], [[3, 1], [3, 1]],
    [[N, N], [2, 2]], [[3, N], [2, 2]], [[N, 3], [2, 2]], [[3, 3], [2, 2]],
    [[1, 1], [1, 1]], [[2, 2], [2, 2]], [[0, 2], [1, N]], [[0, 2], [1, 3]],
    [[2, 0], [N, 1]], [[2, 0], [3, 1]], [[N, 1], [2, 0]], [[3, 1], [2, 0]],
    [[1, N], [0, 2]], [[1, 3], [0, 2]], [[0, 0], [1, 1]], [[0, 2], [0, 2]],
    [[1, 1], [0, 0]], [[2, 0], [2, 0]], [[0, 0], [0, 0]],
]

# [subtile][row][col] -> (x, y) in 16px block units relative to the D block
BLOCK_D_SUBTILES = [
    [[(1, 2), (1, 2)], [(1, 2), (1, 2)]], [[(2, 0), (1, 2)], [(1, 2), (1, 2)]],
    [[(1, 2), (2, 0)], [(1, 2), (1, 2)]], [[(2, 0), (2, 0)], [(1, 2), (1, 2)]],
    [[(1, 2), (1, 2)], [(1, 2), (2, 0)]], [[(2, 0), (1, 2)], [(1, 2), (2, 0)]],
    [[(1, 2), (2, 0)], [(1, 2), (2, 0)]], [[(2, 0), (2, 0)], [(1, 2), (2, 0)]],
    [[(1, 2), (1, 2)], [(2, 0), (1, 2)]], [[(2, 0), (1, 2)], [(2, 0), (1, 2)]],
    [[(1, 2), (2, 0)], [(2, 0), (1, 2)]], [[(2, 0), (2, 0)], [(2, 0), (1, 2)]],
    [[(1, 2), (1, 2)], [(2, 0), (2, 0)]], [[(2, 0), (1, 2)], [(2, 0), (2, 0)]],
    [[(1, 2), (2, 0)], [(2, 0), (2, 0)]], [[(2, 0), (2, 0)], [(2, 0), (2, 0)]],
    [[(0, 2), (0, 2)], [(0, 2), (0, 2)]], [[(0, 2), (2, 0)], [(0, 2), (0, 2)]],
    [[(0, 2), (0, 2)], [(0, 2), (2, 0)]], [[(0, 2), (2, 0)], [(0, 2), (2, 0)]],
    [[(1, 1), (1, 1)], [(1, 1), (1, 1)]], [[(1, 1), (1, 1)], [(1, 1), (2, 0)]],
    [[(1, 1), (1, 1)], [(2, 0), (1, 1)]], [[(1, 1), (1, 1)], [(2, 0), (2, 0)]],
    [[(2, 2), (2, 2)], [(2, 2), (2, 2)]], [[(2, 2), (2, 2)], [(2, 0), (2, 2)]],
    [[(2, 0), (2, 2)], [(2, 2), (2, 2)]], [[(2, 0), (2, 2)], [(2, 0), (2, 2)]],
    [[(1, 3), (1, 3)], [(1, 3), (1, 3)]], [[(2, 0), (1, 3)], [(1, 3), (1, 3)]],
    [[(1, 3), (2, 0)], [(1, 3), (1, 3)]], [[(2, 0), (2, 0)], [(1, 3), (1, 3)]],
    [[(0, 2), (2, 2)], [(0, 2), (2, 2)]], [[(1, 1), (1, 1)], [(1, 3), (1, 3)]],
    [[(0, 1), (0, 1)], [(0, 1), (0, 1)]], [[(0, 1), (0, 1)], [(0, 1), (2, 0)]],
    [[(2, 1), (2, 1)], [(2, 1), (2, 1)]], [[(2, 1), (2, 1)], [(2, 0), (2, 1)]],
    [[(2, 3), (2, 3)], [(2, 3), (2, 3)]], [[(2, 0), (2, 3)], [(2, 3), (2, 3)]],
    [[(0, 3), (0, 3)], [(0, 3), (0, 3)]], [[(0, 3), (2, 0)], [(0, 3), (0, 3)]],
    [[(0, 1), (2, 1)], [(0, 1), (2, 1)]], [[(0, 1), (0, 1)], [(0, 3), (0, 3)]],
    [[(0, 3), (2, 3)], [(0, 3), (2, 3)]], [[(2, 1), (2, 1)], [(2, 3), (2, 3)]],
    [[(0, 1), (2, 1)], [(0, 3), (2, 3)]], [[(1, 2), (1, 2)], [(1, 2), (1, 2)]],
    [[(1, 2), (1, 2)], [(1, 2), (1, 2)]], [[(0, 0), (0, 0)], [(0, 0), (0, 0)]],
]
assert len(BLOCK_A_SUBTILES) == 47 and len(BLOCK_D_SUBTILES) == 50

BLOCK_C, BLOCK_D, BLOCK_E, BLOCK_F = 3000, 4000, 5000, 10000
EMPTY = (0xFF, 0xFF)  # sentinel quarter: draw nothing
FRAMES = 4            # stored animation frames (AB water uses 0..2, C uses 0..3)


def _quarters_from_blocks(q):
    """q[j][i] = (bx, by) block coords -> 8x8 sub-tile coords per quarter."""
    out = []
    for j in range(2):
        for i in range(2):
            bx, by = q[j][i]
            out.append((bx * 2 + i, by * 2 + j))
    return out


def resolve_ab(tile_id, frame):
    block = tile_id // 1000
    b_sub = (tile_id - block * 1000) // 50
    a_sub = tile_id - block * 1000 - b_sub * 50
    if b_sub >= 16 or a_sub >= 47:
        return None
    anim = frame % 3
    q = [[None, None], [None, None]]
    for j in range(2):
        for i in range(2):
            if BLOCK_A_SUBTILES[a_sub][j][i] != -1:
                continue
            t = (b_sub >> (j * 2 + i)) & 1
            if block == 2:
                t ^= 3
            q[j][i] = (anim, 4 + t)
    for j in range(2):
        for i in range(2):
            a = BLOCK_A_SUBTILES[a_sub][j][i]
            if a == -1:
                continue
            q[j][i] = (anim + (3 if block == 1 else 0), a)
    if b_sub != 0 and a_sub != 0:
        for j in range(2):
            for i in range(2):
                t = (b_sub >> (j * 2 + i)) & 1
                if block == 2:
                    t *= 2
                if t == 0:
                    continue
                q[j][i] = (anim, 4 + t)
    return _quarters_from_blocks(q)


def resolve_d(tile_id):
    block = (tile_id - BLOCK_D) // 50
    sub = tile_id - BLOCK_D - block * 50
    if not (0 <= block < 12 and 0 <= sub < 50):
        return None
    if block < 4:
        bx, by = (block % 2) * 3, 8 + (block // 2) * 4
    else:
        bx, by = 6 + (block % 2) * 3, ((block - 4) // 2) * 4
    q = [[(bx + BLOCK_D_SUBTILES[sub][j][i][0], by + BLOCK_D_SUBTILES[sub][j][i][1])
          for i in range(2)] for j in range(2)]
    return _quarters_from_blocks(q)


def _simple(col, row):
    return _quarters_from_blocks([[(col, row), (col, row)], [(col, row), (col, row)]])


def resolve_lower(tile_id, frame):
    """Return 4 quarters (sub_x, sub_y) or None for an invalid id."""
    if tile_id >= BLOCK_E:
        idx = tile_id - BLOCK_E
        if idx >= 144:
            return None
        if idx < 96:
            return _simple(12 + idx % 6, idx // 6)
        return _simple(18 + (idx - 96) % 6, (idx - 96) // 6)
    if tile_id >= BLOCK_D:
        return resolve_d(tile_id)
    if tile_id >= BLOCK_C:
        return _simple(3 + (tile_id - BLOCK_C) // 50, 4 + frame % 4)
    if tile_id >= 0:
        return resolve_ab(tile_id, frame)
    return None


def resolve_upper(tile_id):
    idx = tile_id - BLOCK_F
    if not 0 <= idx < 144:
        return None
    if idx < 48:
        return _simple(18 + idx % 6, 8 + idx // 6)
    return _simple(24 + (idx - 48) % 6, (idx - 48) // 6)


def lower_passability_index(tile_id):
    """Index into chipset.passable_data_lower (162 entries)."""
    if tile_id >= BLOCK_E:
        return 18 + (tile_id - BLOCK_E)
    if tile_id >= BLOCK_D:
        return 6 + (tile_id - BLOCK_D) // 50
    if tile_id >= BLOCK_C:
        return 3 + (tile_id - BLOCK_C) // 50
    return tile_id // 1000
