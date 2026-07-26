/*
 * render.h -- blit the screen grid to the hi-res display file.
 *
 * The render module knows two things:
 * 1. How to transform a glyph with attributes into 8 pixel rows (PURE logic).
 * 2. How to write those rows to the hi-res display file (hardware, via hires.c).
 *
 * PURE part (host-tested, no z80.h):
 * - render_cell_bytes(ch, attr, out[8]): produce 8 row bytes for one cell.
 *
 * Hardware part (ZEsarUX-tested only):
 * - render_flush(s): blit all dirty rows to display, clear dirty flags.
 * - render_cursor(s): draw/undraw cursor at current position.
 *
 * Per the toolchain rule, only the hardware functions may include z80.h.
 * The PURE functions are compiled on both host and target.
 */
#ifndef RENDER_H
#define RENDER_H

#include "types.h"
#include "screen.h"

/* Cell attribute bits (from screen.h, duplicated here for pure modules). */
#define ATTR_REVERSE   0x01u
#define ATTR_UNDERLINE 0x02u

/*
 * Screen geometry. 80 cells of 6 px are centred in the 512-px hi-res line:
 * a 16-px (2-byte) margin each side, cells occupying scanline bytes 2..61.
 * The margin is chosen byte-aligned so column 0 starts on a byte boundary.
 */
#define RENDER_LEFT_MARGIN_PX 16u
#define RENDER_CELL_PX        6u

/*
 * Where cell `col` lives inside a scanline.
 * byte_idx: scanline byte (0..63) holding the cell's leftmost pixel.
 * sh:       right shift taking a glyph byte (cell in bits 7..2) into place.
 * mask0:    bits this cell owns inside byte_idx.
 * mask1:    bits it owns inside byte_idx + 1; zero when the cell does not spill.
 *
 * PURE logic: host-tested, must not include z80.h.
 */
void render_cell_span(u8 col, u8 *byte_idx, u8 *sh, u8 *mask0, u8 *mask1);

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
 * Pack one full scanline into the two 32-byte display-file rows that hold it.
 * g80: COLS attribute-applied glyph bytes for one scanline, cell in bits 7..2,
 *      left to right (as produced by render_cell_bytes/glyph_row_byte per cell).
 * ev, od: the 32-byte destination rows for the even (file0) and odd (file1)
 *         display files; margin indices 0 and 31 are left untouched.
 *
 * Cells are grouped in eights because eight cells (48 px) span six scanline
 * bytes, which is three consecutive indices in each display file.
 * PURE logic: host-tested, must not include z80.h.
 */
void render_row_bytes(const u8 *g80, u8 *ev, u8 *od);

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

/*
 * Blit all dirty rows from the screen to the hi-res display file.
 * For each dirty row, render each cell and write via hires_addr().
 * Clears dirty flags after rendering.
 *
 * Hardware-only: uses hires_addr() and direct memory writes.
 */
void render_flush(screen_t *s);

/*
 * Fast hardware scroll for a full-width text-row region already reflected in
 * the screen model. Handles one text row up/down; returns non-zero if applied.
 * On success it also clears the region dirty flags because pixels were moved
 * directly in video RAM.
 */
u8 render_scroll_region(screen_t *s, u8 top, u8 bot, s8 n);

/*
 * Render the cursor at the current screen position.
 * When MODE_CURSOR_VISIBLE is set, invert the cell at (cx, cy).
 * Hardware-only.
 */
void render_cursor(const screen_t *s);

#endif /* RENDER_H */
