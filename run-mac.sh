#!/bin/bash
# Launch the macOS BDD Pre-4 build against the Steam (Windows depot) game data.
# Usage:  ./run-mac.sh [extra quake args...]
#   ./run-mac.sh                          # just run it
#   ./run-mac.sh +connect 192.168.1.50    # join a host
#   ./run-mac.sh +coop 1 +maxplayers 4 +map 2-SALGUEIROSTREET2   # host coop
BASE="${BDD4_BASEDIR:-/Users/milenko/BDD4-win}"
BIN="$(cd "$(dirname "$0")" && pwd)/Build-Pre4/AVIAO3GAME"

[ -x "$BIN" ] || { echo "Missing $BIN — build first:"; \
  echo "  cmake --build build-pre4 -j"; exit 1; }
[ -d "$BASE/bddpre4" ] || { echo "No game data at $BASE (need the bddpre4/ folder)"; exit 1; }

cd "$BASE" || exit 1
exec "$BIN" -basedir "$BASE" -window "$@"
