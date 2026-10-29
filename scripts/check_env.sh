#!/usr/bin/env bash
# Report which prerequisites are present.  Never installs anything.
# Exit code 0 = everything needed for a Dreamcast build is present.
set -u
ok=0
have() { command -v "$1" >/dev/null 2>&1; }
row() { printf '  %-28s %s\n' "$1" "$2"; }
echo "OneShot DC environment check"
echo "Host: $(uname -srm)"
grep -qi microsoft /proc/version 2>/dev/null && echo "  (running inside WSL)"
echo
echo "Host tools:"
for t in git python3 make gcc ffmpeg pcf2bdf; do
  if have "$t"; then row "$t" "OK ($(command -v "$t"))"; else row "$t" "MISSING"; ok=1; fi
done
python3 -c "import PIL" 2>/dev/null && row "python3 Pillow (optional)" "OK" || row "python3 Pillow (optional)" "missing (only tools/preview_map.py needs it)"
[ -f /usr/share/fonts/X11/misc/6x12-ISO8859-1.pcf.gz ] && row "X11 6x12 font" "OK" || { row "X11 6x12 font" "MISSING (apt install xfonts-base)"; ok=1; }
echo
echo "Dreamcast toolchain:"
: "${DC_TOOLCHAINS:=/opt/toolchains/dc}"
if [ -x "$DC_TOOLCHAINS/sh-elf/bin/sh-elf-gcc" ]; then row "sh-elf-gcc" "OK ($("$DC_TOOLCHAINS/sh-elf/bin/sh-elf-gcc" -dumpversion))"; else row "sh-elf-gcc" "MISSING (scripts/setup_toolchain.sh)"; ok=1; fi
[ -f "$DC_TOOLCHAINS/kos/lib/dreamcast/libkallisti.a" ] && row "KallistiOS" "OK" || { row "KallistiOS" "MISSING"; ok=1; }
[ -f "$DC_TOOLCHAINS/kos-ports/lib/libwav.a" ] && row "kos-ports libwav" "OK" || { row "kos-ports libwav" "MISSING"; ok=1; }
MK="${MKDCDISC:-$(command -v mkdcdisc || echo tools/bin/mkdcdisc)}"
[ -x "$MK" ] && row "mkdcdisc" "OK ($MK)" || { row "mkdcdisc" "MISSING (scripts/setup_toolchain.sh builds it)"; ok=1; }
FL="${FLYCAST:-$(command -v flycast || true)}"
[ -n "$FL" ] && [ -x "$FL" ] && row "Flycast (optional)" "OK ($FL)" || row "Flycast (optional)" "not found - install it on your PC to play the .cdi"
echo
echo "Game data:"
G="${GAME:-assets/source_readonly/oneshot}"
if [ -f "$G/RPG_RT.ldb" ]; then row "original OneShot files" "OK ($G)"; else row "original OneShot files" "MISSING - set GAME=/path/to/game"; ok=1; fi
[ -f build/cdroot/data/game.dat ] && row "converted data" "OK" || row "converted data" "not built yet (make data)"
echo
[ $ok = 0 ] && echo "RESULT: ready" || echo "RESULT: something is missing (see above)"
exit $ok
