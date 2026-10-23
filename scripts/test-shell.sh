#!/bin/sh
set -eu
cd "$(dirname "$0")/.."
mkdir -p build/tests
for test in input audio mapview progress; do
    ${CC:-gcc} -m32 -std=gnu99 -D_GNU_SOURCE -D_FILE_OFFSET_BITS=64 -fno-pie -no-pie \
        -Wall -Wextra -Wno-unused-parameter -ffunction-sections -fdata-sections -Wl,--gc-sections -O2 -o "build/tests/$test" "tests/$test.c" -ldl -lpthread -lm
    "build/tests/$test"
done
# Optional: supply public MI headers to also check the native helper's curve.
if [ -n "${MI_SDK_INCLUDE:-}" ]; then
    ${CC:-gcc} -std=gnu99 -Wall -Wextra -O2 -I "$MI_SDK_INCLUDE" \
        -o build/tests/volume tests/volume.c -ldl -lm
    build/tests/volume
fi
