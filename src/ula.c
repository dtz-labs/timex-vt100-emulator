/*
 * ula.c -- plain ZX Spectrum ULA screen-address math. See ula.h.
 */
#include "ula.h"

u16 ula_addr(u8 col, u8 prow)
{
    u16 off = ((u16)(prow & 0xC0u) << 5)      /* which third      */
            | ((u16)(prow & 0x07u) << 8)      /* scanline in cell */
            | ((u16)(prow & 0x38u) << 2);     /* char row in third */
    return (u16)(0x4000u + off + (u16)col);
}
