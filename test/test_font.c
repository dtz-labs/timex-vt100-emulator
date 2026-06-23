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

/* 'A' (0x41) is at index 0x41 - 0x20 = 0x21 = 33 in FONT_ASCII. */
static void test_glyph_A(void)
{
    const u8 *g = font_glyph(0x41);  /* 'A' */
    /* From genfont.py: 'A': [".###....", "#...#...", "#...#...", "#####...",
     *                        "#...#...", "#...#...", "#...#...", "........"] */
    CHECK(g[0] == 0x70);  /* .###.... = 01110000 */
    CHECK(g[1] == 0x88);  /* #...#... = 10001000 */
    CHECK(g[2] == 0x88);  /* #...#... = 10001000 */
    CHECK(g[3] == 0xF8);  /* #####... = 11111000 */
    CHECK(g[4] == 0x88);  /* #...#... = 10001000 */
    CHECK(g[5] == 0x88);  /* #...#... = 10001000 */
    CHECK(g[6] == 0x88);  /* #...#... = 10001000 */
    CHECK(g[7] == 0x00);  /* ........ = 00000000 */
}

/* '~' (0x7E) is last in ASCII range. */
static void test_glyph_tilde(void)
{
    const u8 *g = font_glyph(0x7E);  /* '~' */
    /* From genfont.py: "~": [".#..#...", "#.#.#...", "#..#....", "........", ...] */
    CHECK(g[0] == 0x48);  /* .#..#... = 01001000 */
    CHECK(g[1] == 0xA8);  /* #.#.#... = 10101000 */
    CHECK(g[2] == 0x90);  /* #..#.... = 10010000 */
}

static void check_glyph(const u8 *g, const u8 want[8])
{
    u8 i;
    for (i = 0; i < 8; ++i) {
        CHECK(g[i] == want[i]);
    }
}

/* DEC graphics line drawing uses the standard VT-100 letters. */
static void test_glyph_graph_lines(void)
{
    static const u8 horiz[8] = { 0, 0, 0, 0xFF, 0, 0, 0, 0 };
    static const u8 vert[8]  = { 0x10, 0x10, 0x10, 0x10, 0x10, 0x10, 0x10, 0x10 };
    static const u8 ul[8]    = { 0, 0, 0, 0x1F, 0x10, 0x10, 0x10, 0x10 };
    static const u8 ur[8]    = { 0, 0, 0, 0xF0, 0x10, 0x10, 0x10, 0x10 };
    static const u8 ll[8]    = { 0x10, 0x10, 0x10, 0x1F, 0, 0, 0, 0 };
    static const u8 lr[8]    = { 0x10, 0x10, 0x10, 0xF0, 0, 0, 0, 0 };

    check_glyph(font_glyph(0xF1), horiz);  /* q: horizontal */
    check_glyph(font_glyph(0xF8), vert);   /* x: vertical */
    check_glyph(font_glyph(0xEC), ul);     /* l: upper-left */
    check_glyph(font_glyph(0xEB), ur);     /* k: upper-right */
    check_glyph(font_glyph(0xED), ll);     /* m: lower-left */
    check_glyph(font_glyph(0xEA), lr);     /* j: lower-right */
}

/* DEC graphics: 0x7E (0xFE | 0x80) is last in GRAPH range. */
static void test_glyph_graph_0x7E(void)
{
    const u8 *g = font_glyph(0xFE);  /* GRAPH 0x7E | 0x80 */
    /* From genfont.py: 0x7E: ["........", "........", "...##...", "...##...", ...] */
    CHECK(g[0] == 0x00);
    CHECK(g[1] == 0x00);
    CHECK(g[2] == 0x18);  /* ...##... = 00011000 */
    CHECK(g[3] == 0x18);  /* ...##... = 00011000 */
    CHECK(g[4] == 0x00);
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
    test_glyph_graph_lines();
    test_glyph_graph_0x7E();
    test_invalid_low();
    test_invalid_gap();

    printf("font: %d checks passed\n", checks);
    return 0;
}
