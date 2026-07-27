/*
 * render.c -- the geometry-free pure core: glyph lookup and attribute
 * application, plus the shared 6-px cell packing arithmetic.
 *
 * See render.h for the PURE/hardware split.
 */
#include "render.h"
#include "font.h"

/* PURE: one glyph byte for one scanline, with attributes applied. */
u8 render_glyph_row_byte(const u8 *glyph, u8 attr, u8 scanline)
{
    u8 g = glyph[scanline];

    if (attr & ATTR_REVERSE) {
        /* Invert only the six pixels this cell owns. */
        g = (u8)(~g & 0xFCu);
    }
    if ((attr & ATTR_UNDERLINE) && scanline == 7u) {
        g = 0xFCu;
    }
    return g;
}

/* PURE: produce 8 pixel rows for one cell. */
void render_cell_bytes(u8 ch, u8 attr, u8 out[8])
{
    const u8 *glyph = font_glyph(ch);
    u8 i;

    for (i = 0; i < 8; ++i) {
        out[i] = render_glyph_row_byte(glyph, attr, i);
    }
}

/* PURE: 4 cells (24 px) pack into 3 bytes with constant shifts. */
void render_pack4(const u8 *g, u8 *out3)
{
    out3[0] = (u8)((g[0] & 0xFCu) | (g[1] >> 6));
    out3[1] = (u8)(((g[1] << 2) & 0xF0u) | (g[2] >> 4));
    out3[2] = (u8)(((g[2] << 4) & 0xC0u) | (g[3] >> 2));
}
