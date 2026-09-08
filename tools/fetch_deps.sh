#!/bin/bash
# Copy the one upstream runner source this project compiles directly.
#
# mapper.c is shared nesrecomp code (PRG/CHR banking, mirroring) rather than
# anything DS-specific, so it is fetched from the submodule instead of being
# vendored here - that way it tracks upstream rather than drifting.
set -eu
ROOT="$(cd "$(dirname "$0")/.." && pwd)"
GAME_DIR="${GAME_DIR:-$ROOT/..}"
SRC="$GAME_DIR/nesrecomp/runner/src/mapper.c"

if [ ! -f "$SRC" ]; then
  echo "Cannot find $SRC"
  echo "Run setup.sh in the game project first, or set GAME_DIR."
  exit 1
fi

cp "$SRC" "$ROOT/source/mapper.c"
echo "fetched mapper.c"
