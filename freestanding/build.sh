#!/bin/sh
# Compile the GPTPS C99 core under -ffreestanding -fno-builtin against the stub
# HAL, linking NO -lpthread / -ldl. A clean build + a 0 exit code proves the
# core's only platform dependency is the HAL seam (gptps_hal.h): no threads,
# no dynamic loader, no fork, and (via the demo's static pool) no libc heap.
#
# Usage:  sh freestanding/build.sh [output-binary]
set -eu
ROOT="$(cd "$(dirname "$0")/.." && pwd)"
CC="${CC:-cc}"
OUT="${1:-./gptps_freestanding_demo}"

$CC -ffreestanding -fno-builtin -std=c99 -Wall -Wextra -O2 \
    -I "$ROOT/include" -I "$ROOT/src" \
    "$ROOT/src/alloc.c" "$ROOT/src/config.c" "$ROOT/src/config_toml.c" \
    "$ROOT/src/settings.c" "$ROOT/src/engine.c" \
    "$ROOT/freestanding/hal_stub.c" "$ROOT/freestanding/exec_stub.c" \
    "$ROOT/freestanding/demo_freestanding.c" \
    -o "$OUT"
# NOTE: intentionally no -lpthread, no -ldl.

echo "built $OUT"
