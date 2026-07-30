/*
 * bench_row_normal.c -- T-state harness: blit_flush() with exactly one dirty
 * row, every cell attr == 0 (the inline fast-path group loop this task adds).
 *
 * Measures the real public entry point (blit_flush), not an internal helper,
 * so the number includes the 23 cheap "already clean" row checks a real call
 * does -- see docs/perf/benchmarks.md for the disclosed overhead this adds.
 */
#include "screen.h"
#include "blit.h"
#include "bench_common.h"

static screen_t scr;

int main(void)
{
    u8 c, r;

    screen_init(&scr);   /* blanks every cell, marks every row dirty */

    for (c = 0; c < COLS; ++c) {
        scr.cells[0][c].ch = (u8)('A' + (c % 26u));
        scr.cells[0][c].attr = 0;
    }

    /* Isolate row 0: clear every row's marks, then mark row 0 alone. */
    for (r = 0; r < ROWS; ++r) {
        screen_clear_marks(&scr, r);
    }
    screen_mark_row(&scr, 0);

    bench_mark_a();
    blit_flush(&scr);
    bench_mark_b();

    for (;;) { }
    return 0;
}
