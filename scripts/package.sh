#!/bin/sh
# Assembles the release in dist/, without any game files:
#   dist/Roms/PORTS/...                  copy to the SD card root
#   dist/gta-emiyoolator-<version>.zip   the same, zipped
# Build box86, gtaaudio and gtashell first (scripts/build-*.sh). Players add
# WINO, GTADATA and Music from their own copy of the game.
set -eu

ROOT="$(cd "$(dirname "$0")/.." && pwd)"
VERSION="$(cat "$ROOT/VERSION")"
OUT="$ROOT/dist"
GAME="$OUT/Roms/PORTS/Games/GTA1-Original"

[ -f "$ROOT/build/box86/box86" ] && [ -f "$ROOT/build/gtashell" ] && [ -f "$ROOT/build/gtaaudio" ] || { echo "build box86, gtaaudio and gtashell first" >&2; exit 1; }
[ -f "$ROOT/build/box86-src/LICENSE" ] || { echo "no box86 source (scripts/build-box86.sh clones it)" >&2; exit 1; }

rm -rf "$OUT/Roms" "$OUT"/gta-emiyoolator-*.zip
mkdir -p "$GAME" "$OUT/Roms/PORTS/Shortcuts/Action" "$OUT/Roms/PORTS/Imgs"
docker run --rm --user "$(id -u):$(id -g)" -v "$ROOT:/src" -w /src gta-emiyoolator-box86 \
	arm-linux-gnueabihf-strip -o dist/Roms/PORTS/Games/GTA1-Original/box86 build/box86/box86
cp "$ROOT/build/gtashell" "$ROOT/build/gtaaudio" "$GAME/"
cp "$ROOT/packaging/GTA1-Original/launch.sh" "$ROOT/packaging/GTA1-Original/gtashell.ini" \
	"$ROOT/packaging/GTA1-Original/README.txt" "$GAME/"
cp "$ROOT/VERSION" "$GAME/VERSION"
cp "$ROOT/LICENSE" "$GAME/LICENSE.txt"
cp "$ROOT/build/box86-src/LICENSE" "$GAME/box86-LICENSE.txt"
cp "$ROOT/packaging/Grand Theft Auto (Original).port" "$OUT/Roms/PORTS/Shortcuts/Action/"
cp "$ROOT/packaging/Imgs/Grand Theft Auto (Original).png" "$OUT/Roms/PORTS/Imgs/"
chmod +x "$GAME/box86" "$GAME/gtashell" "$GAME/gtaaudio" "$GAME/launch.sh" \
	"$OUT/Roms/PORTS/Shortcuts/Action/Grand Theft Auto (Original).port"
(cd "$OUT" && zip -qr "gta-emiyoolator-$VERSION.zip" Roms)
echo "packaged $OUT/gta-emiyoolator-$VERSION.zip"
