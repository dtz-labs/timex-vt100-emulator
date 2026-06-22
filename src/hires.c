/*
 * hires.c -- Timex hi-res screen-address math. See hires.h.
 */
#include "hires.h"

u16 hires_addr(u8 col, u8 prow)
{
    u16 base = (col & 1u) ? HIRES_FILE1 : HIRES_FILE0;
    u16 bx   = (u16)(col >> 1);                       /* byte column 0..31 */
    u16 off  = ((u16)(prow & 0xC0u) << 5)             /* which third       */
             | ((u16)(prow & 0x07u) << 8)             /* scanline in cell  */
             | ((u16)(prow & 0x38u) << 2);            /* char row in third */
    return (u16)(base + off + bx);
}
