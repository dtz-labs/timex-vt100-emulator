/*
 * render_geom.h -- the geometry-dependent half of the renderer.
 *
 * Exactly one implementation is linked: src/render_hires.c for the Timex
 * 80-column build, src/render_ula.c for the ZX 40-column build. They export the
 * same names, so they are alternatives and are never linked together.
 * PURE logic: host-tested, must not include z80.h.
 */
#ifndef RENDER_GEOM_H
#define RENDER_GEOM_H

#include "types.h"

/* Where cell `col` lives inside a scanline. byte_idx: scanline byte holding the
 * cell's leftmost pixel. sh: right shift taking a glyph byte (cell in bits 7..2)
 * into place. mask0: bits owned inside byte_idx. mask1: bits owned inside
 * byte_idx + 1, zero when the cell does not spill. */
void render_cell_span(u8 col, u8 *byte_idx, u8 *sh, u8 *mask0, u8 *mask1);

/* Pack one full scanline of COLS attribute-applied glyph bytes into the
 * destination row(s). The hi-res build writes two 32-byte display-file rows
 * (ev, od); the ULA build writes one and ignores `od`. */
void render_row_bytes(const u8 *g, u8 *ev, u8 *od);

/*
 * Destination bytes for four-cell dirty group `g`, in left-to-right scanline
 * order. Pure index arithmetic, not pointers: the caller supplies its own
 * `ev`/`od` (or single-file) row buffer, so this needs no display memory and
 * is host-testable.
 *
 * idx3: the three scanline-byte indices (1..30, inside the row's margin).
 * file3: which row buffer each index belongs to -- 0 for `ev` (or the ULA's
 *        single file), 1 for `od`. The ULA build (one file) sets file3 all 0.
 *
 * render_hires.c/render_ula.c export the same name so blit_hires.c/blit_ula.c
 * can be written in this exact shape.
 *
 * CONSTRAINT: neither implementation's blitter calls this function through
 * this declaration -- both measured a real cross-TU call here too expensive
 * for this hot per-group call site (see docs/perf/benchmarks.md,
 * "Function-call vs. inlined mapping/dirty-check" for hi-res, "ULA blitter:
 * duplicate vs. call (Task 9)" for the ULA build). Both instead use
 * RENDER_GROUP_BYTES_INLINE() below, a macro expressing this SAME formula per
 * geometry, single-sourced here rather than each blitter hand-copying its own
 * copy (re-measured after collapsing the hand-copies into this macro -- see
 * "I5: macro/inline vs. hand-duplicated formulas"). Neither blit_hires.c nor
 * blit_ula.c can be host-compiled (absolute HIRES_FILE0/1 / ULA_FILE
 * addresses), so test/run.sh will NOT catch a divergence if you change this
 * function without updating the matching branch of the macro below. */
void render_group_bytes(u8 g, u8 *idx3, u8 *file3);

/*
 * RENDER_GROUP_BYTES_INLINE(g, idx0, idx1, idx2, file0, file1, file2) --
 * the SAME index arithmetic as render_group_bytes() above, as a macro rather
 * than a function call, geometry-selected by the same TERM_TIMEX/TERM_ZX
 * macro every other geometry-dependent header switches on (see screen.h).
 * Written once here so src/blit_hires.c's and src/blit_ula.c's
 * blit_row_groups() can use ONE expression instead of each hand-copying its
 * own geometry's formula (see docs/perf/benchmarks.md, "I5: macro/inline vs.
 * hand-duplicated formulas" for the measurement that justifies this
 * collapse). render_group_bytes() itself is unchanged and stays the
 * host-tested, function-call form test_render.c exercises; this macro is a
 * second expression of the identical formula, for the one call site where a
 * real CALL was measured too expensive.
 */
#if defined(TERM_TIMEX)
#define RENDER_GROUP_BYTES_INLINE(g, idx0, idx1, idx2, file0, file1, file2) \
    do { \
        u8 render_group_bytes_fi_ = (u8)(1u + 3u * ((g) >> 1)); \
        if (((g) & 1u) == 0u) { \
            (idx0) = render_group_bytes_fi_;         (file0) = 0u; \
            (idx1) = render_group_bytes_fi_;         (file1) = 1u; \
            (idx2) = (u8)(render_group_bytes_fi_ + 1u); (file2) = 0u; \
        } else { \
            (idx0) = (u8)(render_group_bytes_fi_ + 1u); (file0) = 1u; \
            (idx1) = (u8)(render_group_bytes_fi_ + 2u); (file1) = 0u; \
            (idx2) = (u8)(render_group_bytes_fi_ + 2u); (file2) = 1u; \
        } \
    } while (0)
#elif defined(TERM_ZX)
#define RENDER_GROUP_BYTES_INLINE(g, idx0, idx1, idx2, file0, file1, file2) \
    do { \
        (idx0) = (u8)(1u + 3u * (g)); \
        (idx1) = (u8)((idx0) + 1u); \
        (idx2) = (u8)((idx0) + 2u); \
        (file0) = 0u; \
        (file1) = 0u; \
        (file2) = 0u; \
    } while (0)
#endif

#endif /* RENDER_GEOM_H */
