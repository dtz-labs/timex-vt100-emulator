/*
 * blit_ula.c -- write rendered glyph rows to the plain ZX Spectrum ULA
 * display file (hardware, ZEsarUX-tested only).
 *
 * The single-file counterpart of blit_hires.c: one display file at ULA_FILE,
 * no even/odd column-parity split -- four cells (24 px) is exactly three
 * CONSECUTIVE scanline bytes here, where blit_hires.c's same four cells
 * alternate between two files. Written directly in the fast shape blit_hires.c
 * arrived at after Task 5 (dirty-group rendering, an inlined no-attribute
 * path, a blank-row block clear) -- never as a whole-row blitter meant to be
 * optimised later, which is the mistake this task ordering exists to prevent.
 *
 * See blit.h for the interface.
 */
#include "blit.h"
#include "render.h"
#include "render_geom.h"
#include "font.h"
#include "ula.h"
#include <stdint.h>
#include <string.h>

/* True when every cell of `row` is space/attr-0 -- the block-clear fast path
 * can then skip the font entirely. Identical to blit_hires.c's copy: this
 * check has no display-file dependency, so it is the one piece of shape that
 * cannot diverge between the two blitters. */
static u8 blit_row_is_blank(const screen_t *s, u8 row)
{
    const cell_t *c = &s->cells[row][0];
    u8 col;

    for (col = 0; col < COLS; ++col) {
        if (c->ch != BLANK_CH || c->attr != 0u) {
            return 0u;
        }
        ++c;
    }
    return 1u;
}

/* Block-clear a fully blank row: zero scanline bytes 1..30 (the margins, 0
 * and 31, are left alone) in the single display file, across all 8
 * scanlines. No font data is consulted -- every cell is already known to be
 * blank. */
static void blit_row_clear(u8 row)
{
    u8 i;

    for (i = 0; i < 8u; ++i) {
        u16 off = ula_row_scanline_offset(row, i);
        u8 *p = (u8 *)(uintptr_t)(ULA_FILE + off);

        memset(&p[1], 0, 30u);
    }
}

/*
 * Render only the dirty four-cell groups of `row`.
 *
 * The 8 scanline offsets are computed ONCE per row, before the group loop,
 * not once per group -- see blit_hires.c's header comment for the measured
 * cost (~120,000 T for a full row) of getting this wrong.
 *
 * Per group, the four cells' attributes are read once, up front, and their
 * glyph pointers looked up once (not once per scanline):
 * - attr == 0 for all four: the inlined fast path below, no helper calls or
 *   temporary arrays inside the scanline loop.
 * - otherwise: the general fallback through render_glyph_row_byte() and
 *   render_pack4(), exercised by REVERSE/UNDERLINE cells.
 */
