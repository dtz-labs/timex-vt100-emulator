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
 * Produce 8 pixel row bytes for a single cell.
 * ch: character code (ASCII 0x20-0x7E or graphics 0xDF-0xFE).
 * attr: ATTR_* bits (0 = normal, ATTR_REVERSE = invert, ATTR_UNDERLINE = bottom row).
 * out: output buffer of 8 bytes (scanline order, bit 7 = leftmost pixel).
 *
 * PURE logic: lookup glyph, apply REVERSE (~), apply UNDERLINE (set row 7 to 0xFF).
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
