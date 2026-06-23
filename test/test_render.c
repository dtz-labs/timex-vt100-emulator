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
    render_cell_bytes('A', 0, out);  /* 'A', no attributes */

    /* 'A' from font: { 0x70, 0x88, 0x88, 0xF8, 0x88, 0x88, 0x88, 0x00 } */
    CHECK(out[0] == 0x70);
    CHECK(out[1] == 0x88);
    CHECK(out[2] == 0x88);
    CHECK(out[3] == 0xF8);
    CHECK(out[4] == 0x88);
    CHECK(out[5] == 0x88);
    CHECK(out[6] == 0x88);
    CHECK(out[7] == 0x00);
}

/* REVERSE: all glyph bytes inverted (~). */
static void test_render_cell_reverse(void)
{
    u8 out[8];
    render_cell_bytes('A', ATTR_REVERSE, out);

    CHECK(out[0] == (u8)~0x70);  /* 0x8F */
    CHECK(out[1] == (u8)~0x88);  /* 0x77 */
    CHECK(out[2] == (u8)~0x88);  /* 0x77 */
    CHECK(out[3] == (u8)~0xF8);  /* 0x07 */
    CHECK(out[4] == (u8)~0x88);  /* 0x77 */
    CHECK(out[5] == (u8)~0x88);  /* 0x77 */
    CHECK(out[6] == (u8)~0x88);  /* 0x77 */
    CHECK(out[7] == (u8)~0x00);  /* 0xFF */
}

/* UNDERLINE: bottom row set to 0xFF. */
static void test_render_cell_underline(void)
{
    u8 out[8];
    render_cell_bytes('A', ATTR_UNDERLINE, out);

    CHECK(out[0] == 0x70);
    CHECK(out[1] == 0x88);
    CHECK(out[2] == 0x88);
    CHECK(out[3] == 0xF8);
    CHECK(out[4] == 0x88);
    CHECK(out[5] == 0x88);
    CHECK(out[6] == 0x88);
    CHECK(out[7] == 0xFF);  /* underline */
}

/* REVERSE + UNDERLINE: inverted + bottom row 0xFF. */
static void test_render_cell_reverse_underline(void)
{
    u8 out[8];
    render_cell_bytes('A', ATTR_REVERSE | ATTR_UNDERLINE, out);

    CHECK(out[0] == (u8)~0x70);  /* 0x8F */
    CHECK(out[1] == (u8)~0x88);  /* 0x77 */
    CHECK(out[2] == (u8)~0x88);  /* 0x77 */
    CHECK(out[3] == (u8)~0xF8);  /* 0x07 */
    CHECK(out[4] == (u8)~0x88);  /* 0x77 */
    CHECK(out[5] == (u8)~0x88);  /* 0x77 */
    CHECK(out[6] == (u8)~0x88);  /* 0x77 */
    CHECK(out[7] == 0xFF);      /* underline set after reverse */
}

/* DEC graphics: 0xF1 (0x71 | 0x80, q) maps to a horizontal line. */
static void test_render_cell_graphics(void)
{
    u8 out[8];
    render_cell_bytes(0xF1, 0, out);  /* GRAPH q, no attrs */

    CHECK(out[0] == 0x00);
    CHECK(out[1] == 0x00);
    CHECK(out[2] == 0x00);
    CHECK(out[3] == 0xFF);
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
