/*
 * test_font.c -- host unit tests for the font lookup module.
 *
 * Test that font_glyph(ch) returns the correct glyph bytes for:
 * - ASCII range (0x20-0x7E)
 * - DEC graphics range (0xDF-0xFE, i.e. 0x5F-0x7E | 0x80)
 * - Fallback for invalid codes
 */
#include <assert.h>
#include <stdio.h>
#include "font.h"

static int checks = 0;
#define CHECK(cond) do { assert(cond); ++checks; } while (0)

/* Blank glyph should be all zeros. */
static void test_blank_glyph(void)
{
    const u8 *g = font_glyph(0x00);  /* invalid code -> blank */
    CHECK(g[0] == 0 && g[1] == 0 && g[2] == 0 && g[3] == 0);
    CHECK(g[4] == 0 && g[5] == 0 && g[6] == 0 && g[7] == 0);
}

/* Space (0x20) is first in FONT_ASCII. */
static void test_space(void)
{
    const u8 *g = font_glyph(0x20);
    CHECK(g[0] == 0 && g[1] == 0 && g[2] == 0 && g[3] == 0);
    CHECK(g[4] == 0 && g[5] == 0 && g[6] == 0 && g[7] == 0);
}

/* 'A' from the TT3000 ROM: body in rows 1-6, cell in bits 7..2. */
static void test_glyph_A(void)
{
    const u8 *g = font_glyph(0x41);  /* 'A' */
    CHECK(g[0] == 0x00);  /* ...... */
    CHECK(g[1] == 0x70);  /* .###.. */
    CHECK(g[2] == 0x88);  /* #...#. */
    CHECK(g[3] == 0x88);  /* #...#. */
    CHECK(g[4] == 0xF8);  /* #####. */
    CHECK(g[5] == 0x88);  /* #...#. */
    CHECK(g[6] == 0x88);  /* #...#. */
    CHECK(g[7] == 0x00);  /* ...... */
}

/* '~' (0x7E) is the last ASCII entry. */
static void test_glyph_tilde(void)
{
    const u8 *g = font_glyph(0x7E);  /* '~' */
    CHECK(g[0] == 0x00);
    CHECK(g[1] == 0x50);  /* .#.#.. */
    CHECK(g[2] == 0xA0);  /* #.#... */
    CHECK(g[3] == 0x00);
}

/*
 * At 6-px pitch a cell owns bits 7..2 only. A glyph that lights bit 1 or bit 0
 * would bleed into its right-hand neighbour, which reads as a renderer bug
 * rather than a font bug. Check the whole ASCII page, not a sample.
 */
static void test_no_glyph_exceeds_six_pixels_ascii(void)
{
    unsigned code;

    for (code = 0x20u; code <= 0x7Eu; ++code) {
        const u8 *g = font_glyph((u8)code);
        u8 i;

        for (i = 0; i < 8u; ++i) {
            CHECK((g[i] & 0x03u) == 0);
        }
    }
}

/*
 * Same containment property, but for the DEC graphics page (0xDF-0xFE). This
 * page is hand-authored pixel art in tools/genfont.py, which has no equivalent
 * of romfont.py's check_pitch() abort -- row_byte() there slices s[:8] and
 * silently accepts a 7- or 8-character pattern row, so this is the only place
 * that would ever catch a GRAPH entry that overruns its 6-px cell. Split from
 * the ASCII check above so a failing assertion names the page it failed in.
 */
static void test_no_glyph_exceeds_six_pixels_graph(void)
{
    unsigned code;

    for (code = 0xDFu; code <= 0xFEu; ++code) {
        const u8 *g = font_glyph((u8)code);
        u8 i;

        for (i = 0; i < 8u; ++i) {
            CHECK((g[i] & 0x03u) == 0);
        }
    }
}

static void check_glyph(const u8 *g, const u8 want[8])
{
    u8 i;
    for (i = 0; i < 8; ++i) {
        CHECK(g[i] == want[i]);
    }
}

