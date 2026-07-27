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

# The pure core is width-parameterised at compile time, so it is compiled and
# run once per geometry. This is what keeps 40-column wrapping, tabs and erase
# behaviour under test without any runtime width plumbing.
for TERM_DEF in -DTERM_TIMEX -DTERM_ZX; do
    case "$TERM_DEF" in
        -DTERM_TIMEX) GEOM_SRC="$ROOT/src/render_hires.c"; HIRES_SRC="$ROOT/src/hires.c"; SUFFIX=timex ;;
        -DTERM_ZX)    GEOM_SRC="$ROOT/src/render_ula.c";   HIRES_SRC="";                  SUFFIX=zx ;;
    esac
    echo "--- geometry: $TERM_DEF ---"

    $CC $CFLAGS $TERM_DEF "$ROOT/test/test_screen.c" "$ROOT/src/screen.c" \
        -o "$OUT/test_screen_$SUFFIX"
    "$OUT/test_screen_$SUFFIX"

    $CC $CFLAGS $TERM_DEF "$ROOT/test/test_vtparse.c" "$ROOT/src/vtparse.c" \
        "$ROOT/src/screen.c" -o "$OUT/test_vtparse_$SUFFIX"
    "$OUT/test_vtparse_$SUFFIX"

    # src/hires.c is only linked in for the Timex pass: nothing in the ZX
    # pass's test_render references hires_addr (blit_hires.c is the only
    # caller, and it is target-only, never host-compiled).
    $CC $CFLAGS $TERM_DEF "$ROOT/test/test_render.c" "$ROOT/src/render.c" \
        "$GEOM_SRC" "$ROOT/src/font.c" $HIRES_SRC \
        -o "$OUT/test_render_$SUFFIX"
    "$OUT/test_render_$SUFFIX"
done

# Width-independent suites: compiled once, with no machine define at all --
# correct because none of them include screen.h, so its
# TERM_TIMEX/TERM_ZX #error guard (which needs exactly one of the two) never
# triggers.
$CC $CFLAGS "$ROOT/test/test_hires.c" "$ROOT/src/hires.c" -o "$OUT/test_hires"
"$OUT/test_hires"

$CC $CFLAGS "$ROOT/test/test_ula.c" "$ROOT/src/ula.c" -o "$OUT/test_ula"
"$OUT/test_ula"

$CC $CFLAGS "$ROOT/test/test_font.c" "$ROOT/src/font.c" -o "$OUT/test_font"
"$OUT/test_font"

$CC $CFLAGS "$ROOT/test/test_conn.c" "$ROOT/src/conn.c" -o "$OUT/test_conn"
"$OUT/test_conn"

$CC $CFLAGS "$ROOT/test/test_keybuf.c" "$ROOT/src/keybuf.c" -o "$OUT/test_keybuf"
"$OUT/test_keybuf"

$CC $CFLAGS -DKEYMAP_TESTING -DKEYMAP_HOST_TEST "$ROOT/test/test_keymap.c" "$ROOT/src/keymap.c" -o "$OUT/test_keymap"
"$OUT/test_keymap"

echo "ALL HOST TESTS PASSED"
