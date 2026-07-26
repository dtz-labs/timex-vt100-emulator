/*
 * hires.h -- Timex hi-res (512x192) screen-address math (pure, host-tested).
 *
 * The z88dk tshr_* helpers are NOT linkable under +zx (verified, spec §2.1), so
 * we compute the address ourselves. Hi-res builds one 512x192 image from BOTH
 * display files: even scanline byte columns come from the 0x4000 file, odd
 * byte columns from the 0x6000 file. Within a file the bytes use the standard
 * ZX "thirds" interleave. A text character cell is 6 px wide and does NOT
 * align to a byte boundary in general -- it can span one or two of these byte
 * columns. See render_cell_span() in render.h/render.c for how a text column
 * maps onto the byte column(s) to pass here.
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
 * render.h/render.c to map a text column onto the byte column(s) to pass
 * here. */
u16 hires_addr(u8 col, u8 prow);

#endif /* HIRES_H */
