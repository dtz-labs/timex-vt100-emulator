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
 * cost (42,106 T for a full 20-group row; see docs/perf/benchmarks.md, "The
 * row_normal gap: investigated, not silently accepted") of getting this
 * wrong.
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
        u8 col0, idx0, idx1, idx2, file0, file1, file2;
        const cell_t *c0, *c1, *c2, *c3;

        /*
         * Single-sourced macros, not hand-copied formulas: SCREEN_GROUP_DIRTY_BIT()
         * (screen.h) and RENDER_GROUP_BYTES_INLINE() (render_geom.h) express
         * the exact same dirty-bit test and index arithmetic screen_group_dirty()
         * and render_group_bytes() (src/render_ula.c) use, but as macros so no
         * CALL is emitted at this hot per-group call site. This replaces a
         * previous hand-duplicated copy of both formulas -- see
         * docs/perf/benchmarks.md, "I5: macro/inline vs. hand-duplicated
         * formulas" for the re-measurement that justified collapsing the
         * duplication into these macros (this is a fresh decision for the
         * ULA geometry, re-measured on its own terms, not an assumption
         * carried over from blit_hires.c's identical decision).
         */
        if (!SCREEN_GROUP_DIRTY_BIT(s, row, g)) {
            continue;
        }

        col0 = (u8)(g * DIRTY_GROUP_COLS);
        c0 = &s->cells[row][col0];
        c1 = c0 + 1;
        c2 = c0 + 2;
        c3 = c0 + 3;

        /* Three CONSECUTIVE scanline bytes, all in the single display file --
         * RENDER_GROUP_BYTES_INLINE's TERM_ZX branch always sets file0/1/2 to
         * 0, so only idx0 is used below (idx1/idx2 happen to equal
         * idx0+1/idx0+2, mirroring render_group_bytes()'s ULA formula). */
        RENDER_GROUP_BYTES_INLINE(g, idx0, idx1, idx2, file0, file1, file2);
        (void)idx1;
        (void)idx2;
        (void)file0;
        (void)file1;
        (void)file2;

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
 * The scroll path used to keep its own hand-copy of the "thirds" formula
 * (scroll_scanline_offset(), a local static function, mirroring
 * blit_hires.c's identical one) because a real cross-TU call to
 * ula_row_scanline_offset() here -- up to 2x per (row, scanline) pair over a
 * 23-row region, ~370 calls for one blit_scroll_region() (half of
 * blit_hires.c's ~750, since there is only one display file here) -- is the
 * same class of hot loop that measured +95,504 T (~14%) on the Timex build
 * when switched from a local duplicate to a real call (see
 * docs/perf/benchmarks.md, "Task 9: moving row_scanline_offset out of
 * blit_hires.c"). blit_row_groups()/blit_row_clear() above call
 * ula_row_scanline_offset() at most 8 times per row -- measured negligible.
 *
 * ULA_THIRDS_OFFSET() (ula.h) is a macro, not a function, so using it
 * directly here -- rather than keeping a second hand-copy of the same bit
 * arithmetic -- was re-measured rather than assumed safe: see
 * docs/perf/benchmarks.md, "I5: macro/inline vs. hand-duplicated formulas".
 * This is the SAME expression ula_row_scanline_offset() (src/ula.c) and
 * ula_addr() use, single-sourced in ula.h, so there is no longer a second
 * copy to keep in sync.
 *
 * RUN COALESCING. ULA_THIRDS_OFFSET() decomposes a pixel row as
 *
 *     offset = third * 2048 + scanline * 256 + row_in_third * 32
 *
 * so at a FIXED third and scanline the eight text rows are 32 bytes apart in
 * one contiguous 256-byte block. Scrolling by one row within a third is
 * therefore a single overlapping move of that block, not eight separate
 * 32-byte copies: the loop below walks scanline-major and coalesces each
 * maximal run of rows sharing a third.
 *
 * The saving is call and address-computation overhead, not copy speed --
 * LDIR is LDIR either way. A full 24-row region drops from 192 memcpy(32)
 * calls (each preceded by two ULA_THIRDS_OFFSET evaluations) to ~24 memmove
 * calls plus the boundary memcpys, which is where scroll_vram's cost
 * actually lived. Recommendation 5 of
 * docs/superpowers/reviews/2026-07-26-perf-review.md; measurements in
 * docs/perf/benchmarks.md, "Scroll run coalescing".
 */
static void scroll_up_one(u8 top, u8 bot)
{
    u8 scanline;

    for (scanline = 0; scanline < 8u; ++scanline) {
        u8 row = top;

        while (row < bot) {
            /* Rows sharing a third and a scanline are a contiguous run (see
             * the RUN COALESCING note above), so the whole run moves in one
             * memmove instead of `run` separate 32-byte copies. */
            u8 run = (u8)(7u - (row & 7u));
            u8 remaining = (u8)(bot - row);
            u8 *dst = (u8 *)(uintptr_t)(ULA_FILE + ULA_THIRDS_OFFSET((u8)((row << 3) + scanline)));
            const u8 *src = (const u8 *)(uintptr_t)(ULA_FILE + ULA_THIRDS_OFFSET((u8)(((row + 1u) << 3) + scanline)));

            if (run == 0u) {
                /* Last row of a third: its source lives in the NEXT third,
                 * 1,824 bytes away, so this one row cannot join a run. */
                memcpy(dst, src, 32u);
                ++row;
                continue;
            }
            if (run > remaining) {
                run = remaining;   /* the region ends before the third does */
            }
            /* Overlapping by 32 bytes with dst < src: memmove copies forward
             * (LDIR), which is exactly the direction this needs. */
            memmove(dst, src, (u16)run * 32u);
            row = (u8)(row + run);
        }
    }
    for (scanline = 0; scanline < 8u; ++scanline) {
        u8 *dst = (u8 *)(uintptr_t)(ULA_FILE + ULA_THIRDS_OFFSET((u8)((bot << 3) + scanline)));
        memset(dst, 0, 32u);
    }
}

static void scroll_down_one(u8 top, u8 bot)
{
    u8 scanline;

    for (scanline = 0; scanline < 8u; ++scanline) {
        u8 row = bot;

        while (row > top) {
            u8 run = (u8)(row & 7u);
            u8 remaining = (u8)(row - top);
            u8 *dst;
            const u8 *src;

            if (run == 0u) {
                /* First row of a third: its source lives in the PREVIOUS
                 * third, so this one row cannot join a run. */
                dst = (u8 *)(uintptr_t)(ULA_FILE + ULA_THIRDS_OFFSET((u8)((row << 3) + scanline)));
                src = (const u8 *)(uintptr_t)(ULA_FILE + ULA_THIRDS_OFFSET((u8)(((row - 1u) << 3) + scanline)));
                memcpy(dst, src, 32u);
                --row;
                continue;
            }
            if (run > remaining) {
                run = remaining;   /* the region ends before the third does */
            }
            /* Move rows [row-run+1 .. row] from [row-run .. row-1]: address
             * the run from its LOW end, since memmove takes the start of the
             * block, not the end. dst > src here, so memmove copies backward
             * (LDDR). */
            dst = (u8 *)(uintptr_t)(ULA_FILE + ULA_THIRDS_OFFSET((u8)(((row - run + 1u) << 3) + scanline)));
            src = (const u8 *)(uintptr_t)(ULA_FILE + ULA_THIRDS_OFFSET((u8)(((row - run) << 3) + scanline)));
            memmove(dst, src, (u16)run * 32u);
            row = (u8)(row - run);
        }
    }
    for (scanline = 0; scanline < 8u; ++scanline) {
        u8 *dst = (u8 *)(uintptr_t)(ULA_FILE + ULA_THIRDS_OFFSET((u8)((top << 3) + scanline)));
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
