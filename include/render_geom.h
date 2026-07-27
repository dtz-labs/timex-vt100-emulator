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
 * The future ULA blitter (render_ula.c) exports the same name so blit_ula.c
 * can be written in this exact shape. */
void render_group_bytes(u8 g, u8 *idx3, u8 *file3);

#endif /* RENDER_GEOM_H */
