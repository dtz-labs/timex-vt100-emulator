/*
 * bench_row_attrs.c -- T-state harness: blit_flush() with exactly one dirty
 * row where every cell carries ATTR_REVERSE, forcing every one of the row's
 * 20 groups through the slow fallback path (render_glyph_row_byte +
 * render_pack4), since the fast path requires all four cells of a group to
 * have attr == 0.
 */
#include "screen.h"
#include "blit.h"
#include "bench_common.h"

static screen_t scr;

int main(void)
{
    u8 c, r;

    screen_init(&scr);

    for (c = 0; c < COLS; ++c) {
        scr.cells[0][c].ch = (u8)('A' + (c % 26u));
        scr.cells[0][c].attr = ATTR_REVERSE;
    }

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
