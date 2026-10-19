#!/bin/sh
# Builds build/gtashell (32-bit x86) inside the Debian i386 container.
set -eu
ROOT="$(cd "$(dirname "$0")/.." && pwd)"
IMAGE=gta-emiyoolator-i386
docker image inspect "$IMAGE" >/dev/null 2>&1 || docker build -t "$IMAGE" -f "$ROOT/docker/Dockerfile.i386" "$ROOT/docker"
docker run --rm --platform linux/386 --user "$(id -u):$(id -g)" -v "$ROOT:/src" -w /src "$IMAGE" make -j"$(nproc)" "$@"
file "$ROOT/build/gtashell"
