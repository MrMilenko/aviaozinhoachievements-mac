#!/bin/bash
# Build the macOS achievements bridge.
#   SDK defaults to ../../../bdd-deps/steamworks/sdk; override with STEAMWORKS_SDK=...
set -e
cd "$(dirname "$0")"

SDK="${STEAMWORKS_SDK:-/Users/milenko/bdd-deps/steamworks/sdk}"
[ -d "$SDK/public/steam" ] || { echo "Steamworks SDK not found at $SDK"; exit 1; }

cp "$SDK/redistributable_bin/osx/libsteam_api.dylib" .
# the dylib ships quarantined out of the downloaded zip; Gatekeeper blocks it otherwise
xattr -cr libsteam_api.dylib 2>/dev/null || true
codesign --force -s - libsteam_api.dylib 2>/dev/null || true

echo "4241920" > steam_appid.txt

clang++ -std=c++17 -arch arm64 -O2 \
    -I"$SDK/public" \
    launcher_mac.cpp \
    -L. -lsteam_api -Wl,-rpath,@executable_path \
    -o launcher_mac

codesign --force -s - launcher_mac 2>/dev/null || true
echo "built: $(pwd)/launcher_mac"
