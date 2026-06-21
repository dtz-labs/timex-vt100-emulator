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
    printf("screen: %d checks passed\n", checks);
    return 0;
}
