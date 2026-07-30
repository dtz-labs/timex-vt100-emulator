/*
 * video_ula.c -- plain ZX Spectrum display setup (hardware; ZX build only).
 *
 * Monochrome by necessity: a cell is 6 px, an attribute block is 8 px, so a
 * colour change mid-line would corrupt the neighbouring cell. The attribute
 * file is filled once and never touched again -- which also means scrolling
 * never has to move attributes.
 */
#include "video.h"
#include <z80.h>

#define ULA_BITMAP 0x4000u
#define ULA_ATTRS 0x5800u
#define ULA_BITMAP_LEN 6144u
#define ULA_ATTRS_LEN 768u
#define ULA_ATTR_WHITE_ON_BLACK 0x47u   /* BRIGHT white ink, black paper */

void video_init(u8 wob)
{
    (void)wob;                      /* the ZX build is always white on black */
    z80_outp(0xFE, 0x00);           /* black border */
    video_clear();
}

void video_clear(void)
{
    volatile u8 *bitmap = (volatile u8 *)ULA_BITMAP;
    volatile u8 *attrs = (volatile u8 *)ULA_ATTRS;
    u16 i;

    for (i = 0; i < ULA_BITMAP_LEN; ++i) {
        bitmap[i] = 0;
    }
    for (i = 0; i < ULA_ATTRS_LEN; ++i) {
        attrs[i] = ULA_ATTR_WHITE_ON_BLACK;
    }
}
