# Data formats

## Original (read by tools/lcf.py) — confirmed from the actual files

* **LCF container**: length-prefixed ASCII header (`LcfDataBase`, `LcfMapTree`,
  `LcfMapUnit`), then *chunks* `BER(id) BER(size) bytes`, a struct ends at id 0.
  `Array<T>` = `BER(count)` then `BER(index) struct` pairs. BER = big-endian
  7-bit groups, high bit = continue; negative values are 32-bit two's complement.
  Chunk IDs from EasyRPG liblcf `generator/csv/fields.csv` (MIT).
* **Event command list**: `BER code, BER indent, BER len + string, BER n, n×BER param`,
  terminated by four zero bytes.
* **Move route**: `BER code` + params only for codes 32/33 (1 int), 34 (string,int),
  35 (string, 3 ints).
* **XYZ image**: `"XYZ1" u16 w u16 h` + zlib(768-byte RGB palette + w·h indices).
  Index 0 is the transparent colour for charsets/chipsets/keyed pictures.
* **Tile IDs**: 0–2999 water autotiles A/B, 3000–3999 animated (3 × 50),
  4000–4999 terrain autotiles (12 × 50), 5000–5143 lower tiles, 10000–10143 upper.
  Passability index: `id/1000`, `3+(id-3000)/50`, `6+(id-4000)/50`, `18+id-5000`;
  bits Down 1, Left 2, Right 4, Up 8, Above 16, Wall 32, Counter 64.

## Converted (written by tools/convert.py, read by src/resources/gamedata.c)

All little-endian. Strings: `u8 length + bytes` (cp1252, ≤ 255). Command lists
are 4-byte aligned relative to the file start:
`u32 count`, then per command `u16 code, u16 indent, u16 nparams, u16 strlen,
i32 params[n], bytes, pad to 4`.

| File | Layout |
|---|---|
| `game.dat` | `"OSGD" u16 ver=1`, `u16 nswitches, u16 nvars`, 4 strings (system, system2, title, gameover), 4 × sound (string, vol, tempo, pan), title music, 3 × u8 (transitions, stretch), actors, items (name, type, switch, description), skills, `u8 n` menu commands, 7 menu terms, chipsets (file, anim type/speed, 162 + 144 passability bytes), common events (name, trigger, switch flag, switch id, command list), map tree (id, name, parent, music type, music, converted flag), start map/x/y, asset manifest (`u8 category, name, path`) |
| `map/NNNN.map` | `"OSMP" u16 ver` (2; ver 1 still loads), w, h, chipset, scroll, parallax (flag, name, loop/auto/speed), `u16 lower[w·h]`, `u16 upper[w·h]`, **resolved lower quarters** `w·h × 4 frames × 4 quarters × (x,y)` in 8-px chipset units (`0xFF` = empty), upper quarters `w·h × 4 × (x,y)`, events (id, x, y, name, pages: condition (flags, switch A/B, variable, value, item, compare op, **actor id** since ver 2), graphic, movement, trigger, layer, move route, command list) |
| `anim.dat` | `"OSAN" u16 1 u16 n` × (`u16 id`, string file, `u8 scope, u8 position, u8 large`, `u16 nframes` × (`u8 ncells` × (`u8 valid, u8 cell, i16 x, i16 y, u16 zoom, u8 tone r g b gray, u8 transparency`)), `u16 ntimings` × (`u16 frame`, string SE, `u8 vol tempo pan, u8 flash scope, u8 r g b power (0-31), u8 shake`)) — the database's battle animations |
| `img/CC/NNN.img` | `"OSI1" u16 w u16 h`, `u16 palette[256]` (RGB555 + bit 15), `u8 pixels[w·h]` |
| `se/NNN.wav` | RIFF WAVE, Yamaha ADPCM (format 0x20), mono, 22 050 Hz |
| `bgm/NNN.wav` | RIFF WAVE, 16-bit PCM, stereo, 22 050 Hz (streamed) |
| `font.bin` | 256 glyphs × 12 rows, bit 7 = leftmost of 6 columns (X11 misc-fixed 6×12, public domain) |
| `vfs_base.dat` | `"OSVF" u16 ver u16 n`, per entry `u8 kind (0 file, 1 dir), u8 type (0 text, 1 image, 2 binary), string path, u32 len, bytes` |
| `debug.dat` | `"OSDB" u16 ver=1`, `u16 nrooms` × (`u16 map, u16 x, u16 y, u8 dir, u8 source` 0 teleport/1 start/2 centre, string name), `u16 ncut` × (`u16 map, u16 event, u8 page, u8 cond_flags, u16 switch_a, u16 switch_b, u16 var, i32 value, u8 compare_op, u16 item`, string label) — written by `tools/mkdebug.py` |
| `manifest.json` | human-readable manifest: asset list, maps, missing references, manifest offset |

Assets are renamed to numbered 8.3-safe paths (`img/04/012.img`) so ISO9660
name limits and spaces/apostrophes in the original names cannot cause problems;
the runtime finds them by category + case-insensitive original name.

## Save file (VMU)

`vmu_pkg` header (icon, "OneShot") + payload:
`"OSSV" u16 version=1 u16 header_len=24 u32 sequence u32 payload_len u32 crc32 u32 reserved`,
then TLV payload `"OSPL" u16 1` + tags: 1 switches bitset (64 B), 2 variables
(512 × i32), 3 item ids, 4 note ids, 5 meta (ending, player name, name-known,
wallpaper, close-enabled, launches), 6 VFS persistent overlay (`"OSVF"` format
with kind 2 = tombstone), 7 flags. Unknown tags are skipped (forward compatible
within version 1); a newer major version is reported and never overwritten.
