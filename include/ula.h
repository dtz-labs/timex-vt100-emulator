/*
 * ula.h -- plain ZX Spectrum ULA (256x192) screen-address math (pure,
 * host-tested).
 *
 * The single display file at 0x4000, 32 bytes per scanline, standard ZX
 * "thirds" interleave -- mirrors hires.c/hires.h, but there is only one file
 * to address, so no column parity test is needed. A text character cell is
 * 6 px wide and centred with an 8-px margin each side (see render_ula.c),
 * so `col` here is a scanline BYTE COLUMN (0..31), not a text cell.
 */
#ifndef ULA_H
#define ULA_H

#include "types.h"

/*
 * ULA_THIRDS_OFFSET(prow) -- identical formula to hires.h's
 * HIRES_THIRDS_OFFSET(), as a macro for the same reason: src/ula.c's
 * ula_addr()/ula_row_scanline_offset() AND src/blit_ula.c's scroll path share
 * this one expression. The scroll path used to keep its own hand-copy
 * (scroll_scanline_offset()) because a real cross-TU call there measured a
 * double-digit percentage cost on scroll_vram (see docs/perf/benchmarks.md,
 * "Task 9: moving row_scanline_offset out of blit_hires.c"); using this macro
 * directly there instead was re-measured, not assumed, and found to remove
 * that cost entirely rather than merely avoid regressing it -- see
 * docs/perf/benchmarks.md, "I5: macro/inline vs. hand-duplicated formulas".
 */
#define ULA_THIRDS_OFFSET(prow) \
    ( (u16)( ((u16)((u8)(prow) & 0xC0u) << 5) \
           | ((u16)((u8)(prow) & 0x07u) << 8) \
           | ((u16)((u8)(prow) & 0x38u) << 2) ) )

/* Address of pixel scanline `prow` (0..191) of scanline BYTE COLUMN `col`
 * (0..31) in the ULA display file. This is a byte column, not a text
 * character cell: at the 6-px cell pitch used by the terminal, a cell can
 * span one or two of these byte columns. See render_cell_span() in
 * render_geom.h/render_ula.c to map a text column onto the byte column(s)
 * to pass here. */
u16 ula_addr(u8 col, u8 prow);

/* Base address of the ULA display file. Mirrors HIRES_FILE0/1 (hires.h) so
 * src/blit_ula.c can compute ULA_FILE + offset the same way blit_hires.c
 * computes HIRES_FILE0/1 + offset. */
#define ULA_FILE 0x4000u

/*
 * Scanline-byte OFFSET (from ULA_FILE, not a full address -- there is no
 * column here) of pixel scanline `scanline` (0..7) of text row `row` (0..23).
 * Identical formula to hires_row_scanline_offset() (hires.h/hires.c): the ZX
 * "thirds" interleave is the same inside the ULA file as it is inside each
 * hi-res file. Factored out (Task 9) so src/blit_ula.c's row loop can compute
 * the 8 scanline offsets once per row, before its per-group loop, exactly as
 * blit_hires.c does.
 */
u16 ula_row_scanline_offset(u8 row, u8 scanline);

#endif /* ULA_H */
