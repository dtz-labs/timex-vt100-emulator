/*
 * test_vtparse.c -- host unit tests for the VT-100/ANSI parser.
 *
 * Black-box: feed byte sequences, assert on the resulting screen grid / cursor
 * / attributes / reply buffer. Builds with the native compiler (test/run.sh).
 */
#include <assert.h>
#include <stdio.h>
#include "vtparse.h"
#include "screen.h"

static int checks = 0;
#define CHECK(cond) do { assert(cond); ++checks; } while (0)

/* Feed a NUL-terminated byte string through the parser (test strings never
 * contain a literal NUL; ESC is written as "\x1b"). */
static void feed(vtparse_t *vt, screen_t *s, const char *p)
{
    while (*p) {
        vt_feed(vt, s, (u8)(unsigned char)*p++);
    }
}

static void test_printables_write_and_advance(void)
{
    vtparse_t vt;
    screen_t s;
    vt_init(&vt);
    screen_init(&s);

    feed(&vt, &s, "Hi!");
    CHECK(s.cells[0][0].ch == 'H');
    CHECK(s.cells[0][1].ch == 'i');
    CHECK(s.cells[0][2].ch == '!');
    CHECK(s.cx == 3 && s.cy == 0);
}

static void test_c0_controls(void)
{
    vtparse_t vt;
    screen_t s;
    vt_init(&vt);
    screen_init(&s);

    feed(&vt, &s, "AB\r");                 /* CR -> column 0, row unchanged */
    CHECK(s.cx == 0 && s.cy == 0);

    feed(&vt, &s, "X\n");                  /* X over A; LF -> row 1, col kept */
    CHECK(s.cells[0][0].ch == 'X');
    CHECK(s.cy == 1 && s.cx == 1);

    /* BS moves left and stops at column 0 */
    screen_cup(&s, 1, 3);
    vt_feed(&vt, &s, 0x08);
    CHECK(s.cx == 2 && s.cy == 1);
    screen_cup(&s, 1, 0);
    vt_feed(&vt, &s, 0x08);
    CHECK(s.cx == 0);

    /* HT advances to the next multiple of 8, clamped to the last column */
    screen_cup(&s, 2, 0);
    vt_feed(&vt, &s, 0x09);
    CHECK(s.cx == 8);
    screen_cup(&s, 2, 8);
    vt_feed(&vt, &s, 0x09);
    CHECK(s.cx == 16);
    screen_cup(&s, 2, 60);
    vt_feed(&vt, &s, 0x09);
    CHECK(s.cx == COLS - 1);

    /* BEL is ignored (no cursor / cell change) */
    screen_cup(&s, 3, 5);
    vt_feed(&vt, &s, 0x07);
    CHECK(s.cx == 5 && s.cy == 3);

    /* FF behaves like LF */
    screen_cup(&s, 3, 5);
    vt_feed(&vt, &s, 0x0C);
    CHECK(s.cy == 4 && s.cx == 5);
}

static void test_esc_simple(void)
{
    vtparse_t vt;
    screen_t s;
    vt_init(&vt);
    screen_init(&s);

    /* IND (ESC D) = index (down one / scroll at region bottom) */
    screen_cup(&s, 2, 4);
    feed(&vt, &s, "\x1b" "D");
    CHECK(s.cy == 3 && s.cx == 4);

    /* RI (ESC M) = reverse index (up one / scroll at region top) */
    screen_cup(&s, 2, 4);
    feed(&vt, &s, "\x1b" "M");
    CHECK(s.cy == 1 && s.cx == 4);

    /* NEL (ESC E) = CR then LF */
    screen_cup(&s, 2, 9);
    feed(&vt, &s, "\x1b" "E");
    CHECK(s.cy == 3 && s.cx == 0);

    /* DECSC / DECRC (ESC 7 / ESC 8) = save / restore cursor + SGR */
    screen_cup(&s, 5, 7);
    screen_set_attr(&s, 7);
    feed(&vt, &s, "\x1b" "7");
    screen_cup(&s, 0, 0);
    screen_set_attr(&s, 0);
    feed(&vt, &s, "\x1b" "8");
    CHECK(s.cy == 5 && s.cx == 7);
    CHECK(s.attr == ATTR_REVERSE);

    /* RIS (ESC c) = hard reset: blank grid, home cursor, default modes/attr */
    feed(&vt, &s, "junk");
    screen_cup(&s, 4, 4);
    screen_set_attr(&s, 7);
    feed(&vt, &s, "\x1b" "c");
    CHECK(s.cy == 0 && s.cx == 0);
    CHECK(s.attr == 0);
    CHECK(s.cells[0][0].ch == BLANK_CH);
    CHECK(s.mode == (MODE_AUTOWRAP | MODE_CURSOR_VISIBLE));
}

int main(void)
{
    test_printables_write_and_advance();
    test_c0_controls();
    test_esc_simple();
    printf("vtparse: %d checks passed\n", checks);
    return 0;
}
