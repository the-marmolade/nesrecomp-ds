#!/bin/bash
#
# port.sh — take a NES ROM to a bootable .nds in one command.
#
#   bash tools/port.sh /path/to/rom.nes donkey-kong dk
#
# Arguments:
#   1  path to the .nes ROM (yours, of a game you own)
#   2  short project name, used for the output directory and code prefix
#   3  optional GAME id matching an entry in source/game_config.c (default: smb)
#
# What it does:
#   1. builds the nesrecomp recompiler if it isn't built
#   2. writes a minimal game.toml
#   3. recompiles the ROM's 6502 into C
#   4. builds this runner against that C
#
# The ROM itself is never copied into the output; it is read at runtime from
# the SD card, or embedded via nitrofiles/ if you put it there yourself.
set -eu

ROOT="$(cd "$(dirname "$0")/.." && pwd)"
ROM="${1:-}"
NAME="${2:-}"
GAME="${3:-smb}"

if [ -z "$ROM" ] || [ -z "$NAME" ]; then
  sed -n '2,20p' "$0" | sed 's/^# \?//'
  exit 1
fi
[ -f "$ROM" ] || { echo "No such ROM: $ROM"; exit 1; }

# Where the nesrecomp checkout lives. Override if yours is elsewhere.
NESRECOMP="${NESRECOMP:-$ROOT/nesrecomp}"
[ -d "$NESRECOMP/recompiler" ] || {
  echo "No nesrecomp checkout at $NESRECOMP"
  echo "Run: git submodule update --init"
  exit 1
}

GAMEDIR="$ROOT/games/$NAME"
mkdir -p "$GAMEDIR"

# ---- 1. recompiler -------------------------------------------------------
RECOMP_BUILD="$NESRECOMP/recompiler/build"
RECOMP_BIN="$RECOMP_BUILD/NESRecomp"
[ -f "$RECOMP_BIN.exe" ] && RECOMP_BIN="$RECOMP_BIN.exe"
[ -f "$RECOMP_BUILD/Release/NESRecomp.exe" ] && RECOMP_BIN="$RECOMP_BUILD/Release/NESRecomp.exe"

if [ ! -f "$RECOMP_BIN" ]; then
  echo "==> building the recompiler"
  cmake -S "$NESRECOMP/recompiler" -B "$RECOMP_BUILD" -DCMAKE_BUILD_TYPE=Release >/dev/null
  cmake --build "$RECOMP_BUILD" --config Release >/dev/null
  [ -f "$RECOMP_BUILD/NESRecomp" ]           && RECOMP_BIN="$RECOMP_BUILD/NESRecomp"
  [ -f "$RECOMP_BUILD/NESRecomp.exe" ]       && RECOMP_BIN="$RECOMP_BUILD/NESRecomp.exe"
  [ -f "$RECOMP_BUILD/Release/NESRecomp.exe" ] && RECOMP_BIN="$RECOMP_BUILD/Release/NESRecomp.exe"
fi
[ -f "$RECOMP_BIN" ] || { echo "recompiler build produced no binary"; exit 1; }

# ---- 2. game.toml --------------------------------------------------------
if [ ! -f "$GAMEDIR/game.toml" ]; then
  cat > "$GAMEDIR/game.toml" << TOML
# Minimal config. Everything else nesrecomp infers from the iNES header.
#
# deduplicate_functions collapses identical function bodies, which matters a
# lot on a 4MB handheld: the function finder deliberately accepts harmless
# false positives, and overlapping entry points re-emit the same tail many
# times over.
#
# A bare config like this has no idea which bytes are data, so the pointer
# scanner decodes jump tables and level data as if they were code and emits
# functions for them. That inflates the binary substantially. If the build
# overflows, adding [[data_region]] entries for the ROM's data tables is the
# fix - see the game.toml in SuperMarioBrosNESRecomp for a worked example.
[game]
output_prefix = "$NAME"
deduplicate_functions = true
TOML
  echo "==> wrote $GAMEDIR/game.toml"
fi

# A symbols file gives the generated code real routine names instead of bare
# addresses, which makes every later debugging session far easier.
if [ -f "$GAMEDIR/symbols.sym" ] && ! grep -q symbol_file "$GAMEDIR/game.toml"; then
  sed -i 's/^\[game\]/[game]\nsymbol_file = "symbols.sym"/' "$GAMEDIR/game.toml"
fi

# ---- 3. recompile --------------------------------------------------------
echo "==> recompiling $(basename "$ROM")"
( cd "$GAMEDIR" && "$RECOMP_BIN" "$ROM" --game game.toml )

COUNT=$(ls "$GAMEDIR"/generated/*.c 2>/dev/null | wc -l)
[ "$COUNT" -gt 0 ] || { echo "no C generated - check the recompiler output above"; exit 1; }
echo "==> $COUNT generated files"

# ---- 4. build the DS binary ---------------------------------------------
echo "==> building for Nintendo DS (GAME=$GAME)"
cd "$ROOT"
[ -f source/mapper.c ] || bash tools/fetch_deps.sh

make -j4 GAME="$GAME" GAME_DIR="games/$NAME"

echo
echo "built $ROOT/nesrecomp-ds.nds"
echo "Put your ROM on the SD card as ${NAME}.nes, or in nitrofiles/ to embed it."
