/*
 * bench_scroll_model.c -- T-state harness: screen_scroll() over the full
 * 24-row region, content filled so the memmove has real bytes to move.
 *
 * Not touched by Task 5 (Task 3 already replaced the cell loops with
 * memmove()); benchmarked here so the "before" and "after" numbers can show
 * this path is unaffected by this task's blit_flush change.
 */
#include "screen.h"
#include "bench_common.h"

static screen_t scr;

int main(void)
{
    u8 r, c;

    screen_init(&scr);
    for (r = 0; r < ROWS; ++r) {
        for (c = 0; c < COLS; ++c) {
            scr.cells[r][c].ch = (u8)('A' + ((r + c) % 26u));
            scr.cells[r][c].attr = 0;
        }
    }

    bench_mark_a();
    screen_scroll(&scr, 1);
    bench_mark_b();

    for (;;) { }
    return 0;
}
