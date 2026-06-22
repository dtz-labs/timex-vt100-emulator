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
    s->saved_cx = 0;
    s->saved_cy = 0;
    s->saved_attr = 0;
    s->mode = MODE_AUTOWRAP | MODE_CURSOR_VISIBLE;  /* power-on default */
    s->wrap_pending = 0;
}

void screen_putc(screen_t *s, u8 ch)
{
    if (s->wrap_pending) {            /* deferred wrap from a prior last-column write */
        screen_cr(s);                /* (clears wrap_pending, cx -> 0) */
        screen_lf(s);                /* down a row, scrolling at the region bottom */
    }
    s->cells[s->cy][s->cx].ch = ch;
    s->cells[s->cy][s->cx].attr = s->attr;
    s->dirty[s->cy] = 1;
    if (s->cx + 1u >= COLS) {
        /* Last column: park the cursor. With autowrap, defer the wrap until the
         * next printable (VT-100). Without it, stay put and overwrite in place. */
        if (s->mode & MODE_AUTOWRAP) {
            s->wrap_pending = 1;
        }
    } else {
        s->cx++;
    }
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
    s->wrap_pending = 0;             /* any explicit cursor move clears the LCF */
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

/* Scroll an arbitrary inclusive row region [top..bot] by n (>0 up, <0 down),
 * blanking freed rows and dirtying the region. Shared by screen_scroll (the
 * whole scroll region) and IL/DL (a region starting at the cursor row). */
static void scroll_region(screen_t *s, int top, int bot, int n)
{
    int height = bot - top + 1;
    int absn, r, c;

    if (n == 0 || height <= 0) {
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

void screen_scroll(screen_t *s, s8 n)
{
    scroll_region(s, s->top, s->bot, n);
}

void screen_cr(screen_t *s)
{
    s->cx = 0;
    s->wrap_pending = 0;
}

void screen_lf(screen_t *s)
{
    if (s->cy == s->bot) {
        screen_scroll(s, 1);          /* at region bottom: scroll up, stay */
    } else if (s->cy < ROWS - 1) {
        s->cy++;
    }
    s->wrap_pending = 0;
}

void screen_ri(screen_t *s)
{
    if (s->cy == s->top) {
        screen_scroll(s, -1);         /* at region top: scroll down, stay */
    } else if (s->cy > 0) {
        s->cy--;
    }
    s->wrap_pending = 0;
}

/* Blank an inclusive column span [c0..c1] of one row to space/attr-0, dirtying
 * the row. */
static void blank_cells(screen_t *s, int row, int c0, int c1)
{
    int c;
    for (c = c0; c <= c1; ++c) {
        s->cells[row][c].ch = BLANK_CH;
        s->cells[row][c].attr = 0;
    }
    s->dirty[row] = 1;
}

void screen_erase_line(screen_t *s, u8 mode)
{
    if (mode == 0) {
        blank_cells(s, s->cy, s->cx, COLS - 1);
    } else if (mode == 1) {
        blank_cells(s, s->cy, 0, s->cx);
    } else {                                   /* mode 2 (and anything else) */
        blank_cells(s, s->cy, 0, COLS - 1);
    }
}

void screen_erase_display(screen_t *s, u8 mode)
{
    int r;
    if (mode == 0) {                           /* cursor .. end of screen */
        blank_cells(s, s->cy, s->cx, COLS - 1);
        for (r = s->cy + 1; r < (int)ROWS; ++r) {
            blank_cells(s, r, 0, COLS - 1);
        }
    } else if (mode == 1) {                    /* start of screen .. cursor */
        for (r = 0; r < s->cy; ++r) {
            blank_cells(s, r, 0, COLS - 1);
        }
        blank_cells(s, s->cy, 0, s->cx);
    } else {                                   /* mode 2: whole screen */
        for (r = 0; r < (int)ROWS; ++r) {
            blank_cells(s, r, 0, COLS - 1);
        }
    }
}

void screen_insert_lines(screen_t *s, u8 n)
{
    if (s->cy < s->top || s->cy > s->bot) {
        return;
    }
    scroll_region(s, s->cy, s->bot, -(int)n);    /* down: blanks at cursor row */
}

void screen_delete_lines(screen_t *s, u8 n)
{
    if (s->cy < s->top || s->cy > s->bot) {
        return;
    }
    scroll_region(s, s->cy, s->bot, (int)n);     /* up: blanks at region bottom */
}

void screen_insert_chars(screen_t *s, u8 n)
{
    int row = s->cy;
    int cx = s->cx;
    int cnt = n;
    int c;
    if (cnt > (int)COLS - cx) {
        cnt = (int)COLS - cx;
    }
    for (c = (int)COLS - 1; c >= cx + cnt; --c) {
        s->cells[row][c] = s->cells[row][c - cnt];
    }
    for (c = cx; c < cx + cnt; ++c) {
        s->cells[row][c].ch = BLANK_CH;
        s->cells[row][c].attr = 0;
    }
    s->dirty[row] = 1;
}

void screen_delete_chars(screen_t *s, u8 n)
{
    int row = s->cy;
    int cx = s->cx;
    int cnt = n;
    int c;
    if (cnt > (int)COLS - cx) {
        cnt = (int)COLS - cx;
    }
    for (c = cx; c <= (int)COLS - 1 - cnt; ++c) {
        s->cells[row][c] = s->cells[row][c + cnt];
    }
    for (c = (int)COLS - cnt; c < (int)COLS; ++c) {
        s->cells[row][c].ch = BLANK_CH;
        s->cells[row][c].attr = 0;
    }
    s->dirty[row] = 1;
}

void screen_set_attr(screen_t *s, u8 sgr)
{
    switch (sgr) {
    case 0:  s->attr = 0;                      break;  /* reset all */
    case 7:  s->attr |= ATTR_REVERSE;          break;  /* reverse on */
    case 27: s->attr &= (u8)~ATTR_REVERSE;     break;  /* reverse off */
    case 4:  s->attr |= ATTR_UNDERLINE;        break;  /* underline on */
    case 24: s->attr &= (u8)~ATTR_UNDERLINE;   break;  /* underline off */
    default: /* bold, colours, etc.: accepted and ignored (monochrome) */    break;
    }
}

void screen_save_cursor(screen_t *s)
{
    s->saved_cx = s->cx;
    s->saved_cy = s->cy;
    s->saved_attr = s->attr;
}

void screen_restore_cursor(screen_t *s)
{
    s->cx = s->saved_cx;
    s->cy = s->saved_cy;
    s->attr = s->saved_attr;
}

void screen_set_mode(screen_t *s, u8 bits, u8 on)
{
    if (on) {
        s->mode |= bits;
    } else {
        s->mode &= (u8)~bits;
    }
}

void screen_set_scroll_region(screen_t *s, u8 top, u8 bot)
{
    if (bot >= ROWS) {
        bot = ROWS - 1;
    }
    if (top >= bot) {
        return;                  /* degenerate / out of range: ignore (VT-100) */
    }
    s->top = top;
    s->bot = bot;
    screen_cup(s, 0, 0);         /* DECSTBM homes the cursor */
}
