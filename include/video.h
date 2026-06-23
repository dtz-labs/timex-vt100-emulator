/*
 * video.h -- Timex SCLD hi-res video setup (hardware-only, ZEsarUX-tested).
 *
 * Hi-res mode is selected by port 0xFF bits 0-2 = 110 (VMOD_HIRES = 6).
 * The palette (white-on-black vs black-on-white) is bits 3-5.
 * Bits 6-7 must be 0 (bit 6 = hardware DI).
 *
 * Per D9: base palette = white-on-black (black screen, white ink).
 * The exact bits-3-5 code for white-on-black is confirmed at M1 (0x38 = bits3-5=111).
 * Black-on-white (white screen) is bits 3-5 = 000.
 *
 * Functions:
 * - video_mode_byte(wob): compute mode byte for palette.
 * - video_hires_on(wob): enter hi-res mode with selected palette.
 * - video_clear(): clear both display files (0x4000 and 0x6000).
 *
 * This module uses z80_outp() from <z80.h> and is only tested on the target.
 */
#ifndef VIDEO_H
#define VIDEO_H

#include "types.h"

/* Port 0xFF bit definitions. */
#define VIDEO_MODE_HIRES  0x06u   /* bits 0-2 = 110 (hi-res mode) */

/*
 * Compute the mode byte for port 0xFF.
 * wob: non-zero = white-on-black (preferred), 0 = black-on-white.
 *
 * White-on-black: bits 3-5 = 111 (empirically confirmed at M1).
 * Black-on-white: bits 3-5 = 000 (screen is white).
 * Bits 0-2 = 110 (hi-res), bits 6-7 = 0.
 */
u8 video_mode_byte(u8 wob);

/*
 * Enter hi-res mode with the selected palette.
 * wob: non-zero = white-on-black, 0 = black-on-white.
 * Calls z80_outp(0xFF, video_mode_byte(wob)).
 */
void video_hires_on(u8 wob);

/*
 * Clear both display files to zeros (paper colour in both palettes).
 * Zeros 0x4000-0x57FF (file 0) and 0x6000-0x77FF (file 1).
 * Uses a simple loop; ~12KB total.
 */
void video_clear(void);

#endif /* VIDEO_H */