static void blit_row_groups(const screen_t *s, u8 row)
{
    u16 offs[8];
    u8 g, i;

    for (i = 0; i < 8u; ++i) {
        offs[i] = ula_row_scanline_offset(row, i);
    }

    for (g = 0; g < DIRTY_GROUPS; ++g) {
        u8 col0, idx0;
        const cell_t *c0, *c1, *c2, *c3;

        /*
         * DUPLICATED FORMULAS -- KNOWN, MEASURED, DELIBERATE. Read this
         * before changing screen_group_dirty() (src/screen.c) or
         * render_group_bytes() (src/render_ula.c): this block re-implements
         * both by hand and WILL NOT be caught by test/run.sh if you change
         * the originals and forget this copy, because blit_ula.c uses an
         * absolute ULA_FILE address and so cannot be host-compiled at all --
         * see the CONSTRAINT note on both original definitions.
         *
         * Why duplicated instead of called: this is a fresh decision for the
         * ULA geometry, not an assumption carried over from blit_hires.c --
         * the ULA mapping is materially simpler (one file, no even/odd
         * branch), so it was measured rather than assumed. Calling
         * screen_group_dirty() + render_group_bytes() at GROUP granularity
         * (once per group, 10 calls/row each for the 40-column build, not
         * once per scanline) was tried and measured with an ad hoc
         * z88dk-ticks harness mirroring tools/bench/bench_row_normal.c (one
         * dirty row, 40 cells, attr == 0): 130,730 T duplicated vs. 143,181 T
         * calling, +12,451 T (~9.5%). Same deterministic-cost conclusion as
         * blit_hires.c's equivalent measurement (that one +19,115 T, ~7.2%,
         * over 20 groups instead of 10) -- see docs/perf/benchmarks.md,
         * "ULA blitter: duplicate vs. call (Task 9)" for the exact numbers
         * and the commands that produced them.
         */
        if (!(s->dirty[row][g >> 3] & (u8)(1u << (g & 7u)))) {
            continue;
        }

        col0 = (u8)(g * DIRTY_GROUP_COLS);
        c0 = &s->cells[row][col0];
        c1 = c0 + 1;
        c2 = c0 + 2;
        c3 = c0 + 3;

        /* Three CONSECUTIVE scanline bytes -- mirrors render_group_bytes()
         * (src/render_ula.c) exactly: idx3[k] = 1 + 3*g + k, file3[k] = 0. */
        idx0 = (u8)(1u + 3u * g);

        if ((u8)(c0->attr | c1->attr | c2->attr | c3->attr) == 0u) {
            const u8 *fg0 = font_glyph(c0->ch);
            const u8 *fg1 = font_glyph(c1->ch);
            const u8 *fg2 = font_glyph(c2->ch);
            const u8 *fg3 = font_glyph(c3->ch);
            const u16 *op = offs;

            /* Walking pointers, not array indexing -- same rationale as
             * blit_hires.c's identical choice (see its comment). */
            for (i = 0; i < 8u; ++i) {
                u16 off = *op++;
                u8 *p = (u8 *)(uintptr_t)(ULA_FILE + off);
                u8 b0 = *fg0++, b1 = *fg1++, b2 = *fg2++, b3 = *fg3++;

                p[idx0]     = (u8)((b0 & 0xFCu) | (b1 >> 6));
                p[idx0 + 1] = (u8)(((b1 << 2) & 0xF0u) | (b2 >> 4));
                p[idx0 + 2] = (u8)(((b2 << 4) & 0xC0u) | (b3 >> 2));
            }
        } else {
            const u8 *fg0 = font_glyph(c0->ch);
            const u8 *fg1 = font_glyph(c1->ch);
            const u8 *fg2 = font_glyph(c2->ch);
            const u8 *fg3 = font_glyph(c3->ch);
            const u16 *op = offs;

            for (i = 0; i < 8u; ++i) {
                u16 off = *op++;
                u8 *p = (u8 *)(uintptr_t)(ULA_FILE + off);
                u8 g4[4];
                u8 packed[3];

                g4[0] = render_glyph_row_byte(fg0, c0->attr, i);
                g4[1] = render_glyph_row_byte(fg1, c1->attr, i);
                g4[2] = render_glyph_row_byte(fg2, c2->attr, i);
                g4[3] = render_glyph_row_byte(fg3, c3->attr, i);
                render_pack4(g4, packed);

                p[idx0] = packed[0];
                p[idx0 + 1] = packed[1];
                p[idx0 + 2] = packed[2];
            }
        }
    }
}

void blit_flush(screen_t *s)
{
    u8 r;

    for (r = 0; r < ROWS; ++r) {
        if (!screen_row_dirty(s, r)) {
            continue;
        }
        if (blit_row_is_blank(s, r)) {
            blit_row_clear(r);       /* block clear, no font data touched */
        } else {
            blit_row_groups(s, r);  /* only the groups that changed */
        }
        screen_clear_marks(s, r);
    }
}

