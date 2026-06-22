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

static void test_csi_cursor_moves(void)
{
    vtparse_t vt;
    screen_t s;
    vt_init(&vt);
    screen_init(&s);

    /* CUP (ESC[r;cH): 1-based params -> 0-based grid */
    feed(&vt, &s, "\x1b" "[5;10H");
    CHECK(s.cy == 4 && s.cx == 9);

    /* CUP with no params homes the cursor */
    feed(&vt, &s, "\x1b" "[H");
    CHECK(s.cy == 0 && s.cx == 0);

    /* HVP (ESC[r;cf) behaves like CUP */
    feed(&vt, &s, "\x1b" "[3;3f");
    CHECK(s.cy == 2 && s.cx == 2);

    /* CUU / CUD / CUF / CUB with explicit counts */
    screen_cup(&s, 10, 10);
    feed(&vt, &s, "\x1b" "[3A");
    CHECK(s.cy == 7 && s.cx == 10);
    feed(&vt, &s, "\x1b" "[2B");
    CHECK(s.cy == 9 && s.cx == 10);
    feed(&vt, &s, "\x1b" "[4C");
    CHECK(s.cy == 9 && s.cx == 14);
    feed(&vt, &s, "\x1b" "[5D");
    CHECK(s.cy == 9 && s.cx == 9);

    /* default count is 1 (ESC[A) */
    feed(&vt, &s, "\x1b" "[A");
    CHECK(s.cy == 8 && s.cx == 9);

    /* moves clamp at the edges, never wrap */
    screen_cup(&s, 0, 0);
    feed(&vt, &s, "\x1b" "[9A");       /* up past top -> row 0 */
    CHECK(s.cy == 0);
    feed(&vt, &s, "\x1b" "[9D");       /* left past col 0 -> col 0 */
    CHECK(s.cx == 0);
    screen_cup(&s, ROWS - 1, COLS - 1);
    feed(&vt, &s, "\x1b" "[99B");      /* down past bottom -> last row */
    CHECK(s.cy == ROWS - 1);
    feed(&vt, &s, "\x1b" "[99C");      /* right past end -> last col */
    CHECK(s.cx == COLS - 1);

    /* a leading empty param defaults to 1 (ESC[;10H -> row 1, col 10) */
    feed(&vt, &s, "\x1b" "[;10H");
    CHECK(s.cy == 0 && s.cx == 9);
}

static void test_csi_erase_and_edit(void)
{
    vtparse_t vt;
    screen_t s;
    vt_init(&vt);

    /* ED 2 (ESC[2J): clear the whole screen */
    screen_init(&s);
    feed(&vt, &s, "Hello");
    feed(&vt, &s, "\x1b" "[2J");
    CHECK(s.cells[0][0].ch == BLANK_CH);

    /* EL default = 0 (ESC[K): cursor..end of line */
    screen_init(&s);
    feed(&vt, &s, "ABCDEF");
    screen_cup(&s, 0, 3);
    feed(&vt, &s, "\x1b" "[K");
    CHECK(s.cells[0][2].ch == 'C');
    CHECK(s.cells[0][3].ch == BLANK_CH);

    /* EL 1 (ESC[1K): start..cursor inclusive */
    screen_init(&s);
    feed(&vt, &s, "ABCDEF");
    screen_cup(&s, 0, 3);
    feed(&vt, &s, "\x1b" "[1K");
    CHECK(s.cells[0][3].ch == BLANK_CH);
    CHECK(s.cells[0][4].ch == 'E');

    /* IL (ESC[L): insert a blank line at the cursor row, push rows down */
    screen_init(&s);
    feed(&vt, &s, "row0");
    screen_cup(&s, 0, 0);
    feed(&vt, &s, "\x1b" "[L");
    CHECK(s.cells[0][0].ch == BLANK_CH);
    CHECK(s.cells[1][0].ch == 'r');

    /* DL (ESC[M): delete the cursor row, pull rows up */
    screen_init(&s);
    screen_cup(&s, 1, 0);
    feed(&vt, &s, "second");
    screen_cup(&s, 0, 0);
    feed(&vt, &s, "\x1b" "[M");
    CHECK(s.cells[0][0].ch == 's');

    /* ICH (ESC[3@): insert 3 blanks at the cursor, shift right */
    screen_init(&s);
    feed(&vt, &s, "XYZ");
    screen_cup(&s, 0, 0);
    feed(&vt, &s, "\x1b" "[3@");
    CHECK(s.cells[0][0].ch == BLANK_CH);
    CHECK(s.cells[0][3].ch == 'X');

    /* DCH (ESC[2P): delete 2 chars at the cursor, shift left */
    screen_init(&s);
    feed(&vt, &s, "ABCDE");
    screen_cup(&s, 0, 0);
    feed(&vt, &s, "\x1b" "[2P");
    CHECK(s.cells[0][0].ch == 'C');
}

