# SPDX-License-Identifier: GPL-3.0-or-later
# OneShot (2014) -> Sega Dreamcast port.  Top-level build.
#
#   make help            list targets
#   make check-env       verify toolchain / tools / game data
#   make data            convert the original game files (needs GAME=...)
#   make build           build the Dreamcast executable (build/oneshot.elf)
#   make cdi             bootable disc image for emulators (build/oneshot.cdi)
#   make cdi-burn        padded disc image for burning a CD-R
#   make test            host unit tests + scripted gameplay tests
#   make test-doors      walk into every door/stairs/elevator teleport (~30 min)
#   make run             boot the CDI in Flycast (if FLYCAST is set / found)
#   make clean
#
# All paths are configurable on the command line or in the environment.

GAME      ?= assets/source_readonly/oneshot
DATA      ?= build/cdroot/data
MKDCDISC  ?= $(shell command -v mkdcdisc 2>/dev/null || echo tools/bin/mkdcdisc)
FLYCAST   ?= $(shell command -v flycast 2>/dev/null)
PYTHON    ?= python3
ENV        = . scripts/env.sh &&

SRC = src/util.c src/vfs/vfs.c src/save/save.c src/save/persist.c src/resources/gamedata.c \
      src/renderer/gfx.c src/game/game.c src/game/map.c src/game/transition.c src/game/banim.c src/events/interp.c \
      src/events/bridge.c src/dialogue/message.c src/audio/audio.c src/ui/ui.c src/desktop/desktop.c src/desktop/debugmenu.c \
      src/main.c src/platform/platform_dc.c

.PHONY: help check-env data build cdi cdi-burn test test-unit test-play run clean distclean hello

help:
	@sed -n '3,15p' $(firstword $(MAKEFILE_LIST))

check-env:
	@scripts/check_env.sh

data: $(DATA)/game.dat

$(DATA)/game.dat: tools/convert.py tools/lcf.py tools/autotile.py tools/mkvfs.py tools/mkdebug.py
	@test -f "$(GAME)/RPG_RT.ldb" || { echo "Original game data not found in GAME=$(GAME)"; exit 1; }
	$(PYTHON) -I tools/convert.py --game "$(GAME)" --out "$(DATA)"
	$(PYTHON) -I tools/mkvfs.py --game "$(GAME)" --out "$(DATA)"
	$(PYTHON) -I tools/mkdebug.py --game "$(GAME)" --out "$(DATA)"
	$(PYTHON) -I tools/validate.py "$(DATA)"

build: build/oneshot.elf

build/oneshot.elf: $(SRC) $(wildcard src/*.h src/*/*.h)
	@mkdir -p build/obj
	$(ENV) kos-cc -std=gnu11 -O2 -Wall -Wno-unused-parameter $(SRC) -o $@ -lwav
	@echo "Built $@" && ls -la $@

build/oneshot.cdi: build/oneshot.elf $(DATA)/game.dat
	$(MKDCDISC) -e build/oneshot.elf -d $(DATA) -o $@ -n "ONESHOT DC" -a "OneShot DC fan port" -N --allow-overwrite --no-banner

cdi: build/oneshot.cdi

cdi-burn: build/oneshot.elf $(DATA)/game.dat
	$(MKDCDISC) -e build/oneshot.elf -d $(DATA) -o build/oneshot-burn.cdi -n "ONESHOT DC" -a "OneShot DC fan port" --allow-overwrite --no-banner

hello:
	$(ENV) $(MAKE) -C src/hello

test: test-unit test-play

test-unit:
	$(MAKE) -f Makefile.host build/host/unit_tests
	./build/host/unit_tests $(DATA)

test-play: test-gen
	$(MAKE) -f Makefile.host build/host/host_runner
	scripts/run_play_tests.sh $(DATA)

# Reports + test scripts derived from YOUR game data: kept under build/ (never committed)
test-gen:
	$(PYTHON) -I tools/item_inventory.py "$(GAME)" --json build/generated/item_inventory.json > build/generated_items.txt
	$(PYTHON) -I tools/mkdebug.py --game "$(GAME)" --out build/generated --json build/generated/debug_index.json
	$(PYTHON) -I tests/gen/gen_combinations.py
	$(PYTHON) -I tests/gen/gen_item_puzzles.py
	$(PYTHON) -I tests/gen/gen_debug_menu.py
	$(PYTHON) -I tests/gen/gen_alula.py "$(GAME)"
	$(PYTHON) -I tests/gen/gen_elevator.py

# Every touch/A-button teleport (doors, stairs, elevators) walked into from each side (~30 min)
test-doors:
	$(MAKE) -f Makefile.host build/host/door_sweep
	./build/host/door_sweep $(DATA) --tsv build/logs/door_sweep.tsv | tee build/logs/door_sweep.log | tail -1

run: build/oneshot.cdi
	@test -n "$(FLYCAST)" || { echo "Set FLYCAST=/path/to/flycast"; exit 1; }
	$(FLYCAST) build/oneshot.cdi

clean:
	rm -rf build/obj build/oneshot.elf build/oneshot.cdi build/oneshot-burn.cdi build/host
	-$(ENV) $(MAKE) -C src/hello clean

distclean: clean
	rm -rf $(DATA)
