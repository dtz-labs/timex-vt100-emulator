/*
 * test_screen.c -- host unit tests for the terminal cell-grid model.
 *
 * Builds with the native macOS compiler (no emulator): see test/run.sh.
 * This is where the spec's "host unit tests (TDD lives here)" strategy lands.
 */
#include <assert.h>
#include <stdio.h>
#include "screen.h"

static int checks = 0;
#define CHECK(cond) do { assert(cond); ++checks; } while (0)

static void test_init_blanks_grid_and_homes_cursor(void)
{
    screen_t s;
    u8 r, c;

    screen_init(&s);

    for (r = 0; r < ROWS; ++r) {
        for (c = 0; c < COLS; ++c) {
            CHECK(s.cells[r][c].ch == BLANK_CH);
            CHECK(s.cells[r][c].attr == 0);
        }
    }
    CHECK(s.cx == 0 && s.cy == 0);
    CHECK(s.top == 0 && s.bot == ROWS - 1);
    CHECK(s.attr == 0);
    for (r = 0; r < ROWS; ++r) {
        CHECK(s.dirty[r] != 0);   /* a fresh screen needs painting */
    }
}

static void test_putc_writes_cell_and_advances(void)
{
    screen_t s;
    u8 r;

    screen_init(&s);
    for (r = 0; r < ROWS; ++r) {
        s.dirty[r] = 0;          /* clear so we can prove putc sets dirty */
    }
    s.attr = ATTR_REVERSE;

    screen_putc(&s, 'A');
    CHECK(s.cells[0][0].ch == 'A');
    CHECK(s.cells[0][0].attr == ATTR_REVERSE);
    CHECK(s.cx == 1 && s.cy == 0);
    CHECK(s.dirty[0] != 0);

    screen_putc(&s, 'B');
    CHECK(s.cells[0][1].ch == 'B');
    CHECK(s.cx == 2 && s.cy == 0);
}

static void test_cup_sets_clamped_cursor(void)
{
    screen_t s;
    screen_init(&s);

    screen_cup(&s, 5, 10);
    CHECK(s.cy == 5 && s.cx == 10);

    screen_cup(&s, 100, 200);          /* out of range -> clamp to edges */
    CHECK(s.cy == ROWS - 1 && s.cx == COLS - 1);

    screen_cup(&s, 0, 0);
    CHECK(s.cy == 0 && s.cx == 0);
}

/* Helper: stamp a distinct marker in column 0 of every row (A, B, C, ...). */
static void stamp_rows(screen_t *s)
{
    u8 r;
    for (r = 0; r < ROWS; ++r) {
        s->cells[r][0].ch = (u8)('A' + r);
    }
}

/* Helper: fill every cell with the same character. */
static void fill_all(screen_t *s, u8 ch)
{
    u8 r, c;
    for (r = 0; r < ROWS; ++r) {
        for (c = 0; c < COLS; ++c) {
            s->cells[r][c].ch = ch;
        }
    }
}

static void test_scroll_up_and_down_full_screen(void)
{
    screen_t s;
    screen_init(&s);
    stamp_rows(&s);

    screen_scroll(&s, 1);                              /* up by 1 */
    CHECK(s.cells[0][0].ch == 'B');                    /* row 0 <- old row 1 */
    CHECK(s.cells[ROWS - 2][0].ch == (u8)('A' + ROWS - 1));
    CHECK(s.cells[ROWS - 1][0].ch == BLANK_CH);        /* freed bottom blanked */
    CHECK(s.dirty[0] && s.dirty[ROWS - 1]);

    stamp_rows(&s);
    screen_scroll(&s, -1);                             /* down by 1 */
    CHECK(s.cells[ROWS - 1][0].ch == (u8)('A' + ROWS - 2));
    CHECK(s.cells[1][0].ch == 'A');                    /* row 1 <- old row 0 */
    CHECK(s.cells[0][0].ch == BLANK_CH);               /* freed top blanked */
}

