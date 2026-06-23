/*
 * font.c -- 8x8 glyph lookup for the terminal (pure, host-tested).
 *
 * See font.h for the character mapping rules.
 */
#include "font.h"

/* Generated glyph data. */
#include "font_data.h"

/* Blank glyph for invalid codes and space. */
static const u8 blank_glyph[8] = {
    0, 0, 0, 0, 0, 0, 0, 0
};

const u8 *font_glyph(u8 ch)
{
    if (ch >= 0x20 && ch <= 0x7E) {
        /* ASCII printable range. */
        return FONT_ASCII[ch - 0x20];
    }
    if (ch >= 0xDF && ch <= 0xFE) {
        /* DEC graphics page: 0x5F-0x7E | 0x80. */
        u8 idx = (ch & 0x7F) - 0x5F;
        if (idx < (u8)(sizeof FONT_GRAPH / sizeof FONT_GRAPH[0])) {
            return FONT_GRAPH[idx];
        }
    }
    /* Fallback: blank glyph. */
    return blank_glyph;
}