/* DEC line drawing at 6-px pitch: vertical in column 2, horizontal across all six. */
static void test_glyph_graph_lines(void)
{
    static const u8 horiz[8] = { 0, 0, 0, 0xFC, 0, 0, 0, 0 };
    static const u8 vert[8]  = { 0x20, 0x20, 0x20, 0x20, 0x20, 0x20, 0x20, 0x20 };
    static const u8 ul[8]    = { 0, 0, 0, 0x3C, 0x20, 0x20, 0x20, 0x20 };
    static const u8 ur[8]    = { 0, 0, 0, 0xE0, 0x20, 0x20, 0x20, 0x20 };
    static const u8 ll[8]    = { 0x20, 0x20, 0x20, 0x3C, 0, 0, 0, 0 };
    static const u8 lr[8]    = { 0x20, 0x20, 0x20, 0xE0, 0, 0, 0, 0 };

    check_glyph(font_glyph(0xF1), horiz);  /* q: horizontal */
    check_glyph(font_glyph(0xF8), vert);   /* x: vertical */
    check_glyph(font_glyph(0xEC), ul);     /* l: upper-left */
    check_glyph(font_glyph(0xEB), ur);     /* k: upper-right */
    check_glyph(font_glyph(0xED), ll);     /* m: lower-left */
    check_glyph(font_glyph(0xEA), lr);     /* j: lower-right */
}

/* The half-lines must combine into a full run, or boxes show gaps at joints. */
static void test_graph_half_lines_join(void)
{
    const u8 *left  = font_glyph(0xEA);  /* j: lower-right corner, left half */
    const u8 *right = font_glyph(0xED);  /* m: lower-left corner, right half */

    CHECK((u8)(left[3] | right[3]) == 0xFC);
}

/* Cross and tees carry the vertical through every row. */
static void test_glyph_graph_cross(void)
{
    static const u8 cross[8] = { 0x20, 0x20, 0x20, 0xFC, 0x20, 0x20, 0x20, 0x20 };
    check_glyph(font_glyph(0xEE), cross);  /* n: crossing lines */
}

/* DEC graphics 0x7E: centred dot. */
static void test_glyph_graph_0x7E(void)
{
    const u8 *g = font_glyph(0xFE);
    CHECK(g[0] == 0x00);
    CHECK(g[1] == 0x00);
    CHECK(g[2] == 0x00);
    CHECK(g[3] == 0x30);  /* ..##.. */
    CHECK(g[4] == 0x30);  /* ..##.. */
    CHECK(g[5] == 0x00);
    CHECK(g[6] == 0x00);
    CHECK(g[7] == 0x00);
}

/* Invalid code below ASCII range returns blank. */
static void test_invalid_low(void)
{
    const u8 *g = font_glyph(0x1F);
    CHECK(g[0] == 0 && g[1] == 0 && g[2] == 0 && g[3] == 0);
    CHECK(g[4] == 0 && g[5] == 0 && g[6] == 0 && g[7] == 0);
}

/* Invalid code in the gap (0x7F-0xDE) returns blank. */
static void test_invalid_gap(void)
{
    const u8 *g = font_glyph(0x80);
    CHECK(g[0] == 0 && g[1] == 0 && g[2] == 0 && g[3] == 0);
    CHECK(g[4] == 0 && g[5] == 0 && g[6] == 0 && g[7] == 0);
}

int main(void)
{
    test_blank_glyph();
    test_space();
    test_glyph_A();
    test_glyph_tilde();
    test_no_glyph_exceeds_six_pixels_ascii();
    test_no_glyph_exceeds_six_pixels_graph();
    test_glyph_graph_lines();
    test_graph_half_lines_join();
    test_glyph_graph_cross();
    test_glyph_graph_0x7E();
    test_invalid_low();
    test_invalid_gap();

    printf("font: %d checks passed\n", checks);
    return 0;
}