static void test_scroll_respects_region_and_clamps(void)
{
    screen_t s;
    screen_init(&s);
    stamp_rows(&s);
    s.top = 2;
    s.bot = 5;                                         /* region rows 2..5 = C D E F */

    screen_scroll(&s, 1);                              /* up by 1 within [2,5] */
    CHECK(s.cells[1][0].ch == 'B');                    /* above region untouched */
    CHECK(s.cells[2][0].ch == 'D');                    /* row 2 <- old row 3 */
    CHECK(s.cells[5][0].ch == BLANK_CH);               /* freed bottom of region */
    CHECK(s.cells[6][0].ch == (u8)('A' + 6));          /* below region untouched */

    stamp_rows(&s);
    screen_scroll(&s, 100);                            /* |n| >= height -> blank all */
    CHECK(s.cells[2][0].ch == BLANK_CH);
    CHECK(s.cells[5][0].ch == BLANK_CH);
    CHECK(s.cells[1][0].ch == 'B');                    /* still outside region */
}

static void test_cr_returns_to_column_zero(void)
{
    screen_t s;
    screen_init(&s);
    screen_cup(&s, 3, 20);
    screen_cr(&s);
    CHECK(s.cx == 0 && s.cy == 3);          /* row unchanged */
}

static void test_lf_moves_down_then_scrolls_at_bottom(void)
{
    screen_t s;
    screen_init(&s);

    screen_cup(&s, 3, 5);
    screen_lf(&s);
    CHECK(s.cy == 4 && s.cx == 5);          /* down one, column kept */

    stamp_rows(&s);
    screen_cup(&s, ROWS - 1, 2);
    screen_lf(&s);                          /* at region bottom -> scroll up */
    CHECK(s.cy == ROWS - 1 && s.cx == 2);   /* cursor stays put */
    CHECK(s.cells[0][0].ch == 'B');         /* row 0 <- old row 1 */
    CHECK(s.cells[ROWS - 1][0].ch == BLANK_CH);
}

static void test_ri_moves_up_then_scrolls_at_top(void)
{
    screen_t s;
    screen_init(&s);

    screen_cup(&s, 3, 5);
    screen_ri(&s);
    CHECK(s.cy == 2 && s.cx == 5);          /* up one, column kept */

    stamp_rows(&s);
    screen_cup(&s, 0, 2);
    screen_ri(&s);                          /* at region top -> scroll down */
    CHECK(s.cy == 0 && s.cx == 2);          /* cursor stays put */
    CHECK(s.cells[1][0].ch == 'A');         /* row 1 <- old row 0 */
    CHECK(s.cells[0][0].ch == BLANK_CH);
}

static void test_erase_line_modes(void)
{
    screen_t s;
    u8 c;

    screen_init(&s);
    for (c = 0; c < COLS; ++c) {
        s.cells[2][c].ch = 'X';
    }
    s.cy = 2;
    s.cx = 10;

    screen_erase_line(&s, 0);                 /* cursor..end */
    CHECK(s.cells[2][9].ch == 'X');
    CHECK(s.cells[2][10].ch == BLANK_CH);
    CHECK(s.cells[2][COLS - 1].ch == BLANK_CH);
    CHECK(s.dirty[2]);

    for (c = 0; c < COLS; ++c) {
        s.cells[2][c].ch = 'X';
    }
    screen_erase_line(&s, 1);                 /* start..cursor (inclusive) */
    CHECK(s.cells[2][0].ch == BLANK_CH);
    CHECK(s.cells[2][10].ch == BLANK_CH);
    CHECK(s.cells[2][11].ch == 'X');

    for (c = 0; c < COLS; ++c) {
        s.cells[2][c].ch = 'X';
    }
    screen_erase_line(&s, 2);                 /* whole line */
    CHECK(s.cells[2][0].ch == BLANK_CH);
    CHECK(s.cells[2][COLS - 1].ch == BLANK_CH);
}

static void test_erase_display_modes(void)
{
    screen_t s;

    screen_init(&s);
    fill_all(&s, 'X');
    s.cy = 2;
    s.cx = 10;

    screen_erase_display(&s, 0);              /* cursor..end of screen */
    CHECK(s.cells[1][0].ch == 'X');          /* rows above kept */
    CHECK(s.cells[2][9].ch == 'X');          /* before cursor on row kept */
    CHECK(s.cells[2][10].ch == BLANK_CH);    /* cursor cell erased */
    CHECK(s.cells[3][0].ch == BLANK_CH);     /* rows below cleared */
    CHECK(s.dirty[2] && s.dirty[ROWS - 1]);

    fill_all(&s, 'X');
    s.cy = 2;
    s.cx = 10;
    screen_erase_display(&s, 1);             /* start..cursor (inclusive) */
    CHECK(s.cells[1][0].ch == BLANK_CH);     /* rows above cleared */
    CHECK(s.cells[2][10].ch == BLANK_CH);
    CHECK(s.cells[2][11].ch == 'X');         /* after cursor kept */
    CHECK(s.cells[3][0].ch == 'X');          /* rows below kept */

    fill_all(&s, 'X');
    screen_erase_display(&s, 2);             /* whole screen */
    CHECK(s.cells[0][0].ch == BLANK_CH);
    CHECK(s.cells[ROWS - 1][COLS - 1].ch == BLANK_CH);
}

