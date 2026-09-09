#!/bin/bash
# Copy the one upstream runner source this project compiles directly.
#
# mapper.c is shared nesrecomp code (PRG/CHR banking, mirroring) rather than
# anything DS-specific, so it is fetched from the submodule instead of being
# vendored here - that way it tracks upstream rather than drifting.
set -eu
ROOT="$(cd "$(dirname "$0")/.." && pwd)"
SRC="${NESRECOMP:-$ROOT/nesrecomp}/runner/src/mapper.c"

if [ ! -f "$SRC" ]; then
  echo "Cannot find $SRC"
  echo "Run: git submodule update --init"
  exit 1
fi

cp "$SRC" "$ROOT/source/mapper.c"
echo "fetched mapper.c"
