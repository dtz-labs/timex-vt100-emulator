/*
 * render_ula.c -- 40-column geometry on the plain ZX Spectrum display file.
 *
 * 40 cells x 6 px = 240 px = 30 bytes, centred in the 32-byte scanline with an
 * 8-px margin each side, so cells occupy bytes 1..30 and bytes 0 and 31 are
 * never written. Four cells is 24 px is exactly 3 CONSECUTIVE bytes here --
 * simpler than hi-res, where the same three bytes alternate between two files.
 *
 * The alternative to render_hires.c: same symbols, never linked together.
 * PURE logic: host-tested, must not include z80.h.
 */
#include "render_geom.h"
#include "render.h"
#include "screen.h"

#define RENDER_LEFT_MARGIN_PX 8u
#define RENDER_CELL_PX 6u

/* PURE: locate one cell inside a scanline. */
void render_cell_span(u8 col, u8 *byte_idx, u8 *sh, u8 *mask0, u8 *mask1)
{
    u16 px = (u16)(RENDER_LEFT_MARGIN_PX + RENDER_CELL_PX * (u16)col);
    u8 s = (u8)(px & 7u);

    *byte_idx = (u8)(px >> 3);
    *sh = s;
    *mask0 = (u8)(0xFCu >> s);
    *mask1 = (s > 2u) ? (u8)((0xFCu << (8u - s)) & 0xFFu) : 0u;
}

/*
 * PURE: destination bytes for four-cell dirty group `g`.
 *
 * Four cells is 24 px is exactly three CONSECUTIVE scanline bytes here, all in
 * the single ULA display file -- no even/odd file split to track. This is the
 * single readable definition of the ULA group-to-byte mapping and the one
 * host tests exercise (test_group_byte_mapping, test/test_render.c).
 *
 * CONSTRAINT for anyone changing this formula: src/blit_ula.c's
 * blit_row_groups() does not call this function -- Task 9 measured calling
 * this function (and screen_group_dirty()) at group granularity against
 * duplicating it: 130,730 T vs. 143,181 T for a full 10-group fast-path row,
 * +12,451 T (~9.5%) (see docs/perf/benchmarks.md, "ULA blitter: duplicate vs.
 * call (Task 9)"). It instead uses RENDER_GROUP_BYTES_INLINE() (render_geom.h),
 * a macro expressing this SAME `idx0 = 1 + 3*g` arithmetic, single-sourced
 * next to this function's declaration in render_geom.h, re-measured against
 * the hand-duplicated version and found within noise or better (see
 * docs/perf/benchmarks.md, "I5: macro/inline vs. hand-duplicated formulas").
 * blit_ula.c cannot be host-compiled (absolute ULA_FILE address), so
 * test/run.sh will NOT catch a divergence between that macro and this
 * function if you change one without the other -- keep them in sync.
 */
void render_group_bytes(u8 g, u8 *idx3, u8 *file3)
{
    u8 k;

    /* Three consecutive bytes, all in the single display file. */
    for (k = 0; k < 3u; ++k) {
        idx3[k] = (u8)(1u + 3u * g + k);
        file3[k] = 0u;
    }
}

/*
 * PURE: pack one full scanline into the single 32-byte display-file row.
 *
 * g: COLS attribute-applied glyph bytes for one scanline (cell in bits 7..2),
 *    left to right.
 * ev: the 32-byte destination row; margin indices 0 and 31 are left untouched.
 * od: unused -- the ULA has only one display file, so there is nothing to
 *     interleave.
 */
void render_row_bytes(const u8 *g, u8 *ev, u8 *od)
{
    u8 j;

    (void)od;                      /* one display file: nothing to interleave */
    for (j = 0; j < DIRTY_GROUPS; ++j) {
        render_pack4(&g[j * 4u], &ev[1u + 3u * j]);
    }
}
