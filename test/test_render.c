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
        CHECK(rev[i] == (u8)(~plain[i] & 0xFCu));
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
    CHECK(ul[7] == 0xFC);
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
        CHECK(both[i] == (u8)(~plain[i] & 0xFCu));
    }
    CHECK(both[7] == 0xFC);
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

/*
 * Cell geometry. 80 cells of 6 px are centred in the 512-px line, so cell 0
 * starts at pixel 16 (byte 2) and cell 79 ends at pixel 495 (byte 61). The bit
 * phase repeats every four columns; two of the four phases spill into the next
 * byte.
 */
static void test_cell_span_phases(void)
{
    u8 b, sh, m0, m1;

    render_cell_span(0, &b, &sh, &m0, &m1);
    CHECK(b == 2 && sh == 0 && m0 == 0xFC && m1 == 0x00);

    render_cell_span(1, &b, &sh, &m0, &m1);
    CHECK(b == 2 && sh == 6 && m0 == 0x03 && m1 == 0xF0);

    render_cell_span(2, &b, &sh, &m0, &m1);
    CHECK(b == 3 && sh == 4 && m0 == 0x0F && m1 == 0xC0);

    render_cell_span(3, &b, &sh, &m0, &m1);
    CHECK(b == 4 && sh == 2 && m0 == 0x3F && m1 == 0x00);

    /* The phase, not the byte, is what repeats: cell 4 is byte 5, phase 0. */
    render_cell_span(4, &b, &sh, &m0, &m1);
    CHECK(b == 5 && sh == 0 && m0 == 0xFC && m1 == 0x00);
}

/* The last cell must stay inside the line, leaving bytes 62 and 63 as margin. */
static void test_cell_span_last_column(void)
{
    u8 b, sh, m0, m1;

    render_cell_span(79, &b, &sh, &m0, &m1);
    CHECK(b == 61 && sh == 2 && m0 == 0x3F && m1 == 0x00);
}

/* Every cell owns exactly six pixels, and never reaches past byte 61. */
static void test_cell_span_covers_six_pixels(void)
{
    u8 col;

    for (col = 0; col < 80u; ++col) {
        u8 b, sh, m0, m1;
        u8 bits = 0;
        u8 i;

        render_cell_span(col, &b, &sh, &m0, &m1);
        for (i = 0; i < 8u; ++i) {
            if (m0 & (u8)(1u << i)) { ++bits; }
            if (m1 & (u8)(1u << i)) { ++bits; }
        }
        CHECK(bits == 6);
        CHECK(b >= 2u);
        CHECK((m1 != 0) ? (b + 1u <= 61u) : (b <= 61u));
    }
}

/*
 * Reference painter: plot each cell's six pixels one at a time into a 64-byte
 * scanline. Slow and obviously correct; the packer must agree with it.
 */
static void paint_reference(const u8 *glyph_row, u8 ncols, u8 line[64])
{
    u8 col, k, i;

    for (i = 0; i < 64u; ++i) {
        line[i] = 0;
    }
    for (col = 0; col < ncols; ++col) {
        u16 px = (u16)(RENDER_LEFT_MARGIN_PX + RENDER_CELL_PX * (u16)col);

        for (k = 0; k < 6u; ++k) {
            if (glyph_row[col] & (u8)(0x80u >> k)) {
                u16 q = (u16)(px + k);
                line[q >> 3] |= (u8)(0x80u >> (q & 7u));
            }
        }
    }
}

/* The packer must reproduce the reference painter for every bit phase. */
static void test_pack4_matches_reference(void)
{
    /* Four glyph bytes with distinct bit patterns inside the 6-px cell. */
    static const u8 g[4] = { 0xFC, 0xA8, 0x54, 0x84 };
    u8 want[64];
    u8 got[3];

    paint_reference(g, 4, want);
    render_pack4(g, got);

    CHECK(got[0] == want[2]);
    CHECK(got[1] == want[3]);
    CHECK(got[2] == want[4]);
}

/* A single lit cell must not disturb its neighbours' bytes. */
static void test_pack4_isolation(void)
{
    static const u8 only_second[4] = { 0x00, 0xFC, 0x00, 0x00 };
    u8 got[3];

    render_pack4(only_second, got);

    /* Cell 1 is phase 6: two bits low in byte 2, four bits high in byte 3. */
    CHECK(got[0] == 0x03);
    CHECK(got[1] == 0xF0);
    CHECK(got[2] == 0x00);
}

/* All cells fully lit must fill bytes 2..4 completely -- no gaps between cells. */
static void test_pack4_no_gaps(void)
{
    static const u8 all_on[4] = { 0xFC, 0xFC, 0xFC, 0xFC };
    u8 got[3];

    render_pack4(all_on, got);

    CHECK(got[0] == 0xFF);
    CHECK(got[1] == 0xFF);
    CHECK(got[2] == 0xFF);
}

/*
 * No attribute may light bits 1..0 -- those belong to the cell on the right.
 * Reverse is the dangerous one: inverting a whole byte sets them every time.
 */
static void test_attributes_stay_inside_the_cell(void)
{
    static const u8 attrs[3] = { ATTR_REVERSE, ATTR_UNDERLINE,
                                 (u8)(ATTR_REVERSE | ATTR_UNDERLINE) };
    u8 a, i;

    for (a = 0; a < 3u; ++a) {
        u8 out[8];

        render_cell_bytes('A', attrs[a], out);
        for (i = 0; i < 8u; ++i) {
            CHECK((out[i] & 0x03u) == 0);
        }
    }
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
    test_cell_span_phases();
    test_cell_span_last_column();
    test_cell_span_covers_six_pixels();
    test_pack4_matches_reference();
    test_pack4_isolation();
    test_pack4_no_gaps();
    test_attributes_stay_inside_the_cell();

    printf("render: %d checks passed\n", checks);
    return 0;
}
