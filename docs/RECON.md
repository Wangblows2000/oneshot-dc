# Milestone 0 — Reconnaissance and feasibility

Everything below was verified against the actual files on 2026-10-21; items
marked *inferred* are conclusions from how the game data uses something, not
from inspecting the original native code.

## 1. Environment actually found

| Item | Finding |
|---|---|
| Your PC | Windows (x64), reached read-only through the Claude desktop app; no remote shell |
| Build machine | Ubuntu 24.04 container, x86-64, 2 cores, 7.8 GB RAM, root, internet via proxy (GitHub `git clone` and release downloads work; GitHub web/API pages are blocked; GitLab works) |
| Pre-installed | git, python3 3.13, make, cmake, gcc 13.3 |
| Installed during this session | build deps (texinfo, gmp/mpfr/mpc, …), ffmpeg, xfonts-base + pcf2bdf, upx-ucl (analysis), libisofs, meson/ninja, Xvfb tools, xdotool |
| Dreamcast toolchain | **built from source**: KallistiOS master `5f834c1` (2026-10-20), kos-chain "stable" profile → binutils 2.45.1, **GCC 15.2.0**, newlib 4.6.0; libwav (kos-ports); mkdcdisc 0.0.4 (GitLab); Flycast 2.5 Linux AppImage for testing |

## 2. The game data

Folder: `oneshot original rpgmaker release/` (321 files).

| File | Evidence | Conclusion |
|---|---|---|
| `RPG_RT.ldb` | header `LcfDataBase`; System chunk `ldb_id = 2003` | **RPG Maker 2003** database |
| `RPG_RT.lmt` | header `LcfMapTree` | map tree, 66 entries, start map 1 |
| `Map0001-0066.lmu` (65 files, 0051 absent) | header `LcfMapUnit` | all 65 parse with 0 errors |
| `*.xyz` (all images) | `XYZ1` + u16 w/h + zlib(768-byte palette + 8-bit pixels) | all 211 decode; chipsets 480×256, charsets 288×256, facesets 192×192, system 160×80 |
| `Music/*.ogg` | 19 Ogg Vorbis tracks, 19.2 min total | converted with ffmpeg |
| `Sound/*.wav` | 75 PCM WAVs incl. two 132-byte dummies `_func.wav`, `_init.wav` | dummies are bridge markers (see §3) |
| `RPG_RT.ini` | `GameTitle=Oneshot`, `FullPackageFlag=1` | no RTP needed |
| `RPG_RT.exe` | UPX-packed (unpacked a scratch copy: 3.96 MB); imports `libgcj-13.dll`, `SHELL32`, `SHLWAPI`, `USER32`; no RPG Maker/Delphi signatures; contains further obfuscated data | **a modified/replaced runtime**, not a stock RPG_RT. Not reverse engineered further |

Text encoding: Windows-1252 (English, no Shift-JIS lead bytes).
Event commands used: **55 distinct codes, all standard RPG Maker 2003**
(`docs/generated/command_coverage.json`). No Comment-based patches, no DynRPG,
no Harmony.dll, no Maniac Patch parameters.

## 3. The OneShot-specific host mechanism (verified from data)

Common events 1–14 are named `func_GuessName`, `func_SetName`,
`func_SetNameEntry`, `func_ShakeWindow`, `func_SetWallpaper`, `func_MessageBox`,
`func_LeaveWindow`, `func_Save`, `func_WriteItem`, `func_Load`, `func_ReadItem`,
`func_Document`, `func_End`, `func_SetCloseEnabled`. Each does exactly:

```
ControlVars  var1 ("func") = N          (N = 0..13)
ChangeSystemSFX cursor = "_func"        <- the modified EXE intercepts this
ChangeSystemSFX cursor = "menu_cursor"
```

