/*
 * main.c -- M1 hi-res smoke test.
 *
 * Enters Timex hi-res (512x192), clears both display files, and draws ASCII
 * text on the 64x24 character grid using the Spectrum/Timex ROM 8x8 font and
 * our own hires_addr() math (hires.c). Proves the whole M1 path:
 *   +zx build -> .tap -> ZEsarUX as TC2048; SCLD hi-res mode; the hires.c
 *   address formula writing glyphs to the right place on the real machine.
 *
 * Palette: OUT (0xFF), 0x06 sets bits 0-2 = 110 (hi-res); bits 3-5 stay 000,
 * which is black-on-white per the WoS reference. White-on-black is a bits-3-5
 * change to be confirmed empirically on ZEsarUX (spec D9) -- this smoke test's
 * job is to prove geometry + addressing first; palette is the next knob.
 *
 * Poll-driven design (spec D7): no HALT, so the crt's interrupts-disabled boot
 * is harmless. The final loop just keeps the picture on screen.
 */
#include <stdint.h>
#include <z80.h>          /* z80_outp -- OUT (port), A */
#include "hires.h"

#define SCLD_PORT   0xFFu
#define HIRES_MODE  0x06u    /* bits0-2=110 hi-res; bits3-5=000; bits6-7=0 */
#define FILE_LEN    6144u

/* Spectrum/Timex ROM 8x8 font: the bitmap for char 0x20 (space) begins at
 * 0x3D00 (CHARS sysvar 0x3C00 + 0x20*8). 8 bytes per glyph, top row first. */
static const uint8_t *rom_glyph(uint8_t ch)
{
    if (ch < 0x20u || ch > 0x7Fu) {
        ch = 0x20u;
    }
    return (const uint8_t *)(0x3D00u + ((uint16_t)(ch - 0x20u) << 3));
}

/* Blit one 8x8 glyph into character cell (col, row): 8 font bytes down 8
 * consecutive scanlines, each addressed via hires_addr(). */
static void draw_glyph(uint8_t col, uint8_t row, uint8_t ch)
{
    const uint8_t *g = rom_glyph(ch);
    uint8_t i;
    for (i = 0; i < 8u; i++) {
        *(uint8_t *)hires_addr(col, (uint8_t)((row << 3) + i)) = g[i];
    }
}

static void draw_text(uint8_t col, uint8_t row, const char *s)
{
    while (*s != '\0' && col < 64u) {
        draw_glyph(col, row, (uint8_t)*s);
        col++;
        s++;
    }
}

int main(void)
{
    uint16_t i;

    z80_outp(SCLD_PORT, HIRES_MODE);          /* enter hi-res */

    for (i = 0; i < FILE_LEN; i++) {          /* clear both display files */
        *(uint8_t *)(HIRES_FILE0 + i) = 0x00u;
        *(uint8_t *)(HIRES_FILE1 + i) = 0x00u;
    }

    draw_text(0,  0, "TC2048 HI-RES 512x192 / 64 COLS x 24 ROWS");
    draw_text(0,  1, "VT-100 TERMINAL CORE -- M1 SMOKE OK");
    draw_text(0,  3, "ABCDEFGHIJKLMNOPQRSTUVWXYZ abcdefghijklmnopqrstuvwxyz");
    draw_text(0,  4, "0123456789  !\"#$%&'()*+,-./:;<=>?@[]^_");
    draw_text(50, 0, "col=50");
    draw_text(0, 23, "ROW 23 = BOTTOM LINE (64th col ->)");
    draw_text(63, 23, "X");                   /* far bottom-right corner cell */

    for (;;) {
    }
}
