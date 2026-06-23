#!/bin/sh
# Build and run the host (native) unit tests for the pure-logic modules.
# These compile with the macOS system compiler -- no Z80 toolchain, no
# emulator -- so red/green/refactor is instant. Hardware-touching code
# (video/SCLD, render, keyboard ports) is verified separately in ZEsarUX.
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

$CC $CFLAGS "$ROOT/test/test_font.c" "$ROOT/src/font.c" -o "$OUT/test_font"
"$OUT/test_font"

$CC $CFLAGS "$ROOT/test/test_render.c" "$ROOT/src/render.c" "$ROOT/src/font.c" "$ROOT/src/hires.c" -o "$OUT/test_render"
"$OUT/test_render"

$CC $CFLAGS "$ROOT/test/test_conn.c" "$ROOT/src/conn.c" -o "$OUT/test_conn"
"$OUT/test_conn"

$CC $CFLAGS "$ROOT/test/test_keybuf.c" "$ROOT/src/keybuf.c" -o "$OUT/test_keybuf"
"$OUT/test_keybuf"

$CC $CFLAGS -DKEYMAP_TESTING -DKEYMAP_HOST_TEST "$ROOT/test/test_keymap.c" "$ROOT/src/keymap.c" -o "$OUT/test_keymap"
"$OUT/test_keymap"

echo "ALL HOST TESTS PASSED"
