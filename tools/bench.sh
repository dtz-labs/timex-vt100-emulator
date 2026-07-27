#!/bin/sh
# Measure T-states for the renderer's hot paths with z88dk-ticks.
#
# CI prints these; it does not enforce them. A regression threshold would need a
# pinned compiler image, and .github/workflows/ci.yml uses z88dk/z88dk:latest.
#
# Each harness (tools/bench/bench_<path>.c) links the real project sources
# (screen.c, render.c, render_hires.c, blit_hires.c, hires.c, font.c) and
# drives ONE public entry point from a known state, bracketed by
# bench_mark_a()/bench_mark_b() (tools/bench/bench_common.c). z88dk-ticks
# resets its cycle counter the instant PC reaches bench_mark_a's entry and
# stops it the instant PC reaches bench_mark_b's entry, so the printed number
# is the T-states strictly between those two calls, plus a small fixed
# overhead: bench_mark_a's own body (a volatile store + RET) and the CALL
# instruction that enters bench_mark_b -- on the order of 40 T-states,
# negligible against the hundred-thousand-T-state paths measured here.
#
# +zx classic target: CODE orgs at 0x8000 (confirmed against this repo's own
# build/term.map / build/term_CODE.bin -- _main sits at 0x9EED, 0x1EED bytes
# into a CODE.bin that is exactly that far removed from the reported image-end
# address). Every bench harness below is a tiny freestanding `main()` linked
# the same way, so it orgs at the same address.
set -e

ROOT="$(cd "$(dirname "$0")/.." && pwd)"
OUT="$ROOT/build/bench"
BENCH_SRC="$ROOT/tools/bench"
mkdir -p "$OUT"

CODE_ORG=0x8000
TICKS_COUNTER=100000000

# Mirror the Makefile's z88dk auto-detection so this script works whether or
# not the caller has already put z88dk's bin/ on PATH.
Z88DK_COMMON_PREFIXES="$HOME/Programowanie/z88dk /opt/homebrew/opt/z88dk /opt/homebrew /usr/local/opt/z88dk /usr/local /opt/local /opt/z88dk /usr/local/z88dk /usr"
if ! command -v zcc >/dev/null 2>&1; then
    for dir in $Z88DK_COMMON_PREFIXES; do
        if [ -x "$dir/bin/zcc" ]; then
            PATH="$dir/bin:$PATH"
            export PATH
            break
        fi
    done
fi
if [ -z "$ZCCCFG" ]; then
    for dir in $Z88DK_COMMON_PREFIXES; do
        if [ -d "$dir/lib/config" ]; then
            ZCCCFG="$dir/lib/config"
            export ZCCCFG
            break
        fi
    done
fi

command -v zcc >/dev/null 2>&1 || { echo "zcc not found on PATH; install z88dk or set Z88DK_HOME." >&2; exit 127; }
command -v z88dk-ticks >/dev/null 2>&1 || { echo "z88dk-ticks not found on PATH." >&2; exit 127; }

echo "compiler: $(zcc 2>&1 | head -1)"
echo "see docs/perf/benchmarks.md for the recorded reference numbers"

# Task 11: measure BOTH geometries, mirroring test/run.sh's two-pass
# convention. screen.h requires exactly one of TERM_TIMEX/TERM_ZX (the
# COLS=80/40 split), so each geometry gets its own -D and its own
# render_*.c/blit_*.c/{hires,ula}.c trio, in its own output subdirectory so
# the two passes' object/binary names never collide.
for geom in timex zx; do
    case "$geom" in
        timex)
            TERM_DEF=-DTERM_TIMEX
            GEOM_SOURCES="$ROOT/src/render_hires.c $ROOT/src/blit_hires.c $ROOT/src/hires.c"
            ;;
        zx)
            TERM_DEF=-DTERM_ZX
            GEOM_SOURCES="$ROOT/src/render_ula.c $ROOT/src/blit_ula.c $ROOT/src/ula.c"
            ;;
    esac
    PROJECT_SOURCES="$ROOT/src/screen.c $ROOT/src/render.c $GEOM_SOURCES $ROOT/src/font.c $BENCH_SRC/bench_common.c"
    GEOM_OUT="$OUT/$geom"
    mkdir -p "$GEOM_OUT"

    echo "--- geometry: $TERM_DEF ---"
    for path in row_normal row_attrs row_blank scroll_model scroll_vram; do
        printf '%-14s ' "$path"

        zcc +zx -SO3 -clib=sdcc_iy -iquote"$ROOT/include" $TERM_DEF \
            "$BENCH_SRC/bench_$path.c" $PROJECT_SOURCES \
            -o "$GEOM_OUT/bench_$path" -create-app -m >"$GEOM_OUT/bench_$path.build.log" 2>&1 \
            || { echo "BUILD FAILED (see $GEOM_OUT/bench_$path.build.log)"; exit 1; }

        # NOTE: z88dk-ticks loads <input_file> using whatever `load_address` its
        # arg-parser has seen SO FAR -- flags are applied left to right as parsed,
        # and the file load happens the instant the (flag-less) filename argument
        # is reached. -l MUST therefore appear BEFORE the filename, or the file
        # loads at address 0 while PC starts at $8000 (a walk through unrelated/
        # zero memory that still happens to cross the -start/-end addresses,
        # producing a plausible-looking but meaningless number). Confirmed by
        # tracing: filename-before-flags reproducibly returns the NOP-count
        # between the two label addresses instead of the real T-state cost.
        z88dk-ticks -l "$CODE_ORG" \
            -x "$GEOM_OUT/bench_$path.map" \
            -start _bench_mark_a -end _bench_mark_b \
            -counter "$TICKS_COUNTER" \
            "$GEOM_OUT/bench_${path}_CODE.bin" 2>/dev/null | tail -1
    done
done
