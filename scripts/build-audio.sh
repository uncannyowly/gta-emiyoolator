#!/bin/sh
# Builds build/gtaaudio, the native ARM sound player (MI_AO) for the Mini.
set -eu
ROOT="$(cd "$(dirname "$0")/.." && pwd)"
docker run --rm --user "$(id -u):$(id -g)" -v "$ROOT:/src" -w /src gta-emiyoolator-box86 sh -c \
	'arm-linux-gnueabihf-gcc -O2 -Wall -marm -mcpu=cortex-a7 -mfpu=neon-vfpv4 -mfloat-abi=hard -o build/gtaaudio tools/gtaaudio.c -ldl -lpthread && arm-linux-gnueabihf-strip build/gtaaudio'
file "$ROOT/build/gtaaudio"
