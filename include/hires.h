/*
 * hires.h -- Timex hi-res (512x192) screen-address math (pure, host-tested).
 *
 * The z88dk tshr_* helpers are NOT linkable under +zx (verified, spec §2.1), so
 * we compute the address ourselves. Hi-res builds one 512x192 image from BOTH
 * display files: even scanline byte columns come from the 0x4000 file, odd
 * byte columns from the 0x6000 file. Within a file the bytes use the standard
 * ZX "thirds" interleave. A text character cell is 6 px wide and does NOT
 * align to a byte boundary in general -- it can span one or two of these byte
 * columns. See render_cell_span() in render_geom.h/render_hires.c for how a
 * text column maps onto the byte column(s) to pass here.
 */
#ifndef HIRES_H
#define HIRES_H

#include "types.h"

#define HIRES_FILE0 0x4000u   /* even character columns */
#define HIRES_FILE1 0x6000u   /* odd character columns  */

/* Address of pixel scanline `prow` (0..191) of scanline BYTE COLUMN `col`
 * (0..63) in the hi-res display files. This is a byte column, not a text
 * character cell: at the 6-px cell pitch used by the terminal, a cell can
 * span one or two of these byte columns. See render_cell_span() in
 * render_geom.h/render_hires.c to map a text column onto the byte column(s)
 * to pass here. */
u16 hires_addr(u8 col, u8 prow);

/*
 * Scanline-byte OFFSET (from HIRES_FILE0/1, not a full address -- there is no
 * column here) of pixel scanline `scanline` (0..7) of text row `row` (0..23).
 * Same "thirds" interleave as hires_addr(), factored out because
 * src/blit_hires.c's row loop needs the offset alone (it already knows which
 * file each byte belongs to) and calls this once per row, before its
 * per-group loop -- not once per group, which Task 5 measured as costly.
 *
 * Moved here from a blit_hires.c-local static function (Task 9) because
 * src/ula.c's ula_row_scanline_offset() below needs the identical formula --
 * the ZX "thirds" interleave is the same inside a hi-res file as it is in the
 * ULA file -- so one pure, host-tested definition per module beats a third
 * hand-copy.
 */
u16 hires_row_scanline_offset(u8 row, u8 scanline);

#endif /* HIRES_H */
