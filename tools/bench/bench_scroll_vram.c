/*
 * bench_scroll_vram.c -- T-state harness: blit_scroll_region() over the full
 * 24-row region -- the hardware hi-res-file memmove/memset path.
 *
 * Not touched by Task 5; benchmarked here so the "before" and "after" numbers
 * can show this path is unaffected by this task's blit_flush change.
 */
#include "screen.h"
#include "blit.h"
#include "bench_common.h"

static screen_t scr;

int main(void)
{
    screen_init(&scr);

    bench_mark_a();
    blit_scroll_region(&scr, 0, ROWS - 1, 1);
    bench_mark_b();

    for (;;) { }
    return 0;
}
