/*
 * hires.h -- Timex hi-res (512x192) screen-address math (pure, host-tested).
 *
 * The z88dk tshr_* helpers are NOT linkable under +zx (verified, spec §2.1), so
 * we compute the address ourselves. Hi-res builds one 512x192 image from BOTH
 * display files: even character columns come from the 0x4000 file, odd columns
 * from the 0x6000 file. Within a file the bytes use the standard ZX "thirds"
 * interleave. A character cell is 8 px wide = exactly one byte (always aligned).
 */
#ifndef HIRES_H
#define HIRES_H

#include "types.h"

#define HIRES_FILE0 0x4000u   /* even character columns */
#define HIRES_FILE1 0x6000u   /* odd character columns  */

/* Address of pixel scanline `prow` (0..191) of character column `col` (0..63)
 * in the hi-res display files. */
u16 hires_addr(u8 col, u8 prow);

#endif /* HIRES_H */
