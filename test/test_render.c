/*
 * test_render.c -- host unit tests for the render module (PURE part only).
 *
 * Tests render_cell_bytes (glyph lookup + attribute transforms).
 * Hardware-touching functions (render_flush, render_cursor) are verified
 * on the emulator.
 */
#include <assert.h>
#include <stdio.h>
#include "render.h"

static int checks = 0;
#define CHECK(cond) do { assert(cond); ++checks; } while (0)

/* Normal cell: glyph bytes copied as-is. */
static void test_render_cell_normal(void)
{
    u8 out[8];
    render_cell_bytes('A', 0, out);

    /* 'A' from the TT3000 ROM: body in rows 1-6. */
    CHECK(out[0] == 0x00);
    CHECK(out[1] == 0x70);
    CHECK(out[2] == 0x88);
    CHECK(out[3] == 0x88);
    CHECK(out[4] == 0xF8);
    CHECK(out[5] == 0x88);
    CHECK(out[6] == 0x88);
    CHECK(out[7] == 0x00);
}

/* REVERSE inverts the glyph. */
static void test_render_cell_reverse(void)
{
    u8 plain[8];
    u8 rev[8];
    u8 i;

    render_cell_bytes('A', 0, plain);
    render_cell_bytes('A', ATTR_REVERSE, rev);

    for (i = 0; i < 8u; ++i) {
        CHECK(rev[i] == (u8)~plain[i]);
    }
}

/* UNDERLINE fills the bottom row and leaves the rest alone. */
static void test_render_cell_underline(void)
{
    u8 plain[8];
    u8 ul[8];
    u8 i;

    render_cell_bytes('A', 0, plain);
    render_cell_bytes('A', ATTR_UNDERLINE, ul);

    for (i = 0; i < 7u; ++i) {
        CHECK(ul[i] == plain[i]);
    }
    CHECK(ul[7] == 0xFF);
}

/* REVERSE + UNDERLINE: underline wins on the bottom row. */
static void test_render_cell_reverse_underline(void)
{
    u8 plain[8];
    u8 both[8];
    u8 i;

    render_cell_bytes('A', 0, plain);
    render_cell_bytes('A', (u8)(ATTR_REVERSE | ATTR_UNDERLINE), both);

    for (i = 0; i < 7u; ++i) {
        CHECK(both[i] == (u8)~plain[i]);
    }
    CHECK(both[7] == 0xFF);
}

/* DEC graphics: 0xF1 (0x71 | 0x80, q) maps to a horizontal line. */
static void test_render_cell_graphics(void)
{
    u8 out[8];
    render_cell_bytes(0xF1, 0, out);  /* GRAPH q, no attrs */

    CHECK(out[0] == 0x00);
    CHECK(out[1] == 0x00);
    CHECK(out[2] == 0x00);
    CHECK(out[3] == 0xFC);  /* horizontal run spans the 6-px cell */
    CHECK(out[4] == 0x00);
    CHECK(out[5] == 0x00);
    CHECK(out[6] == 0x00);
    CHECK(out[7] == 0x00);
}

/* Invalid code: blank glyph (all zeros). */
static void test_render_cell_invalid(void)
{
    u8 out[8];
    render_cell_bytes(0x01, 0, out);  /* invalid code */

    CHECK(out[0] == 0);
    CHECK(out[1] == 0);
    CHECK(out[2] == 0);
    CHECK(out[3] == 0);
    CHECK(out[4] == 0);
    CHECK(out[5] == 0);
    CHECK(out[6] == 0);
    CHECK(out[7] == 0);
}

/* Space (0x20) is blank glyph. */
static void test_render_cell_space(void)
{
    u8 out[8];
    render_cell_bytes(' ', 0, out);

    CHECK(out[0] == 0);
    CHECK(out[1] == 0);
    CHECK(out[2] == 0);
    CHECK(out[3] == 0);
    CHECK(out[4] == 0);
    CHECK(out[5] == 0);
    CHECK(out[6] == 0);
    CHECK(out[7] == 0);
}

int main(void)
{
    test_render_cell_normal();
    test_render_cell_reverse();
    test_render_cell_underline();
    test_render_cell_reverse_underline();
    test_render_cell_graphics();
    test_render_cell_invalid();
    test_render_cell_space();

    printf("render: %d checks passed\n", checks);
    return 0;
}
