/*
 * render.h -- the geometry-free pure core of the renderer: glyph lookup and
 * attribute application, plus the 6-px cell packing arithmetic shared by
 * every geometry.
 *
 * PURE part (host-tested, no z80.h):
 * - render_cell_bytes(ch, attr, out[8]): produce 8 row bytes for one cell.
 * - render_glyph_row_byte(glyph, attr, scanline): one glyph byte for one
 *   scanline, with attributes applied.
 * - render_pack4(g, out3): pack four cells' worth of pixels into 3 bytes.
 *
 * The geometry-dependent half (render_cell_span, render_row_bytes) lives in
 * render_geom.h, implemented once per machine (render_hires.c for the Timex
 * 80-column build). The hardware half (blitting to the display file) lives
 * in blit.h / blit_hires.c.
 *
 * Per the toolchain rule, only the hardware functions may include z80.h.
 * The PURE functions here are compiled on both host and target.
 */
#ifndef RENDER_H
#define RENDER_H

#include "types.h"
#include "screen.h"

/* Cell attribute bits (from screen.h, duplicated here for pure modules). */
#define ATTR_REVERSE   0x01u
#define ATTR_UNDERLINE 0x02u

/*
 * One glyph byte for one scanline, with attributes applied to the cell.
 * glyph: 8-byte glyph as returned by font_glyph().
 * attr: ATTR_* bits (0 = normal, ATTR_REVERSE = invert, ATTR_UNDERLINE = bottom row).
 * scanline: 0..7, row within the glyph.
 *
 * REVERSE inverts only the six pixels the cell owns (masked to 0xFC).
 * UNDERLINE sets row 7 to 0xFC. Neither attribute touches bits 1..0, which
 * belong to the next cell.
 *
 * Shared by every geometry, so it lives here rather than being static.
 * PURE logic: host-tested, must not include z80.h.
 */
u8 render_glyph_row_byte(const u8 *glyph, u8 attr, u8 scanline);

/*
 * Pack four cells into the three scanline bytes they occupy.
 * g:    four glyph bytes for one scanline, cell in bits 7..2, attributes applied.
 * out3: the three bytes, in left-to-right scanline order.
 *
 * 4 cells x 6 px = 24 px = exactly 3 bytes, so every shift here is a constant.
 * PURE logic: host-tested, must not include z80.h.
 */
void render_pack4(const u8 *g, u8 *out3);

/*
 * Produce 8 pixel row bytes for a single cell.
 * ch: character code (ASCII 0x20-0x7E or graphics 0xDF-0xFE).
 * attr: ATTR_* bits (0 = normal, ATTR_REVERSE = invert, ATTR_UNDERLINE = bottom row).
 * out: output buffer of 8 bytes (scanline order, bit 7 = leftmost pixel).
 *
 * PURE logic: lookup glyph, apply REVERSE (invert the 6 px the cell owns,
 * masked to 0xFC) and UNDERLINE (set row 7 to 0xFC) -- neither attribute
 * touches bits 1..0, which belong to the next cell.
 * This function is host-tested and must not include z80.h.
 */
void render_cell_bytes(u8 ch, u8 attr, u8 out[8]);

#endif /* RENDER_H */
