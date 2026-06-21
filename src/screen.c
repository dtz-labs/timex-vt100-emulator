/*
 * screen.c -- terminal cell-grid model. See screen.h.
 */
#include "screen.h"

void screen_init(screen_t *s)
{
    u8 r, c;

    for (r = 0; r < ROWS; ++r) {
        for (c = 0; c < COLS; ++c) {
            s->cells[r][c].ch = BLANK_CH;
            s->cells[r][c].attr = 0;
        }
        s->dirty[r] = 1;
    }
    s->cx = 0;
    s->cy = 0;
    s->top = 0;
    s->bot = ROWS - 1;
    s->attr = 0;
}

void screen_putc(screen_t *s, u8 ch)
{
    s->cells[s->cy][s->cx].ch = ch;
    s->cells[s->cy][s->cx].attr = s->attr;
    s->dirty[s->cy] = 1;
    s->cx++;
}
