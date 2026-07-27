/*
 * video_hires.c -- Timex SCLD hi-res video setup (hardware-only).
 *
 * See video.h for the bit layout of port 0xFF.
 */
#include "video.h"
#include <z80.h>

#define DISPLAY_FILE0_SIZE 6144u  /* 0x4000-0x57FF */

u8 video_mode_byte(u8 wob)
{
    /* Bits 0-2 = 110 (hi-res), bits 3-5 = palette, bits 6-7 = 0. */
    if (wob) {
        /* White-on-black: bits 3-5 = 111 (empirically confirmed). */
        return 0x06u | 0x38u;  /* 00111110 = bits 0-2=110, bits 3-5=111 */
    } else {
        /* Black-on-white: bits 3-5 = 000. */
        return 0x06u;  /* 00000110 = bits 0-2=110 only */
    }
}

void video_init(u8 wob)
{
    z80_outp(0xFF, video_mode_byte(wob));
}

void video_clear(void)
{
    /* Clear both display files to 0. Use a volatile pointer for direct writes. */
    volatile u8 *file0 = (volatile u8 *)0x4000;
    volatile u8 *file1 = (volatile u8 *)0x6000;
    u16 i;

    for (i = 0; i < DISPLAY_FILE0_SIZE; ++i) {
        file0[i] = 0;
        file1[i] = 0;
    }
}
