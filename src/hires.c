/*
 * hires.c -- Timex hi-res screen-address math. See hires.h.
 */
#include "hires.h"

/* The "thirds" scanline-offset formula, shared by hires_addr() and
 * hires_row_scanline_offset() below -- see hires.h. */
static u16 scanline_offset(u8 prow)
{
    return (u16)(((u16)(prow & 0xC0u) << 5)     /* which third       */
               | ((u16)(prow & 0x07u) << 8)     /* scanline in cell  */
               | ((u16)(prow & 0x38u) << 2));   /* char row in third */
}

u16 hires_addr(u8 col, u8 prow)
{
    u16 base = (col & 1u) ? HIRES_FILE1 : HIRES_FILE0;
    u16 bx   = (u16)(col >> 1);                       /* byte column 0..31 */

    return (u16)(base + scanline_offset(prow) + bx);
}

u16 hires_row_scanline_offset(u8 row, u8 scanline)
{
    u8 prow = (u8)((row << 3) + scanline);

    return scanline_offset(prow);
}
