/*
 * hires.c -- Timex hi-res screen-address math. See hires.h.
 */
#include "hires.h"

/* The "thirds" scanline-offset formula is HIRES_THIRDS_OFFSET() (hires.h) --
 * a macro, not a local static function, so hires_addr()/
 * hires_row_scanline_offset() below and src/blit_hires.c's scroll path share
 * one expression instead of each keeping its own copy. See that macro's
 * comment. */

u16 hires_addr(u8 col, u8 prow)
{
    u16 base = (col & 1u) ? HIRES_FILE1 : HIRES_FILE0;
    u16 bx   = (u16)(col >> 1);                       /* byte column 0..31 */

    return (u16)(base + HIRES_THIRDS_OFFSET(prow) + bx);
}

u16 hires_row_scanline_offset(u8 row, u8 scanline)
{
    u8 prow = (u8)((row << 3) + scanline);

    return HIRES_THIRDS_OFFSET(prow);
}
