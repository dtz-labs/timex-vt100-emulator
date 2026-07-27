/*
 * blit_hires.c -- write rendered glyph rows to the Timex hi-res display
 * files (hardware, ZEsarUX-tested only).
 *
 * See blit.h for the interface.
 */
#include "blit.h"
#include "render.h"
#include "render_geom.h"
#include "font.h"
#include "hires.h"
#include <stdint.h>
#include <string.h>

/* True when every cell of `row` is space/attr-0 -- the block-clear fast path
 * can then skip the font entirely. */
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
 * and 31, are left alone) in both display files, across all 8 scanlines. No
 * font data is consulted -- every cell is already known to be blank. */
static void blit_row_clear(u8 row)
{
    u8 i;

    for (i = 0; i < 8u; ++i) {
        u16 off = hires_row_scanline_offset(row, i);
        u8 *ev = (u8 *)(uintptr_t)(HIRES_FILE0 + off);
        u8 *od = (u8 *)(uintptr_t)(HIRES_FILE1 + off);

        memset(&ev[1], 0, 30u);
        memset(&od[1], 0, 30u);
    }
}

/*
 * Render only the dirty four-cell groups of `row`.
 *
 * The 8 scanline offsets (and the ev/od base pointers they produce) do not
 * depend on the group, only on `row` -- computed ONCE per row, before the
 * group loop, not once per group. Recomputing them per group was measured to
 * add ~120,000 T to a 20-group row (8x the calls: 160 instead of 8), which
 * would have silently defeated most of this task's point.
 *
 * Per group, the four cells' attributes are read once, up front, and their
 * glyph pointers looked up once (not once per scanline -- the same 8x trap
 * applies to font_glyph()):
 * - attr == 0 for all four: the inlined fast path below. No helper calls and
 *   no temporary g[8]/packed[3] arrays inside the scanline loop -- this
 *   distinction is the dominant per-row cost (review: inlining the packer
 *   alone took an 80-column row from 670,861 T to 194,617 T).
 * - otherwise: the general fallback through render_glyph_row_byte() and
 *   render_pack4(), exercised by REVERSE/UNDERLINE cells.
 *
 * Destination bytes come from render_group_bytes() (render_geom.h), the
 * host-tested group-to-scanline-byte mapping -- computed once per group
 * (not per scanline), so the even/odd file choice is a single branch per
 * group rather than a per-scanline test.
 */
