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

int main(void)
{
    test_init_blanks_grid_and_homes_cursor();
    test_putc_writes_cell_and_advances();
    printf("screen: %d checks passed\n", checks);
    return 0;
}
