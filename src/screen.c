/*
 * screen.c -- terminal cell-grid model. See screen.h.
 */
#include "screen.h"
#include <string.h>

void screen_mark_cell(screen_t *s, u8 row, u8 col)
{
    u8 g = (u8)(col / DIRTY_GROUP_COLS);

    s->dirty[row][g >> 3] |= (u8)(1u << (g & 7u));
}

void screen_mark_span(screen_t *s, u8 row, u8 c0, u8 c1)
{
    u8 g = (u8)(c0 / DIRTY_GROUP_COLS);
    u8 last = (u8)(c1 / DIRTY_GROUP_COLS);

    for (; g <= last; ++g) {
        s->dirty[row][g >> 3] |= (u8)(1u << (g & 7u));
    }
}

void screen_mark_row(screen_t *s, u8 row)
{
    screen_mark_span(s, row, 0, (u8)(COLS - 1u));
}

void screen_clear_marks(screen_t *s, u8 row)
{
    u8 b;

    for (b = 0; b < DIRTY_BYTES; ++b) {
        s->dirty[row][b] = 0;
    }
}

/*
 * CONSTRAINT for anyone changing this bit test: src/blit_hires.c's
 * blit_row_groups() hand-copies this exact expression inline, for measured
 * hot-path performance (see docs/perf/benchmarks.md, "Function-call vs.
 * inlined mapping/dirty-check"). blit_hires.c cannot be host-compiled
 * (absolute HIRES_FILE0/1 addresses), so test/run.sh will NOT catch a
 * divergence between that copy and this function if you change this
 * without updating both. Grep for "DUPLICATED FORMULAS" in blit_hires.c
 * before touching this.
 */
u8 screen_group_dirty(const screen_t *s, u8 row, u8 group)
{
    return (u8)(s->dirty[row][group >> 3] & (u8)(1u << (group & 7u)));
}

u8 screen_row_dirty(const screen_t *s, u8 row)
{
    u8 b, any = 0;

    for (b = 0; b < DIRTY_BYTES; ++b) {
        any |= s->dirty[row][b];
    }
    return any;
}

void screen_init(screen_t *s)
{
    u8 r, c;

    for (r = 0; r < ROWS; ++r) {
        for (c = 0; c < COLS; ++c) {
            s->cells[r][c].ch = BLANK_CH;
            s->cells[r][c].attr = 0;
        }
        screen_mark_row(s, r);
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
    s->scroll_seq = 0;
    s->last_scroll_top = 0;
    s->last_scroll_bot = ROWS - 1;
    s->last_scroll_n = 0;
}

void screen_putc(screen_t *s, u8 ch)
{
    if (s->wrap_pending) {            /* deferred wrap from a prior last-column write */
        screen_cr(s);                /* (clears wrap_pending, cx -> 0) */
        screen_lf(s);                /* down a row, scrolling at the region bottom */
    }
    s->cells[s->cy][s->cx].ch = ch;
    s->cells[s->cy][s->cx].attr = s->attr;
    screen_mark_cell(s, s->cy, s->cx);   /* mark before the cursor advances */
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
 * blanking freed rows (fully dirtied) and migrating each surviving row's
 * dirty marks along with its cells. Shared by screen_scroll (the whole scroll
 * region) and IL/DL (a region starting at the cursor row). */
static s8 effective_scroll_n(int top, int bot, int n)
{
    int height = bot - top + 1;
    int absn;

    if (n == 0 || height <= 0) {
        return 0;
    }
    absn = (n > 0) ? n : -n;
    if (absn > height) {
        absn = height;
    }
    return (s8)((n > 0) ? absn : -absn);
}

static void scroll_region(screen_t *s, int top, int bot, int n)
{
    int absn, r;

    n = effective_scroll_n(top, bot, n);
    if (n == 0) {
        return;
    }
    absn = (n > 0) ? n : -n;

    if (n > 0) {                       /* scroll up: content moves toward top */
        u8 keep = (u8)(bot - absn - top + 1);

        if (keep != 0) {
            memmove(&s->cells[top][0], &s->cells[top + absn][0],
                    (size_t)keep * COLS * sizeof(cell_t));
            memmove(&s->dirty[top][0], &s->dirty[top + absn][0],
                    (size_t)keep * DIRTY_BYTES);
        }
        for (r = bot - absn + 1; r <= bot; ++r) {
            blank_row(s, r);
            screen_mark_row(s, (u8)r);
        }
    } else {                           /* scroll down: content moves toward bot */
        u8 keep = (u8)(bot - (top + absn) + 1);

        if (keep != 0) {
            memmove(&s->cells[top + absn][0], &s->cells[top][0],
                    (size_t)keep * COLS * sizeof(cell_t));
            memmove(&s->dirty[top + absn][0], &s->dirty[top][0],
                    (size_t)keep * DIRTY_BYTES);
        }
        for (r = top; r <= top + absn - 1; ++r) {
            blank_row(s, r);
            screen_mark_row(s, (u8)r);
        }
    }
}

void screen_scroll(screen_t *s, s8 n)
{
    s8 actual = effective_scroll_n(s->top, s->bot, n);

    scroll_region(s, s->top, s->bot, actual);
    if (actual != 0) {
        ++s->scroll_seq;
        s->last_scroll_top = s->top;
        s->last_scroll_bot = s->bot;
        s->last_scroll_n = actual;
    }
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
    screen_mark_span(s, (u8)row, (u8)c0, (u8)c1);
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
    screen_mark_span(s, (u8)row, (u8)cx, (u8)(COLS - 1u));
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
    screen_mark_span(s, (u8)row, (u8)cx, (u8)(COLS - 1u));
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
    s->wrap_pending = 0;
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
    screen_cup(s, (s->mode & MODE_ORIGIN) ? top : 0, 0);  /* DECSTBM homes */
}