static void test_csi_sgr(void)
{
    vtparse_t vt;
    screen_t s;
    vt_init(&vt);
    screen_init(&s);

    feed(&vt, &s, "\x1b" "[7m");           /* reverse on */
    CHECK(s.attr == ATTR_REVERSE);

    feed(&vt, &s, "\x1b" "[4m");           /* + underline */
    CHECK(s.attr == (ATTR_REVERSE | ATTR_UNDERLINE));

    feed(&vt, &s, "\x1b" "[m");            /* ESC[m == ESC[0m, reset */
    CHECK(s.attr == 0);

    feed(&vt, &s, "\x1b" "[0;7;4m");       /* multi-param: reset+rev+underline */
    CHECK(s.attr == (ATTR_REVERSE | ATTR_UNDERLINE));

    feed(&vt, &s, "\x1b" "[27;24m");       /* clear reverse + underline */
    CHECK(s.attr == 0);

    feed(&vt, &s, "\x1b" "[7;1;31;42m");   /* bold/colour accepted & ignored */
    CHECK(s.attr == ATTR_REVERSE);

    /* the current SGR is stamped into subsequently written cells */
    feed(&vt, &s, "\x1b" "[0;7m");
    screen_cup(&s, 5, 5);
    feed(&vt, &s, "Q");
    CHECK(s.cells[5][5].attr == ATTR_REVERSE);
}

static void test_csi_scroll_region_and_modes(void)
{
    vtparse_t vt;
    screen_t s;
    vt_init(&vt);
    screen_init(&s);

    /* DECSTBM (ESC[5;20r): set region [4..19] 0-based and home the cursor */
    screen_cup(&s, 10, 10);
    feed(&vt, &s, "\x1b" "[5;20r");
    CHECK(s.top == 4 && s.bot == 19);
    CHECK(s.cy == 0 && s.cx == 0);

    /* DECSTBM reset (ESC[r): full screen */
    feed(&vt, &s, "\x1b" "[r");
    CHECK(s.top == 0 && s.bot == ROWS - 1);

    /* DECAWM off / on (ESC[?7l / ESC[?7h) */
    feed(&vt, &s, "\x1b" "[?7l");
    CHECK(!(s.mode & MODE_AUTOWRAP));
    feed(&vt, &s, "\x1b" "[?7h");
    CHECK(s.mode & MODE_AUTOWRAP);

    /* DECTCEM cursor hide / show (ESC[?25l / ESC[?25h) */
    feed(&vt, &s, "\x1b" "[?25l");
    CHECK(!(s.mode & MODE_CURSOR_VISIBLE));
    feed(&vt, &s, "\x1b" "[?25h");
    CHECK(s.mode & MODE_CURSOR_VISIBLE);

    /* multiple private modes in one sequence (ESC[?7;25l turns both off) */
    feed(&vt, &s, "\x1b" "[?7;25l");
    CHECK(!(s.mode & MODE_AUTOWRAP));
    CHECK(!(s.mode & MODE_CURSOR_VISIBLE));

    /* a non-private ANSI mode (ESC[4h IRM) is ignored, modes unchanged */
    feed(&vt, &s, "\x1b" "[?7;25h");
    feed(&vt, &s, "\x1b" "[4h");
    CHECK(s.mode & MODE_AUTOWRAP);
    CHECK(s.mode & MODE_CURSOR_VISIBLE);
}

