/*
 * ula.c -- plain ZX Spectrum ULA screen-address math. See ula.h.
 */
#include "ula.h"

/* The "thirds" scanline-offset formula, shared by ula_addr() and
 * ula_row_scanline_offset() below -- identical to hires.c's copy (see ula.h). */
static u16 scanline_offset(u8 prow)
{
    return (u16)(((u16)(prow & 0xC0u) << 5)     /* which third      */
               | ((u16)(prow & 0x07u) << 8)     /* scanline in cell */
               | ((u16)(prow & 0x38u) << 2));   /* char row in third */
}

u16 ula_addr(u8 col, u8 prow)
{
    return (u16)(ULA_FILE + scanline_offset(prow) + (u16)col);
}

u16 ula_row_scanline_offset(u8 row, u8 scanline)
{
    u8 prow = (u8)((row << 3) + scanline);

    return scanline_offset(prow);
}
