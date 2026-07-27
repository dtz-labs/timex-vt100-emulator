/*
 * test_render.c -- host unit tests for the render module (PURE part only).
 *
 * Tests render_cell_bytes and render_glyph_row_byte (glyph lookup + attribute
 * transforms), render_cell_span and render_pack4 (6-px cell packing into
 * scanline bytes), and render_row_bytes (a full 80-column scanline against a
 * reference painter) -- this branch's primary correctness evidence for the
 * 80-column packing arithmetic. Hardware-touching functions (blit_flush,
 * blit_cursor_toggle, in src/blit_hires.c) are verified on the emulator.
 */
#include <assert.h>
#include <stdio.h>
#include "render.h"
#include "render_geom.h"
#include "font.h"
#include "screen.h"

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
/* Timex geometry: 16-px margin, 6-px cell pitch (private to render_hires.c;
 * mirrored here as literals since this reference painter must stay
 * independent of the packer it is checking). */
#define TEST_LEFT_MARGIN_PX 16u
#define TEST_CELL_PX        6u

static void paint_reference(const u8 *glyph_row, u8 ncols, u8 line[64])
{
    u8 col, k, i;

    for (i = 0; i < 64u; ++i) {
        line[i] = 0;
    }
    for (col = 0; col < ncols; ++col) {
        u16 px = (u16)(TEST_LEFT_MARGIN_PX + TEST_CELL_PX * (u16)col);

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
 * render_row_bytes is a piece of the 80-column blitter's packing arithmetic
 * (the fi = 1 + j*3 group-index arithmetic) with no other host coverage,
 * since the hardware functions that write pixels (blit_row_groups,
 * blit_row_clear, in src/blit_hires.c) write absolute video-RAM addresses
 * that only exist on target. Build one full scanline -- all four bit
 * phases, a blank cell, and a fully-lit cell -- and check it against
 * paint_reference exactly like test_pack4_matches_reference does for a
 * single group of four.
 */
static void test_row_bytes_matches_reference(void)
{
    static const u8 pattern[4] = { 0xFC, 0xA8, 0x54, 0x84 };
    u8 g80[80];
    u8 want[64];
    u8 ev[32], od[32];
    u8 col, b;

    for (col = 0; col < 80u; ++col) {
        if (col == 5u) {
            g80[col] = 0x00;  /* blank cell */
        } else if (col == 6u) {
            g80[col] = 0xFC;  /* fully-lit cell */
        } else {
            g80[col] = pattern[col % 4u];  /* cycles through all four phases */
        }
    }

    paint_reference(g80, 80u, want);

    /* Sentinel the destinations so an out-of-range write is caught too. */
    for (b = 0; b < 32u; ++b) {
        ev[b] = 0xAA;
        od[b] = 0xAA;
    }

    render_row_bytes(g80, ev, od);

    /* Scanline byte 2b lives in the even file at index b, byte 2b+1 in the odd
     * file at the same index -- de-interleave paint_reference's output to
     * compare against render_row_bytes' two 32-byte rows. */
    for (b = 1; b < 31u; ++b) {
        CHECK(ev[b] == want[b * 2u]);
        CHECK(od[b] == want[b * 2u + 1u]);
    }

    /* The 16-px margins (scanline bytes 0,1 and 62,63) are index 0 and 31 in
     * both files and must be left untouched by the packer. */
    CHECK(ev[0] == 0xAA);
    CHECK(od[0] == 0xAA);
    CHECK(ev[31] == 0xAA);
    CHECK(od[31] == 0xAA);
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

static void test_glyph_row_byte_is_shared(void)
{
    /* render_glyph_row_byte was static; it must now be callable so both
     * geometries can use one implementation. Row 1 of 'A' is 0x70 in the ROM
     * font; reverse inverts only the six owned pixels. */
    const u8 *a = font_glyph('A');

    CHECK(render_glyph_row_byte(a, 0, 1) == a[1]);
    CHECK(render_glyph_row_byte(a, ATTR_REVERSE, 1) == (u8)(~a[1] & 0xFCu));
    CHECK(render_glyph_row_byte(a, ATTR_UNDERLINE, 7) == 0xFCu);
    CHECK((render_glyph_row_byte(a, ATTR_REVERSE, 3) & 0x03u) == 0u);
}

/*
 * Task 5: the four-cell-group -> three-scanline-byte mapping that blit_flush's
 * fast path uses to write only the groups a row's dirty bitmap marks. Pure
 * arithmetic (no display memory needed), so it is host-testable even though
 * the blitter that consumes it is not. Every group's three bytes must be
 * distinct, inside the margins (index 1..30 -- 0 and 31 are the untouched
 * margin columns), and never claimed by two groups: that would mean one
 * group's write clobbers another's pixels.
 */
static void test_group_byte_mapping(void)
{
    u8 idx[3], file[3];
    u8 g, k;
    u8 seen_ev[32], seen_od[32];

    for (k = 0; k < 32u; ++k) { seen_ev[k] = 0; seen_od[k] = 0; }

    for (g = 0; g < DIRTY_GROUPS; ++g) {
        render_group_bytes(g, idx, file);
        for (k = 0; k < 3u; ++k) {
            CHECK(idx[k] >= 1u && idx[k] <= 30u);
            if (file[k] == 0u) {
                CHECK(seen_ev[idx[k]] == 0u);
                seen_ev[idx[k]] = 1u;
            } else {
                CHECK(seen_od[idx[k]] == 0u);
                seen_od[idx[k]] = 1u;
            }
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
    test_row_bytes_matches_reference();
    test_attributes_stay_inside_the_cell();
    test_glyph_row_byte_is_shared();
    test_group_byte_mapping();

    printf("render: %d checks passed\n", checks);
    return 0;
}
