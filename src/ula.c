/*
 * ula.c -- plain ZX Spectrum ULA screen-address math. See ula.h.
 */
#include "ula.h"

/* The "thirds" scanline-offset formula is ULA_THIRDS_OFFSET() (ula.h) -- a
 * macro, not a local static function, so ula_addr()/ula_row_scanline_offset()
 * below and src/blit_ula.c's scroll path share one expression instead of each
 * keeping its own copy. See that macro's comment. */

u16 ula_addr(u8 col, u8 prow)
{
    return (u16)(ULA_FILE + ULA_THIRDS_OFFSET(prow) + (u16)col);
}

u16 ula_row_scanline_offset(u8 row, u8 scanline)
{
    u8 prow = (u8)((row << 3) + scanline);

    return ULA_THIRDS_OFFSET(prow);
}
