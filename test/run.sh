#!/bin/sh
# Build and run the host (native) unit tests for the pure-logic modules.
# These compile with the macOS system compiler -- no Z80 toolchain, no
# emulator -- so red/green/refactor is instant. Hardware-touching code
# (video/SCLD, render, keyboard ports) is verified separately in Fuse.
set -e

ROOT="$(cd "$(dirname "$0")/.." && pwd)"
CC="${CC:-cc}"
CFLAGS="-std=c99 -Wall -Wextra -Werror -I$ROOT/include"
OUT="$ROOT/build/host"
mkdir -p "$OUT"

# One executable per test_*.c, linked against the matching pure-logic sources.
$CC $CFLAGS "$ROOT/test/test_screen.c" "$ROOT/src/screen.c" -o "$OUT/test_screen"
"$OUT/test_screen"

$CC $CFLAGS "$ROOT/test/test_vtparse.c" "$ROOT/src/vtparse.c" "$ROOT/src/screen.c" -o "$OUT/test_vtparse"
"$OUT/test_vtparse"

$CC $CFLAGS "$ROOT/test/test_hires.c" "$ROOT/src/hires.c" -o "$OUT/test_hires"
"$OUT/test_hires"

echo "ALL HOST TESTS PASSED"