Callers put arguments in variables 2–5 (`arg1..arg4`) and read variable 6
(`return`). Map 1 ("INIT") performs a handshake with `_init` after writing a
signature (all switches off; switches 1–20 set to a fixed on/off pattern;
vars 1–4 = 8008135, 3133710, 1010101, 1234567). If var1 is still 8008135
afterwards it shows the `error` picture — i.e. the game refuses to run on a
stock RPG_RT. *Inferred*: the signature lets the external code find the
switch/variable arrays in memory; it must then clear it.

Message text contains the literal placeholder `_PlayerName_xxxxxxxxxxxxxxxxxxxx`
which the EXE replaces with the player's name; actor 2 is named `TPlayerNameT`.
The full call-site inventory is `docs/generated/compatibility_inventory.json`.

## 4. mkxp-oneshot (https://github.com/elizagamedev/mkxp-oneshot, commit 87819a0, GPL-2.0)

Cloned read-only for study (114 MB, not copied into this project).

* **What it is**: a fork of mkxp, an open implementation of RGSS (the Ruby
  scripting runtime of **RPG Maker XP/VX/VX Ace**), built for the **2016
  commercial OneShot**. It needs Ruby (MRI), SDL2, OpenGL, OpenAL, PhysFS,
  Boost, Steamworks (via steamshim), GTK/xfconf on Linux, and a host OS with a
  writable home directory.
* **Relevant files**: `src/oneshot.cpp` (user/Documents paths via
  `getenv("HOME")`/XDG and Win32, locale, `SDL_ShowMessageBox` Yes/No with
  Yes=1/No=0, window shake, exit control), `binding-mri/oneshot-binding.cpp`
  (Ruby module `Oneshot`: `msgbox`, `set_yes_no`, `textinput`, `allow_exit`,
  `exiting`, `shake`, `crc32`…), `journal/` (separate helper application),
  `src/steam.cpp`, `src/filesystem.cpp`.
* **Why it cannot be used directly**: the 2014 game is RPG Maker **2003**
  (LCF binary data, event commands) — mkxp runs RGSS Ruby scripts and cannot
  load `.ldb/.lmu` at all. Its dependencies (Ruby interpreter, OpenGL, SDL2,
  OpenAL, Boost) are far beyond 16 MB RAM / PowerVR2 / no-OS.
* **What was useful**: confirmation of the *conventions* (Yes=1/No=0 message
  boxes; Documents folder as the "outside" location; controlled exit), which
  match how the 2014 events test `return`.

| mkxp-oneshot behaviour | Reuse? | Dreamcast approach |
|---|---|---|
| RGSS/Ruby runtime | no (wrong engine) | own RPG Maker 2003 interpreter |
| `msgbox` / Yes-No | semantics only | `ui_msgbox_open` modal dialog |
| Documents folder writes | semantics only | VFS `/Documents` (persistent overlay) |
| window shake / exit control | semantics only | screen shake; simulated close/relaunch |
| journal helper app | deferred | 2016-only; not used by the 2014 data |
| Steam, achievements | no | n/a |

## 5. Strategy decision

| Option | Effort | Data compatibility | DC fit | Licence | Verdict |
|---|---|---|---|---|---|
| A. Port EasyRPG Player | high (C++17, liblcf, pixman, fmt, SDL2-class backend) | excellent | poor/unknown on 16 MB RAM, no maintained DC backend | GPL-3 | rejected for now |
| A'. Port mkxp-oneshot | very high | **none** (wrong engine) | very poor | GPL-2 | rejected |
| **B. Small custom RM2003-compatible runtime + offline converter** | medium | high for the commands OneShot uses (all 55 handled) | good: precomputed tiles, compact binary data, PVR quads | own code GPL-3 | **chosen** |
| C. Bespoke recreation | low at first | poor | good | — | rejected (would not scale to the real game) |

Option B keeps all heavy parsing on the PC (Python), and the Dreamcast only
reads small pre-validated binary files. EasyRPG/liblcf were used as
*documentation* of the formats (chunk IDs, autotile tables), with attribution.
