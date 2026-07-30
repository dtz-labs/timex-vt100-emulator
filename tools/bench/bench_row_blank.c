/*
 * bench_row_blank.c -- T-state harness: blit_flush() with exactly one dirty
 * row that is fully blank (space, attr 0) -- the block-clear fast path that
 * never touches font data.
 */
#include "screen.h"
#include "blit.h"
#include "bench_common.h"

static screen_t scr;

int main(void)
{
    u8 r;

    screen_init(&scr);   /* every cell already space/attr-0 */

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
