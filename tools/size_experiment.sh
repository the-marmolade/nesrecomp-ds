#!/bin/bash
#
# size_experiment.sh — measure how nesrecomp options affect the DS binary.
#
#   bash tools/size_experiment.sh /path/to/rom.nes smb
#
# The recompiler's function finder deliberately accepts false positives: on a
# PC the wasted code costs nothing, on a 4MB handheld it is the difference
# between fitting and not. Several game.toml options trade discovery breadth
# against size, and the only honest way to pick is to measure.
#
# Runs a clean recompile + DS build per configuration and reports .text.
# Nothing is kept; games/<name>/ is restored at the end.
set -eu

ROOT="$(cd "$(dirname "$0")/.." && pwd)"
ROM="${1:-}"; NAME="${2:-smb}"
[ -n "$ROM" ] && [ -f "$ROM" ] || { echo "usage: $0 <rom.nes> [name]"; exit 1; }

NESRECOMP="${NESRECOMP:-$ROOT/nesrecomp}"
RECOMP="$NESRECOMP/recompiler/build/Release/NESRecomp.exe"
[ -f "$RECOMP" ] || RECOMP="$NESRECOMP/recompiler/build/NESRecomp"
[ -f "$RECOMP" ] || { echo "build the recompiler first (see tools/port.sh)"; exit 1; }

SIZE="${DEVKITARM:-/opt/devkitpro/devkitARM}/bin/arm-none-eabi-size"
GAMEDIR="$ROOT/games/$NAME"
BACKUP=""
if [ -d "$GAMEDIR" ]; then
  BACKUP="$(mktemp -d)"
  cp -r "$GAMEDIR" "$BACKUP/"
fi

run_case() {
  local label="$1" extra="$2"
  rm -rf "$GAMEDIR"; mkdir -p "$GAMEDIR"
  {
    echo "[game]"
    echo "output_prefix = \"$NAME\""
    [ -n "$extra" ] && echo "$extra"
  } > "$GAMEDIR/game.toml"

  ( cd "$GAMEDIR" && "$RECOMP" "$ROM" --game game.toml --output-prefix "$NAME" ) \
      > "$GAMEDIR/recomp.log" 2>&1 || { printf '%-28s RECOMPILE FAILED\n' "$label"; return; }

  local funcs
  funcs=$(grep -oE "Found [0-9]+ functions" "$GAMEDIR/recomp.log" | grep -oE "[0-9]+" | head -1)

  ( cd "$ROOT" && make clean >/dev/null 2>&1; \
    make -j4 GAME="$NAME" GAME_DIR="games/$NAME" >/dev/null 2>&1 ) \
      || { printf '%-28s %6s funcs   LINK FAILED (too big?)\n' "$label" "$funcs"; return; }

  local text
  text=$("$SIZE" "$ROOT/$NAME.elf" 2>/dev/null || "$SIZE" "$ROOT"/*.elf 2>/dev/null | tail -1)
  printf '%-28s %6s funcs   %s\n' "$label" "$funcs" "$(echo "$text" | tail -1 | awk '{print $1" bytes text"}')"
}

echo "config                        functions   size"
echo "------------------------------------------------------------"
run_case "baseline"                 ""
run_case "dedup"                    "deduplicate_functions = true"
run_case "no-ptr-scan"              "disable_ptr_scan = true"
run_case "dedup + no-ptr-scan"      "deduplicate_functions = true
disable_ptr_scan = true"

if [ -n "$BACKUP" ]; then
  rm -rf "$GAMEDIR"
  cp -r "$BACKUP/$NAME" "$GAMEDIR"
  rm -rf "$BACKUP"
  echo
  echo "games/$NAME restored"
fi

cat << 'NOTE'

disable_ptr_scan stops the recompiler inventing functions from what may be
data. It is the biggest single lever, but it is also the riskiest: a game that
genuinely dispatches through a pointer table will lose those entry points and
hit the (stubbed) interpreter path at runtime. Measure first, then test the
game actually plays before trusting the smaller build.
NOTE
