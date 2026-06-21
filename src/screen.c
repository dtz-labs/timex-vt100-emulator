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

void screen_cup(screen_t *s, u8 row, u8 col)
{
    if (row >= ROWS) {
        row = ROWS - 1;
    }
    if (col >= COLS) {
        col = COLS - 1;
    }
    s->cy = row;
    s->cx = col;
}

/* Blank one row to space/attr-0. */
static void blank_row(screen_t *s, int r)
{
    int c;
    for (c = 0; c < (int)COLS; ++c) {
        s->cells[r][c].ch = BLANK_CH;
        s->cells[r][c].attr = 0;
    }
}

void screen_scroll(screen_t *s, s8 n)
{
    int top = s->top;
    int bot = s->bot;
    int height = bot - top + 1;
    int absn, r, c;

    if (n == 0) {
        return;
    }
    absn = (n > 0) ? n : -n;
    if (absn > height) {
        absn = height;
    }

    if (n > 0) {                       /* scroll up: content moves toward top */
        for (r = top; r <= bot - absn; ++r) {
            for (c = 0; c < (int)COLS; ++c) {
                s->cells[r][c] = s->cells[r + absn][c];
            }
        }
        for (r = bot - absn + 1; r <= bot; ++r) {
            blank_row(s, r);
        }
    } else {                           /* scroll down: content moves toward bot */
        for (r = bot; r >= top + absn; --r) {
            for (c = 0; c < (int)COLS; ++c) {
                s->cells[r][c] = s->cells[r - absn][c];
            }
        }
        for (r = top; r <= top + absn - 1; ++r) {
            blank_row(s, r);
        }
    }

    for (r = top; r <= bot; ++r) {
        s->dirty[r] = 1;
    }
}