/* Assert the parser's reply buffer holds exactly the given ASCII string. */
static void check_reply(const vtparse_t *vt, const char *want)
{
    u8 i = 0;
    while (want[i]) {
        CHECK(vt->out[i] == (u8)(unsigned char)want[i]);
        ++i;
    }
    CHECK(vt->nout == i);
}

static void test_csi_device_queries(void)
{
    vtparse_t vt;
    screen_t s;
    vt_init(&vt);
    screen_init(&s);

    /* DSR (ESC[6n) -> cursor position report ESC[{row};{col}R, 1-based */
    screen_cup(&s, 4, 9);
    feed(&vt, &s, "\x1b" "[6n");
    check_reply(&vt, "\x1b" "[5;10R");

    /* DA (ESC[c) -> VT-100 identity ESC[?1;0c */
    vt.nout = 0;
    feed(&vt, &s, "\x1b" "[c");
    check_reply(&vt, "\x1b" "[?1;0c");

    /* DSR with another param (ESC[5n) adds no reply in this subset */
    vt.nout = 0;
    feed(&vt, &s, "\x1b" "[5n");
    CHECK(vt.nout == 0);

    /* secondary DA (ESC[>c) is not answered with the primary identity */
    vt.nout = 0;
    feed(&vt, &s, "\x1b" "[>c");
    CHECK(vt.nout == 0);
}

static void test_charset_line_drawing(void)
{
    vtparse_t vt;
    screen_t s;
    vt_init(&vt);

    /* Default G0 = ASCII: 'q' is stored as-is */
    screen_init(&s);
    feed(&vt, &s, "q");
    CHECK(s.cells[0][0].ch == 'q');

    /* ESC(0 designates G0 = DEC special graphics: 0x5F-0x7E -> high-bit page */
    screen_init(&s);
    feed(&vt, &s, "\x1b" "(0");
    feed(&vt, &s, "q");
    CHECK(s.cells[0][0].ch == (u8)(0x71 | 0x80));

    /* bytes below 0x5F are unaffected in graphics mode */
    screen_cup(&s, 0, 1);
    feed(&vt, &s, "A");
    CHECK(s.cells[0][1].ch == 'A');

    /* ESC(B restores ASCII */
    feed(&vt, &s, "\x1b" "(B");
    screen_cup(&s, 0, 2);
    feed(&vt, &s, "q");
    CHECK(s.cells[0][2].ch == 'q');

    /* G1 designation + SO/SI select the active charset */
    screen_init(&s);
    feed(&vt, &s, "\x1b" ")0");          /* G1 = special graphics */
    vt_feed(&vt, &s, 0x0E);               /* SO -> G1 active */
    feed(&vt, &s, "x");
    CHECK(s.cells[0][0].ch == (u8)(0x78 | 0x80));
    vt_feed(&vt, &s, 0x0F);               /* SI -> G0 active (ASCII) */
    screen_cup(&s, 0, 1);
    feed(&vt, &s, "x");
    CHECK(s.cells[0][1].ch == 'x');
}

int main(void)
{
    test_printables_write_and_advance();
    test_c0_controls();
    test_esc_simple();
    test_csi_cursor_moves();
    test_csi_erase_and_edit();
    test_csi_sgr();
    test_csi_scroll_region_and_modes();
    test_csi_device_queries();
    test_charset_line_drawing();
    printf("vtparse: %d checks passed\n", checks);
    return 0;
}
