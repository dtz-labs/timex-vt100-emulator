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

int main(void)
{
    test_init_blanks_grid_and_homes_cursor();
    test_putc_writes_cell_and_advances();
    test_cup_sets_clamped_cursor();
    test_scroll_up_and_down_full_screen();
    test_scroll_respects_region_and_clamps();
    printf("screen: %d checks passed\n", checks);
    return 0;
}
