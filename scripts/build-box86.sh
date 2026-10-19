#!/bin/sh
# Cross-compiles box86 (x86 → ARM translator) for the Miyoo Mini into
# build/box86/box86. The source is cloned into build/box86-src (not committed);
# BOX86_COMMIT pins the version this project was tested with.
set -eu

ROOT="$(cd "$(dirname "$0")/.." && pwd)"
IMAGE=gta-emiyoolator-box86
BOX86_COMMIT="${BOX86_COMMIT:-7dec0819b5f212910cc7ff030f5161742add121c}"

if ! docker image inspect gta1-miyoo-toolchain >/dev/null 2>&1; then
	echo "needs the gta1-miyoo-toolchain image: run ../gta1-miyoo/scripts/build-miyoo.sh once" >&2
	exit 1
fi
docker image inspect "$IMAGE" >/dev/null 2>&1 || docker build -t "$IMAGE" -f "$ROOT/docker/Dockerfile.box86" "$ROOT/docker"

if [ ! -d "$ROOT/build/box86-src/.git" ]; then
	git clone https://github.com/ptitSeb/box86.git "$ROOT/build/box86-src"
fi
git -C "$ROOT/build/box86-src" fetch -q --depth 1 origin "$BOX86_COMMIT" 2>/dev/null || true
git -C "$ROOT/build/box86-src" checkout -q "$BOX86_COMMIT"

docker run --rm --user "$(id -u):$(id -g)" -v "$ROOT:/src" -w /src "$IMAGE" sh -c '
	set -e
	mkdir -p build/box86 && cd build/box86
	cmake ../box86-src -DCMAKE_TOOLCHAIN_FILE=/src/docker/miyoo.cmake \
		-DRPI2=1 -DARM_DYNAREC=ON -DCMAKE_BUILD_TYPE=RelWithDebInfo -DNOGIT=1
	make -j"$(nproc)" box86
'
file "$ROOT/build/box86/box86"