/*
 * DUPLICATED FORMULA -- KNOWN, MEASURED, DELIBERATE, mirroring blit_hires.c's
 * scroll_scanline_offset(). blit_row_groups()/blit_row_clear() above call
 * ula_row_scanline_offset() (src/ula.c) at most 8 times per row -- measured
 * negligible. The scroll primitives below call it up to 2x per (row,
 * scanline) pair over a 23-row region, ~370 calls for one
 * blit_scroll_region() (half of blit_hires.c's ~750, since there is only one
 * display file here) -- the same class of hot loop that measured +95,504 T
 * (~14%) on the Timex build when switched from a local duplicate to a real
 * call (see docs/perf/benchmarks.md, "Scanline-offset call vs. duplicate in
 * the scroll path (Task 9)"). Duplicated here for the same reason, rather
 * than assuming the ULA's simpler geometry makes the call free. Keep this in
 * sync with ula_row_scanline_offset() (src/ula.c) if the "thirds" formula
 * ever changes -- test/run.sh cannot catch a divergence here, since this
 * file cannot be host-compiled.
 */
static u16 scroll_scanline_offset(u8 row, u8 scanline)
{
    u8 prow = (u8)((row << 3) + scanline);

    return (u16)(((u16)(prow & 0xC0u) << 5)
               | ((u16)(prow & 0x07u) << 8)
               | ((u16)(prow & 0x38u) << 2));
}

static void scroll_up_one(u8 top, u8 bot)
{
    u8 row, scanline;

    for (row = top; row < bot; ++row) {
        for (scanline = 0; scanline < 8u; ++scanline) {
            u8 *dst = (u8 *)(uintptr_t)(ULA_FILE + scroll_scanline_offset(row, scanline));
            const u8 *src = (const u8 *)(uintptr_t)(ULA_FILE + scroll_scanline_offset((u8)(row + 1u), scanline));
            memcpy(dst, src, 32u);
        }
    }
    for (scanline = 0; scanline < 8u; ++scanline) {
        u8 *dst = (u8 *)(uintptr_t)(ULA_FILE + scroll_scanline_offset(bot, scanline));
        memset(dst, 0, 32u);
    }
}

static void scroll_down_one(u8 top, u8 bot)
{
    u8 row, scanline;

    for (row = bot; row > top; --row) {
        for (scanline = 0; scanline < 8u; ++scanline) {
            u8 *dst = (u8 *)(uintptr_t)(ULA_FILE + scroll_scanline_offset(row, scanline));
            const u8 *src = (const u8 *)(uintptr_t)(ULA_FILE + scroll_scanline_offset((u8)(row - 1u), scanline));
            memcpy(dst, src, 32u);
        }
    }
    for (scanline = 0; scanline < 8u; ++scanline) {
        u8 *dst = (u8 *)(uintptr_t)(ULA_FILE + scroll_scanline_offset(top, scanline));
        memset(dst, 0, 32u);
    }
}

u8 blit_scroll_region(screen_t *s, u8 top, u8 bot, s8 n)
{
    (void)s;  /* the model's dirty marks travel with the scroll; see blit.h */

    if (top >= bot || bot >= ROWS) {
        return 0;
    }
    if (n == 1) {
        scroll_up_one(top, bot);
    } else if (n == -1) {
        scroll_down_one(top, bot);
    } else {
        return 0;
    }

    return 1;
}

/* Hardware: invert the six pixels of the cursor cell, in place. */
void blit_cursor_toggle(const screen_t *s)
{
    u8 byte_idx, sh, mask0, mask1;
    u8 i;

    if (!(s->mode & MODE_CURSOR_VISIBLE)) {
        return;
    }

    /* The masks alone describe what to invert; the shift is not needed here. */
    render_cell_span(s->cx, &byte_idx, &sh, &mask0, &mask1);

    for (i = 0; i < 8u; ++i) {
        u8 prow = (u8)(s->cy * 8u + i);
        u8 *p = (u8 *)(uintptr_t)ula_addr(byte_idx, prow);

        *p ^= mask0;
        if (mask1 != 0) {
            u8 *q = (u8 *)(uintptr_t)ula_addr((u8)(byte_idx + 1u), prow);
            *q ^= mask1;
        }
    }
}
