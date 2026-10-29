# OneShot (2014) for Sega Dreamcast — fan port

A from-scratch **RPG Maker 2003-compatible runtime for the Sega Dreamcast** that
plays the original, free 2014 *OneShot* using your own copy of its data files,
plus a sandboxed **virtual computer** that stands in for the Windows desktop
OneShot talks to ("the document outside the game", the wallpaper, message
boxes, closing the game).

> Status: **playable vertical slice** — boots on an emulated Dreamcast (Flycast),
> renders the real maps/sprites/pictures, runs the original event scripts, and the
> original "document outside the game" safe puzzle is solvable end-to-end through
> the virtual computer. Not yet tested on real hardware. See
> [docs/PROGRESS.md](docs/PROGRESS.md) for exactly what works and what does not.

## What is in this repository

| Path | What it is |
|---|---|
| `src/` | The Dreamcast runtime (C11). Portable core + `platform/platform_dc.c` (KallistiOS) and `platform/platform_host.c` (headless Linux build for tests) |
| `tools/` | Python converters: `lcf.py` (reads RPG Maker 2003 `.ldb/.lmt/.lmu`), `convert.py`, `mkvfs.py`, `validate.py`, coverage/inventory reports |
| `tests/` | Unit tests (VFS, saves, persistence, bridge) + scripted gameplay tests that run on the real data |
| `scripts/` | `setup_toolchain.sh`, `check_env.sh`, `env.sh`, test runners, Flycast driver |
| `docs/` | Reconnaissance report, architecture, data formats, compatibility registry, test results, progress log |

**Not in this repository:** the original game, any converted game data, and any
disc image. You must supply your own copy of the 2014 freeware release and build
the disc image yourself (see below).

## What you need

* **Your own copy of OneShot (2014, RPG Maker 2003 version).** The folder that
  contains `RPG_RT.ldb`. It is only ever read, never modified.