static void test_insert_delete_lines(void)
{
    screen_t s;

    screen_init(&s);
    stamp_rows(&s);                            /* col 0 = A B C ... X */
    s.cy = 2;
    s.cx = 5;
    screen_insert_lines(&s, 1);                /* blank at row 2, rows shift down */
    CHECK(s.cells[1][0].ch == 'B');            /* above untouched */
    CHECK(s.cells[2][0].ch == BLANK_CH);       /* inserted blank line */
    CHECK(s.cells[3][0].ch == 'C');            /* old row 2 pushed to row 3 */
    CHECK(s.cx == 5 && s.cy == 2);             /* cursor unchanged */
    CHECK(s.dirty[2]);

    screen_init(&s);
    stamp_rows(&s);
    s.cy = 2;
    s.cx = 5;
    screen_delete_lines(&s, 1);                /* remove row 2, rows shift up */
    CHECK(s.cells[1][0].ch == 'B');
    CHECK(s.cells[2][0].ch == 'D');            /* old row 3 pulled up to row 2 */
    CHECK(s.cells[ROWS - 1][0].ch == BLANK_CH);/* freed bottom blanked */
    CHECK(s.cx == 5 && s.cy == 2);
}

static void test_insert_delete_chars(void)
{
    screen_t s;
    u8 c;

    screen_init(&s);
    for (c = 0; c < COLS; ++c) {
        s.cells[4][c].ch = (u8)('a' + (c % 26));
    }
    s.cy = 4;
    s.cx = 2;
    screen_insert_chars(&s, 3);                /* cols 2.. shift right by 3 */
    CHECK(s.cells[4][1].ch == (u8)('a' + 1));  /* before cursor unchanged ('b') */
    CHECK(s.cells[4][2].ch == BLANK_CH);
    CHECK(s.cells[4][4].ch == BLANK_CH);
    CHECK(s.cells[4][5].ch == (u8)('a' + 2));  /* old col 2 ('c') now at col 5 */
    CHECK(s.dirty[4]);

    for (c = 0; c < COLS; ++c) {
        s.cells[4][c].ch = (u8)('a' + (c % 26));
    }
    s.cx = 2;
    screen_delete_chars(&s, 3);                /* cols to the right shift left by 3 */
    CHECK(s.cells[4][1].ch == (u8)('a' + 1));  /* before cursor unchanged */
    CHECK(s.cells[4][2].ch == (u8)('a' + 5));  /* old col 5 ('f') now at col 2 */
    CHECK(s.cells[4][COLS - 1].ch == BLANK_CH);
    CHECK(s.cells[4][COLS - 3].ch == BLANK_CH);
}

static void test_set_attr_sgr(void)
{
    screen_t s;
    screen_init(&s);

    CHECK(s.attr == 0);                          /* fresh screen: no attrs */

    screen_set_attr(&s, 7);                      /* SGR 7: reverse on */
    CHECK(s.attr == ATTR_REVERSE);

    screen_set_attr(&s, 4);                      /* SGR 4: underline on */
    CHECK(s.attr == (ATTR_REVERSE | ATTR_UNDERLINE));

    screen_set_attr(&s, 27);                     /* SGR 27: reverse off */
    CHECK(s.attr == ATTR_UNDERLINE);

    screen_set_attr(&s, 24);                     /* SGR 24: underline off */
    CHECK(s.attr == 0);

    screen_set_attr(&s, 7);
    screen_set_attr(&s, 4);
    screen_set_attr(&s, 0);                      /* SGR 0: reset all */
    CHECK(s.attr == 0);

    screen_set_attr(&s, 7);
    screen_set_attr(&s, 1);                      /* bold: accepted & ignored */
    screen_set_attr(&s, 31);                     /* colour: accepted & ignored */
    CHECK(s.attr == ATTR_REVERSE);               /* unchanged by ignored codes */
}

