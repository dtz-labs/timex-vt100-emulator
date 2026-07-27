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

#endif /* RENDER_GEOM_H */