* **A Dreamcast emulator** to run the result: [Flycast](https://github.com/flyinghead/flycast/releases)
  (free, no BIOS files needed). A real Dreamcast works too, but this has not been tested.
* **A Linux environment** to build. The Dreamcast toolchain (KallistiOS + an
  SH-4 GCC) is Linux-first, so on Windows you build inside **WSL2**.

## Building on Linux

Tested on Debian/Ubuntu (the toolchain script uses `apt`). Other distributions
need the equivalent packages installed by hand — see the package list in
`scripts/setup_toolchain.sh`.

```bash
git clone https://github.com/Wangblows2000/oneshot-dc.git
cd oneshot-dc

# 1. One-time toolchain setup: apt packages, KallistiOS, kos-ports, the SH-4
#    GCC cross compiler (~1 hour), libwav, and mkdcdisc. Asks before using sudo.
scripts/setup_toolchain.sh

# 2. Check that everything is present (installs nothing).
scripts/check_env.sh

# 3. Convert YOUR copy of the game (point GAME at the folder containing RPG_RT.ldb).
make data GAME=/path/to/OneShot

# 4. Build the Dreamcast executable -> build/oneshot.elf
make build

# 5. Build a bootable disc image for emulators -> build/oneshot.cdi
make cdi

# 6. (optional) Run the host-side unit tests and scripted gameplay tests.
make test GAME=/path/to/OneShot
```

Then open `build/oneshot.cdi` in Flycast, or run `make run FLYCAST=/path/to/flycast`.
For a disc to burn to a CD-R for a real console, use `make cdi-burn`
(`build/oneshot-burn.cdi`).

`make help` lists all targets. Every path can be overridden on the command line
or in the environment: `GAME=`, `DATA=`, `MKDCDISC=`, `FLYCAST=`, `DC_TOOLCHAINS=`
(default `/opt/toolchains/dc`).

## Building on Windows (via WSL2)

The Dreamcast toolchain does not build natively on Windows with this project's
scripts; a native Windows (MSYS2/MinGW) setup has **not** been tried here. Use WSL2:

1. In an **administrator PowerShell**, install WSL2 with Ubuntu, then reboot:
   ```powershell
   wsl --install -d Ubuntu
   ```
2. Install [Flycast for Windows](https://github.com/flyinghead/flycast/releases)
   (download the `.zip`, extract it, run `flycast.exe`).
3. Open the **Ubuntu** app and follow the Linux steps above. Windows drives are
   under `/mnt/c`, so your game folder is reachable as, for example,
   `/mnt/c/Users/<you>/Desktop/OneShot`:
   ```bash
   git clone https://github.com/Wangblows2000/oneshot-dc.git
   cd oneshot-dc
   scripts/setup_toolchain.sh
   scripts/check_env.sh
   make data GAME="/mnt/c/Users/<you>/Desktop/OneShot"
   make build
   make cdi
   ```
   Cloning into a Windows folder (e.g. `cd /mnt/c/Users/<you>/Desktop` first) makes
   the result easy to find from Windows, but builds are noticeably faster inside
   the WSL filesystem (your Linux home directory).
4. Open the finished disc in Flycast from Windows. From inside WSL its location is
   `build/oneshot.cdi`; from Windows, if the repo lives in the WSL filesystem, browse to
   `\\wsl$\Ubuntu\home\<you>\oneshot-dc\build\oneshot.cdi`.

## Troubleshooting

* **`cannot find entry symbol start` / ELF is only ~25 KB** — the current
  KallistiOS master forgets the linker script in `KOS_LDFLAGS`. Always
  `source scripts/env.sh` (the Makefile does) — it contains the workaround.
* **`Original game data not found`** — pass `GAME=/path/to/the/folder/with/RPG_RT.ldb`.
* **Black screen in Flycast** — make sure you opened the `.cdi`, not the `.elf`.
* **Font missing** — `sudo apt install xfonts-base pcf2bdf`.
* **`make hello`** (a minimal toolchain smoke test) needs a `src/hello/Makefile`
  that is not part of this repository yet; the normal `make build` does not need it.

## Controls

Flycast's default keyboard mapping is shown in the second column.

| Dreamcast | Keyboard in Flycast | In the game |
|---|---|---|
| D-pad / stick | arrow keys | move, menus |
| A | `X` | talk / confirm / advance text |
| B | `C` | cancel / open the menu (Items, Equipment, Notes, Quit) |
| START | `Enter` | open the **virtual computer** (the "outside of the game") |
| Y | `D` | deselect a held item (OneShot's Shift key); in the computer: hold + D-pad to move a window |
| X | `S` | open fast travel (OneShot's number keys) |
| L / R triggers | `F` / `V` | switch between computer windows |

### Using items (2014 rules)

Open the menu with **B**, choose **Items** and select an item: Niko now *holds*
it (its icon appears bottom-right). Press **A** on something to use it there.
Select a second item while holding one to **combine** them; select the held
item again (or press **Y**) to put it away.

### Saving manually (memory card)

OneShot normally saves when Niko goes to bed. On the virtual computer, open
**Memory Card** and press **A** ("A: Save now"): this runs the game's own save
(the same as a bed), writes it to the VMU and tells you how many blocks it
used, or why it could not save (no card / not enough free blocks). A save uses
about 6 blocks per copy; two copies are kept, so leave at least 12 blocks free.
A VMU is optional — without one, the game warns you and keeps progress until the
console is switched off.

### Hidden Debug Menu

On the virtual computer's icon screen (no window open) press **Up, Up, Down,
Down**, then **hold L + R together** for about half a second. A **Debug Menu**
icon appears (until the console is switched off). Its window has three pages,
switched with **R**: *Rooms* (A = go there), *Cutscenes* (A = play; **B** stops it
and returns to the desktop; when it ends you are back in the Debug Menu), and
*Music* (A = play; **B** or closing the window stops it). Watching a cutscene
never changes your progress or your memory-card save.

## Legal

This is an unofficial fan project, not affiliated with the OneShot developers or
Sega. The engine source is **GPL-3.0-or-later** (it adapts the RPG Maker
autotile tables from EasyRPG Player, also GPL-3.0); see [LICENSE](LICENSE).
**No OneShot content is included in the source**: the converter reads your own
legally obtained copy of the 2014 freeware release. A disc image you generate
contains converted game assets — keep it for personal use and do not
redistribute it. Distributing the engine does not grant any right to distribute
the game.
