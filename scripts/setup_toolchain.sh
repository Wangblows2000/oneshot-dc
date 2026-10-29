#!/usr/bin/env bash
# Install the Dreamcast toolchain on Debian/Ubuntu (native Linux or WSL2).
# This is exactly the sequence used to build and test this project.
#
#   scripts/setup_toolchain.sh            # asks before using sudo
#
# What it does (each step is skipped if already done):
#   1. apt packages needed to build GCC/KOS and to convert assets   (needs sudo)
#   2. clones KallistiOS + kos-ports into $DC_TOOLCHAINS (default /opt/toolchains/dc)
#   3. builds the sh-elf cross compiler with KOS's kos-chain (~1 hour)
#   4. builds KallistiOS and the libwav port
#   5. builds mkdcdisc (disc image tool) into tools/bin/
# Nothing outside $DC_TOOLCHAINS, tools/bin and the apt packages is changed.
set -euo pipefail
: "${DC_TOOLCHAINS:=/opt/toolchains/dc}"
HERE="$(cd "$(dirname "$0")/.." && pwd)"
JOBS="${JOBS:-$(nproc)}"

echo "== 1/5 apt packages"
PKGS="build-essential git python3 python3-pil texinfo libgmp-dev libmpfr-dev libmpc-dev libjpeg-dev libpng-dev libelf-dev wget gettext bison flex ffmpeg xfonts-base pcf2bdf meson ninja-build pkg-config libisofs-dev"
missing=$(for p in $PKGS; do dpkg -s "$p" >/dev/null 2>&1 || echo "$p"; done)
if [ -n "$missing" ]; then
  echo "Need to install: $missing"
  read -r -p "Run 'sudo apt-get install' for these now? [y/N] " a
  [ "$a" = y ] || { echo "Aborted - install them yourself and re-run."; exit 1; }
  sudo apt-get update && sudo apt-get install -y $missing
fi

echo "== 2/5 sources in $DC_TOOLCHAINS"
if [ ! -w "$(dirname "$DC_TOOLCHAINS")" ] && [ ! -d "$DC_TOOLCHAINS" ]; then
  echo "Creating $DC_TOOLCHAINS needs sudo (it will be owned by $USER)."
  sudo mkdir -p "$DC_TOOLCHAINS" && sudo chown "$USER" "$DC_TOOLCHAINS"
fi
mkdir -p "$DC_TOOLCHAINS"
cd "$DC_TOOLCHAINS"
[ -d kos ] || git clone --depth 1 https://github.com/KallistiOS/KallistiOS.git kos
[ -d kos-ports ] || git clone --depth 1 https://github.com/KallistiOS/kos-ports.git kos-ports

echo "== 3/5 sh-elf toolchain (kos-chain)"
if [ ! -x "$DC_TOOLCHAINS/sh-elf/bin/sh-elf-gcc" ]; then
  cd "$DC_TOOLCHAINS/kos/utils/kos-chain"
  cp -n Makefile.dreamcast.cfg Makefile.cfg
  sed -i "s/^makejobs=.*/makejobs=$JOBS/; s/^enable_objc=1/enable_objc=0/; s/^enable_objcpp=1/enable_objcpp=0/" Makefile.cfg
  make
fi

echo "== 4/5 KallistiOS + libwav"
cd "$DC_TOOLCHAINS/kos"
[ -f environ.sh ] || cp doc/environ.sh.sample environ.sh
bash -c ". '$HERE/scripts/env.sh' && make -j$JOBS"
bash -c ". '$HERE/scripts/env.sh' && cd '$DC_TOOLCHAINS/kos-ports/libwav' && make install"

echo "== 5/5 mkdcdisc"
if [ ! -x "$HERE/tools/bin/mkdcdisc" ]; then
  mkdir -p "$HERE/tools/bin" "$HERE/build/ext"
  cd "$HERE/build/ext"
  [ -d mkdcdisc ] || git clone --depth 1 https://gitlab.com/simulant/mkdcdisc.git
  cd mkdcdisc && meson setup builddir --wipe >/dev/null 2>&1 || meson setup builddir
  meson compile -C builddir
  cp builddir/mkdcdisc "$HERE/tools/bin/"
fi
echo
"$HERE/scripts/check_env.sh" || true
