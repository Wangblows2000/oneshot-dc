#!/usr/bin/env python3
"""List the files on a Dreamcast VMU image (e.g. Flycast's vmu_save_A1.bin) with the
blocks each one occupies - used to MEASURE save sizes on the emulated card.
Usage: vmu_blocks.py VMU_IMAGE"""
import struct, sys
d = open(sys.argv[1], 'rb').read()
if len(d) != 128 * 1024:
    sys.exit(f"not a 128 KB VMU image ({len(d)} bytes)")
root = d[255 * 512:256 * 512]
fat_blk, fat_sz, dir_blk, dir_sz = struct.unpack_from('<HHHH', root, 0x46)
user_blocks = struct.unpack_from('<H', root, 0x50)[0] or 200
fat = struct.unpack_from('<256H', d, fat_blk * 512)
free = sum(1 for i in range(user_blocks) if fat[i] == 0xFFFC)
total = 0
print(f"{'file':14} type  blocks  bytes")
for b in range(dir_blk, dir_blk - dir_sz, -1):
    blk = d[b * 512:(b + 1) * 512]
    for e in range(16):
        ent = blk[e * 32:(e + 1) * 32]
        if ent[0] not in (0x33, 0xCC):
            continue
        name = ent[4:16].decode('ascii', 'replace').rstrip('\0 ')
        size = struct.unpack_from('<H', ent, 0x18)[0]
        total += size
        print(f"{name:14} {'data' if ent[0] == 0x33 else 'game'}  {size:5}  {size * 512:6}")
print(f"total {total} blocks ({total * 512} bytes) in files; {free} of {user_blocks} user blocks free")