static void test_save_restore_cursor(void)
{
    screen_t s;
    screen_init(&s);

    /* DECRC before any DECSC homes the cursor and clears attrs (init seeds
     * the saved slot with the VT-100 power-on default). */
    screen_cup(&s, 7, 9);
    s.attr = ATTR_REVERSE;
    screen_restore_cursor(&s);
    CHECK(s.cy == 0 && s.cx == 0);
    CHECK(s.attr == 0);

    /* DECSC then move then DECRC restores both position and attribute. */
    screen_cup(&s, 5, 10);
    screen_set_attr(&s, 7);                       /* reverse on */
    screen_save_cursor(&s);

    screen_cup(&s, 1, 1);
    screen_set_attr(&s, 0);                       /* reset */
    CHECK(s.cx == 1 && s.cy == 1 && s.attr == 0);

    screen_restore_cursor(&s);
    CHECK(s.cy == 5 && s.cx == 10);
    CHECK(s.attr == ATTR_REVERSE);
}

static void test_modes_default_and_toggle(void)
{
    screen_t s;
    screen_init(&s);

    /* Sensible power-on default: autowrap on, cursor visible. */
    CHECK(s.mode & MODE_AUTOWRAP);
    CHECK(s.mode & MODE_CURSOR_VISIBLE);
    CHECK(!(s.mode & MODE_CURSOR_APPLICATION));
    CHECK(!(s.mode & MODE_ORIGIN));
    CHECK(!(s.mode & MODE_INSERT));
    CHECK(!(s.mode & MODE_NEWLINE));

    screen_set_mode(&s, MODE_CURSOR_VISIBLE, 0);   /* DECTCEM reset: hide */
    CHECK(!(s.mode & MODE_CURSOR_VISIBLE));
    CHECK(s.mode & MODE_AUTOWRAP);                 /* other mode untouched */

    screen_set_mode(&s, MODE_AUTOWRAP, 0);         /* DECAWM reset */
    CHECK(!(s.mode & MODE_AUTOWRAP));

    screen_set_mode(&s, MODE_AUTOWRAP, 1);         /* DECAWM set */
    CHECK(s.mode & MODE_AUTOWRAP);
    CHECK(!(s.mode & MODE_CURSOR_VISIBLE));        /* still hidden */

    screen_set_mode(&s, MODE_CURSOR_APPLICATION, 1); /* DECCKM set */
    CHECK(s.mode & MODE_CURSOR_APPLICATION);
    screen_set_mode(&s, MODE_CURSOR_APPLICATION, 0); /* DECCKM reset */
    CHECK(!(s.mode & MODE_CURSOR_APPLICATION));

    screen_set_mode(&s, MODE_ORIGIN | MODE_INSERT | MODE_NEWLINE, 1);
    CHECK(s.mode & MODE_ORIGIN);
    CHECK(s.mode & MODE_INSERT);
    CHECK(s.mode & MODE_NEWLINE);
    screen_set_mode(&s, MODE_ORIGIN | MODE_INSERT | MODE_NEWLINE, 0);
    CHECK(!(s.mode & MODE_ORIGIN));
    CHECK(!(s.mode & MODE_INSERT));
    CHECK(!(s.mode & MODE_NEWLINE));
}

