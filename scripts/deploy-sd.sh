#!/bin/sh
# Installs GTA-eMiYoolator on an OnionOS SD card:
#   scripts/deploy-sd.sh /run/media/$USER/CARD "/path/to/Grand Theft Auto"
# Copies box86, gtashell and the launcher into Roms/PORTS/Games/GTA1-Original,
# the original exe from the given install, GTADATA once (from the GTA1 port's
# folder on the card if present, else from the install), the Ports shortcut
# and box art. Keeps an existing gtashell.ini. Never touches other ports.
set -eu

ROOT="$(cd "$(dirname "$0")/.." && pwd)"
SD="${1:?SD card root}"
INSTALL="${2:?original GTA1 install folder (with WINO and GTADATA)}"
DEST="$SD/Roms/PORTS/Games/GTA1-Original"

[ -d "$SD/Roms/PORTS" ] || { echo "$SD does not look like an OnionOS card" >&2; exit 1; }
[ -f "$INSTALL/WINO/Grand Theft Auto.exe" ] || { echo "no WINO/Grand Theft Auto.exe in $INSTALL" >&2; exit 1; }
[ -f "$ROOT/build/box86/box86" ] && [ -f "$ROOT/build/gtashell" ] || { echo "build box86 and gtashell first" >&2; exit 1; }

mkdir -p "$DEST/WINO"
docker run --rm --user "$(id -u):$(id -g)" -v "$ROOT:/src" -w /src gta-emiyoolator-box86 \
	arm-linux-gnueabihf-strip -o build/box86/box86.stripped build/box86/box86
cp -f "$ROOT/build/box86/box86.stripped" "$DEST/box86"
cp -f "$ROOT/build/gtashell" "$DEST/gtashell"
cp -f "$ROOT/packaging/GTA1-Original/launch.sh" "$ROOT/packaging/GTA1-Original/README.txt" "$DEST/"
cp -f "$ROOT/VERSION" "$DEST/VERSION"
if [ -f "$DEST/gtashell.ini" ]; then
	cp -f "$ROOT/packaging/GTA1-Original/gtashell.ini" "$DEST/gtashell.ini.new"
else
	cp -f "$ROOT/packaging/GTA1-Original/gtashell.ini" "$DEST/gtashell.ini"
fi
cp -f "$INSTALL/WINO/Grand Theft Auto.exe" "$DEST/WINO/"

if [ ! -d "$DEST/GTADATA" ]; then
	if [ -d "$SD/Roms/PORTS/Games/GTA1/GTADATA" ]; then
		cp -r "$SD/Roms/PORTS/Games/GTA1/GTADATA" "$DEST/GTADATA"
	else
		cp -r "$INSTALL/GTADATA" "$DEST/GTADATA"
	fi
fi

mkdir -p "$SD/Roms/PORTS/Shortcuts/Action"
cp -f "$ROOT/packaging/Grand Theft Auto (Original).port" "$SD/Roms/PORTS/Shortcuts/Action/"
if [ -f "$SD/Roms/PORTS/Imgs/Grand Theft Auto.png" ] && [ ! -f "$SD/Roms/PORTS/Imgs/Grand Theft Auto (Original).png" ]; then
	cp "$SD/Roms/PORTS/Imgs/Grand Theft Auto.png" "$SD/Roms/PORTS/Imgs/Grand Theft Auto (Original).png"
fi
sync
echo "installed to $DEST"
ls -la "$DEST"
