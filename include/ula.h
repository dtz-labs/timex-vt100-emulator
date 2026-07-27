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

/* Address of pixel scanline `prow` (0..191) of scanline BYTE COLUMN `col`
 * (0..31) in the ULA display file. This is a byte column, not a text
 * character cell: at the 6-px cell pitch used by the terminal, a cell can
 * span one or two of these byte columns. See render_cell_span() in
 * render_geom.h/render_ula.c to map a text column onto the byte column(s)
 * to pass here. */
u16 ula_addr(u8 col, u8 prow);

#endif /* ULA_H */