static void blit_row_groups(const screen_t *s, u8 row)
{
    u16 offs[8];
    u8 g, i;

    for (i = 0; i < 8u; ++i) {
        offs[i] = hires_row_scanline_offset(row, i);
    }

    for (g = 0; g < DIRTY_GROUPS; ++g) {
        u8 col0, fi;
        const cell_t *c0, *c1, *c2, *c3;
        u8 idx0, idx1, idx2, ev_first;

        /*
         * DUPLICATED FORMULAS -- KNOWN, MEASURED, DELIBERATE. Read this
         * before changing screen_group_dirty() (src/screen.c) or
         * render_group_bytes() (src/render_hires.c): this block re-implements
         * both by hand and WILL NOT be caught by test/run.sh if you change
         * the originals and forget this copy, because blit_hires.c uses
         * absolute HIRES_FILE0/1 addresses and so cannot be host-compiled at
         * all -- see the constraint note on both original definitions.
         *
         * Why duplicated instead of called: calling render_group_bytes()
         * and screen_group_dirty() at GROUP granularity (once per group, 20
         * calls/row each, not once per scanline -- that would be the 8x trap
         * this file's header comment warns about) was tried and measured:
         * 267,267 T -> 286,382 T for a full 20-group fast-path row, +19,115 T
         * (~7%). That is a deterministic, reproducible cost (this emulator
         * has no run-to-run variance), not measurement noise, so it was
         * judged material against a row that already misses its 194,617 T
         * reference target -- see docs/perf/benchmarks.md, "Function-call
         * vs. inlined mapping/dirty-check" for the exact before/after and the
         * commands that produced them. Task 9's blit_ula.c brief must know
         * this shape is duplicated, not assume it can just call the shared
         * functions and match this file's speed.
         */
        if (!(s->dirty[row][g >> 3] & (u8)(1u << (g & 7u)))) {
            continue;
        }

        col0 = (u8)(g * DIRTY_GROUP_COLS);
        c0 = &s->cells[row][col0];
        c1 = c0 + 1;
        c2 = c0 + 2;
        c3 = c0 + 3;

        fi = (u8)(1u + 3u * (g >> 1));
        if ((g & 1u) == 0u) {
            idx0 = fi; idx1 = fi; idx2 = (u8)(fi + 1u);
            ev_first = 1u;
        } else {
            idx0 = (u8)(fi + 1u); idx1 = (u8)(fi + 2u); idx2 = (u8)(fi + 2u);
            ev_first = 0u;
        }

        if ((u8)(c0->attr | c1->attr | c2->attr | c3->attr) == 0u) {
            const u8 *fg0 = font_glyph(c0->ch);
            const u8 *fg1 = font_glyph(c1->ch);
            const u8 *fg2 = font_glyph(c2->ch);
            const u8 *fg3 = font_glyph(c3->ch);
            const u16 *op = offs;

            /* Walking pointers, not array indexing: fgN[i]/offs[i] made SDCC
             * re-derive a base+index address (with 8-bit carry propagation)
             * on every one of the 8 iterations; *fgN++ is a plain increment.
             * Measured effect on a full 20-group fast-path row: substantial
             * (see docs/perf/benchmarks.md). */
            for (i = 0; i < 8u; ++i) {
                u16 off = *op++;
                u8 *ev = (u8 *)(uintptr_t)(HIRES_FILE0 + off);
                u8 *od = (u8 *)(uintptr_t)(HIRES_FILE1 + off);
                u8 b0 = *fg0++, b1 = *fg1++, b2 = *fg2++, b3 = *fg3++;
                u8 p0 = (u8)((b0 & 0xFCu) | (b1 >> 6));
                u8 p1 = (u8)(((b1 << 2) & 0xF0u) | (b2 >> 4));
                u8 p2 = (u8)(((b2 << 4) & 0xC0u) | (b3 >> 2));

                if (ev_first) {
                    ev[idx0] = p0; od[idx1] = p1; ev[idx2] = p2;
                } else {
                    od[idx0] = p0; ev[idx1] = p1; od[idx2] = p2;
                }
            }
        } else {
            const u8 *fg0 = font_glyph(c0->ch);
            const u8 *fg1 = font_glyph(c1->ch);
            const u8 *fg2 = font_glyph(c2->ch);
            const u8 *fg3 = font_glyph(c3->ch);
            const u16 *op = offs;

            for (i = 0; i < 8u; ++i) {
                u16 off = *op++;
                u8 *ev = (u8 *)(uintptr_t)(HIRES_FILE0 + off);
                u8 *od = (u8 *)(uintptr_t)(HIRES_FILE1 + off);
                u8 g4[4];
                u8 packed[3];

                g4[0] = render_glyph_row_byte(fg0, c0->attr, i);
                g4[1] = render_glyph_row_byte(fg1, c1->attr, i);
                g4[2] = render_glyph_row_byte(fg2, c2->attr, i);
                g4[3] = render_glyph_row_byte(fg3, c3->attr, i);
                render_pack4(g4, packed);

                if (ev_first) {
                    ev[idx0] = packed[0]; od[idx1] = packed[1]; ev[idx2] = packed[2];
                } else {
                    od[idx0] = packed[0]; ev[idx1] = packed[1]; od[idx2] = packed[2];
                }
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
 * DUPLICATED FORMULA -- KNOWN, MEASURED, DELIBERATE. Same formula as
 * hires_row_scanline_offset() (src/hires.c), copied here rather than called.
 *
 * blit_row_groups()/blit_row_clear() above DO call hires_row_scanline_offset()
 * (Task 9 moved the original blit_hires.c-local static function there so
 * src/ula.c could share the identical "thirds" formula): those two call it at
 * most 8 times per row, and that measured as a negligible ~1,000 T/row
 * (row_normal 267,267 -> 268,283 T; see docs/perf/benchmarks.md). The scroll
 * primitives below are a different story: scroll_file_up_one/down_one call
 * it up to 2x per (row, scanline) pair, x2 files, over a 23-row region --
 * roughly 750 calls for one blit_scroll_region() -- and switching those calls
 * to the real cross-TU function measured as +95,504 T (680,099 -> 775,603 T,
 * ~14%) on `scroll_vram`, a path Tasks 3/4 already optimised and this task
 * has no business regressing. Reverted to a local duplicate for exactly this
 * loop; see docs/perf/benchmarks.md, "Scanline-offset call vs. duplicate in
 * the scroll path (Task 9)" for the numbers. Keep this in sync with
 * hires_row_scanline_offset() (src/hires.c) and hires_addr()'s own offset
 * math if the "thirds" formula ever changes -- test/run.sh cannot catch a
 * divergence here, since this file cannot be host-compiled.
 */
static u16 scroll_scanline_offset(u8 row, u8 scanline)
{
    u8 prow = (u8)((row << 3) + scanline);

    return (u16)(((u16)(prow & 0xC0u) << 5)
               | ((u16)(prow & 0x07u) << 8)
               | ((u16)(prow & 0x38u) << 2));
}

static void scroll_file_up_one(u16 base, u8 top, u8 bot)
{
    u8 row, scanline;

    for (row = top; row < bot; ++row) {
        for (scanline = 0; scanline < 8u; ++scanline) {
            u8 *dst = (u8 *)(uintptr_t)(base + scroll_scanline_offset(row, scanline));
            const u8 *src = (const u8 *)(uintptr_t)(base + scroll_scanline_offset((u8)(row + 1u), scanline));
            memcpy(dst, src, 32u);
        }
    }
    for (scanline = 0; scanline < 8u; ++scanline) {
        u8 *dst = (u8 *)(uintptr_t)(base + scroll_scanline_offset(bot, scanline));
        memset(dst, 0, 32u);
    }
}

static void scroll_file_down_one(u16 base, u8 top, u8 bot)
{
    u8 row, scanline;

    for (row = bot; row > top; --row) {
        for (scanline = 0; scanline < 8u; ++scanline) {
            u8 *dst = (u8 *)(uintptr_t)(base + scroll_scanline_offset(row, scanline));
            const u8 *src = (const u8 *)(uintptr_t)(base + scroll_scanline_offset((u8)(row - 1u), scanline));
            memcpy(dst, src, 32u);
        }
    }
    for (scanline = 0; scanline < 8u; ++scanline) {
        u8 *dst = (u8 *)(uintptr_t)(base + scroll_scanline_offset(top, scanline));
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
        scroll_file_up_one(HIRES_FILE0, top, bot);
        scroll_file_up_one(HIRES_FILE1, top, bot);
    } else if (n == -1) {
        scroll_file_down_one(HIRES_FILE0, top, bot);
        scroll_file_down_one(HIRES_FILE1, top, bot);
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
        u8 *p = (u8 *)(uintptr_t)hires_addr(byte_idx, prow);

        *p ^= mask0;
        if (mask1 != 0) {
            u8 *q = (u8 *)(uintptr_t)hires_addr((u8)(byte_idx + 1u), prow);
            *q ^= mask1;
        }
    }
}