static void test_putc_deferred_wrap(void)
{
    screen_t s;

    /* Autowrap ON (default): a char in the last column is placed there and the
     * cursor parks. The wrap is deferred until the *next* printable char --
     * the classic VT-100 behaviour that lets the last column be filled without
     * scrolling. */
    screen_init(&s);
    screen_cup(&s, 0, COLS - 1);
    screen_putc(&s, 'X');
    CHECK(s.cells[0][COLS - 1].ch == 'X');
    CHECK(s.cx == COLS - 1 && s.cy == 0);          /* parked, not advanced */

    screen_putc(&s, 'Y');                          /* deferred wrap fires now */
    CHECK(s.cells[1][0].ch == 'Y');
    CHECK(s.cx == 1 && s.cy == 1);

    /* Autowrap OFF: the last column is overwritten in place, never wraps. */
    screen_init(&s);
    screen_set_mode(&s, MODE_AUTOWRAP, 0);
    screen_cup(&s, 0, COLS - 1);
    screen_putc(&s, 'X');
    screen_putc(&s, 'Y');
    CHECK(s.cells[0][COLS - 1].ch == 'Y');         /* overwrote X */
    CHECK(s.cells[1][0].ch == BLANK_CH);           /* no wrap */
    CHECK(s.cx == COLS - 1 && s.cy == 0);

    /* Deferred wrap on the bottom line scrolls the region up by one. */
    screen_init(&s);
    stamp_rows(&s);                                /* col 0 = A B C ... */
    screen_cup(&s, ROWS - 1, COLS - 1);
    screen_putc(&s, 'X');                          /* park at bottom-right */
    screen_putc(&s, 'Y');                          /* wrap -> scroll up */
    CHECK(s.cells[0][0].ch == 'B');                /* row 0 <- old row 1 */
    CHECK(s.cy == ROWS - 1 && s.cx == 1);          /* still on (new) last row */
    CHECK(s.cells[ROWS - 1][0].ch == 'Y');

    /* An explicit cursor move (CUP) clears the pending wrap. */
    screen_init(&s);
    screen_cup(&s, 0, COLS - 1);
    screen_putc(&s, 'X');                          /* wrap pending */
    screen_cup(&s, 5, 5);                          /* move clears the flag */
    screen_putc(&s, 'Z');
    CHECK(s.cells[5][5].ch == 'Z');
    CHECK(s.cx == 6 && s.cy == 5);
    CHECK(s.cells[1][0].ch == BLANK_CH);           /* the deferred wrap never ran */

    /* CR also clears the pending wrap. */
    screen_init(&s);
    screen_cup(&s, 2, COLS - 1);
    screen_putc(&s, 'X');                          /* wrap pending */
    screen_cr(&s);                                 /* col 0, flag cleared */
    screen_putc(&s, 'Z');
    CHECK(s.cells[2][0].ch == 'Z');                /* same row, col 0 -- no LF */
    CHECK(s.cx == 1 && s.cy == 2);
}

static void test_set_scroll_region(void)
{
    screen_t s;
    screen_init(&s);

    screen_cup(&s, 5, 5);
    screen_set_scroll_region(&s, 2, 8);
    CHECK(s.top == 2 && s.bot == 8);
    CHECK(s.cx == 0 && s.cy == 0);             /* DECSTBM homes the cursor */

    screen_set_scroll_region(&s, 3, 100);      /* bot past last row -> clamp */
    CHECK(s.top == 3 && s.bot == ROWS - 1);

    screen_set_scroll_region(&s, 10, 4);       /* degenerate -> ignored */
    CHECK(s.top == 3 && s.bot == ROWS - 1);
    screen_set_scroll_region(&s, 7, 7);        /* single line -> ignored */
    CHECK(s.top == 3 && s.bot == ROWS - 1);

    screen_set_scroll_region(&s, 0, ROWS - 1); /* reset to full screen */
    CHECK(s.top == 0 && s.bot == ROWS - 1);

    screen_set_mode(&s, MODE_ORIGIN, 1);
    screen_set_scroll_region(&s, 4, 9);
    CHECK(s.top == 4 && s.bot == 9);
    CHECK(s.cx == 0 && s.cy == 4);             /* DECOM home = top margin */
    screen_set_mode(&s, MODE_ORIGIN, 0);

    /* The set region actually governs scrolling (lf at region bottom). */
    screen_set_scroll_region(&s, 1, 3);
    stamp_rows(&s);                            /* col 0 = A B C D E ... */
    screen_cup(&s, 3, 0);                      /* bottom of region */
    screen_lf(&s);                             /* scroll region [1..3] up */
    CHECK(s.cells[1][0].ch == 'C');            /* row 1 <- old row 2 */
    CHECK(s.cells[3][0].ch == BLANK_CH);       /* freed bottom of region */
    CHECK(s.cells[0][0].ch == 'A');            /* above region untouched */
    CHECK(s.cells[4][0].ch == 'E');            /* below region untouched */
}

int main(void)
{
    test_init_blanks_grid_and_homes_cursor();
    test_putc_writes_cell_and_advances();
    test_cup_sets_clamped_cursor();
    test_scroll_up_and_down_full_screen();
    test_scroll_respects_region_and_clamps();
    test_cr_returns_to_column_zero();
    test_lf_moves_down_then_scrolls_at_bottom();
    test_ri_moves_up_then_scrolls_at_top();
    test_erase_line_modes();
    test_erase_display_modes();
    test_insert_delete_lines();
    test_insert_delete_chars();
    test_set_attr_sgr();
    test_save_restore_cursor();
    test_modes_default_and_toggle();
    test_putc_deferred_wrap();
    test_set_scroll_region();
    printf("screen: %d checks passed\n", checks);
    return 0;
}
