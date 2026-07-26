/*
 * render.c -- blit the screen grid to the hi-res display file.
 *
 * See render.h for the PURE/hardware split.
 */
#include "render.h"
#include "font.h"
#include "hires.h"
#include <stdint.h>
#include <string.h>

/* PURE: one glyph byte for one scanline, with attributes applied to the cell. */
static u8 glyph_row_byte(const u8 *glyph, u8 attr, u8 scanline)
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
        out[i] = glyph_row_byte(glyph, attr, i);
    }
}

/* PURE: 4 cells (24 px) pack into 3 bytes with constant shifts. */
void render_pack4(const u8 *g, u8 *out3)
{
    out3[0] = (u8)((g[0] & 0xFCu) | (g[1] >> 6));
    out3[1] = (u8)(((g[1] << 2) & 0xF0u) | (g[2] >> 4));
    out3[2] = (u8)(((g[2] << 4) & 0xC0u) | (g[3] >> 2));
}

/* PURE: locate one cell inside a scanline. */
void render_cell_span(u8 col, u8 *byte_idx, u8 *sh, u8 *mask0, u8 *mask1)
{
    u16 px = (u16)(RENDER_LEFT_MARGIN_PX + RENDER_CELL_PX * (u16)col);
    u8 s = (u8)(px & 7u);

    *byte_idx = (u8)(px >> 3);
    *sh = s;
    *mask0 = (u8)(0xFCu >> s);
    *mask1 = (s > 2u) ? (u8)((0xFCu << (8u - s)) & 0xFFu) : 0u;
}

/*
 * Blit one text row.
 *
 * Glyph lookup is hoisted out of the scanline loop on purpose: doing it inside
 * would cost 640 lookups per row instead of 80.
 *
 * Cells are walked in groups of eight because eight cells span six bytes, which
 * is three CONSECUTIVE indices in each display file -- so both files are written
 * sequentially with no parity test in the inner loop.
 */
static const u8 *row_glyphs[COLS];
static u8 row_attrs[COLS];

static void render_row_fast(const screen_t *s, u8 r)
{
    u8 *even_dst[8];
    u8 *odd_dst[8];
    const cell_t *cell = &s->cells[r][0];
    u8 col, i, j;

    for (col = 0; col < COLS; ++col) {
        row_glyphs[col] = font_glyph(cell->ch);
        row_attrs[col] = cell->attr;
        ++cell;
    }

    for (i = 0; i < 8u; ++i) {
        u8 prow = (u8)((r << 3) + i);
        u16 off = ((u16)(prow & 0xC0u) << 5)
                | ((u16)(prow & 0x07u) << 8)
                | ((u16)(prow & 0x38u) << 2);
        even_dst[i] = (u8 *)(uintptr_t)(HIRES_FILE0 + off);
        odd_dst[i] = (u8 *)(uintptr_t)(HIRES_FILE1 + off);
    }

    for (i = 0; i < 8u; ++i) {
        u8 *ev = even_dst[i];
        u8 *od = odd_dst[i];

        for (j = 0; j < 10u; ++j) {
            u8 base = (u8)(j * 8u);   /* first cell of this group          */
            u8 fi = (u8)(1u + j * 3u); /* first file index of this group    */
            u8 g[8];
            u8 packed[3];
            u8 k;

            for (k = 0; k < 8u; ++k) {
                g[k] = glyph_row_byte(row_glyphs[base + k], row_attrs[base + k], i);
            }

            /* Scanline bytes 2+6j .. 4+6j. */
            render_pack4(&g[0], packed);
            ev[fi] = packed[0];
            od[fi] = packed[1];
            ev[fi + 1u] = packed[2];

            /* Scanline bytes 5+6j .. 7+6j. */
            render_pack4(&g[4], packed);
            od[fi + 1u] = packed[0];
            ev[fi + 2u] = packed[1];
            od[fi + 2u] = packed[2];
        }
    }
}

void render_flush(screen_t *s)
{
    u8 r;

    for (r = 0; r < ROWS; ++r) {
        if (!s->dirty[r]) {
            continue;
        }
        render_row_fast(s, r);
        s->dirty[r] = 0;  /* clear dirty flag */
    }
}

static u16 row_scanline_offset(u8 row, u8 scanline)
{
    u8 prow = (u8)((row << 3) + scanline);

    return (u16)(((u16)(prow & 0xC0u) << 5)
               | ((u16)(prow & 0x07u) << 8)
               | ((u16)(prow & 0x38u) << 2));
}

static void scroll_file_up_one(u16 base, u8 top, u8 bot)
{
    u8 row, scanline;

    for (row = top; row < bot; ++row) {
        for (scanline = 0; scanline < 8u; ++scanline) {
            u8 *dst = (u8 *)(uintptr_t)(base + row_scanline_offset(row, scanline));
            const u8 *src = (const u8 *)(uintptr_t)(base + row_scanline_offset((u8)(row + 1u), scanline));
            memcpy(dst, src, 32u);
        }
    }
    for (scanline = 0; scanline < 8u; ++scanline) {
        u8 *dst = (u8 *)(uintptr_t)(base + row_scanline_offset(bot, scanline));
        memset(dst, 0, 32u);
    }
}

static void scroll_file_down_one(u16 base, u8 top, u8 bot)
{
    u8 row, scanline;

    for (row = bot; row > top; --row) {
        for (scanline = 0; scanline < 8u; ++scanline) {
            u8 *dst = (u8 *)(uintptr_t)(base + row_scanline_offset(row, scanline));
            const u8 *src = (const u8 *)(uintptr_t)(base + row_scanline_offset((u8)(row - 1u), scanline));
            memcpy(dst, src, 32u);
        }
    }
    for (scanline = 0; scanline < 8u; ++scanline) {
        u8 *dst = (u8 *)(uintptr_t)(base + row_scanline_offset(top, scanline));
        memset(dst, 0, 32u);
    }
}

u8 render_scroll_region(screen_t *s, u8 top, u8 bot, s8 n)
{
    u8 row;

    if (top >= bot || bot >= ROWS) {
        return 0;
    }
    if (n == 1) {
        scroll_file_up_one(HIRES_FILE0, top, bot);
        scroll_file_up_one(HIRES_FILE1, top, bot);
    } else if (n == -1) {
        scroll_file_down_one(HIRES_FILE0, top, bot);
        scroll_file_down_one(HIRES_FILE1, top, bot);
    } else {
        return 0;
    }

    for (row = top; row <= bot; ++row) {
        s->dirty[row] = 0;
    }
    return 1;
}

/* Hardware: invert the six pixels of the cursor cell, in place. */
void render_cursor(const screen_t *s)
{
    u8 byte_idx, sh, mask0, mask1;
    u8 i;

    if (!(s->mode & MODE_CURSOR_VISIBLE)) {
        return;
    }

    /* The masks alone describe what to invert; the shift is not needed here. */
    render_cell_span(s->cx, &byte_idx, &sh, &mask0, &mask1);

    for (i = 0; i < 8u; ++i) {
        u8 prow = (u8)(s->cy * 8u + i);
        u8 *p = (u8 *)(uintptr_t)hires_addr(byte_idx, prow);

        *p ^= mask0;
        if (mask1 != 0) {
            u8 *q = (u8 *)(uintptr_t)hires_addr((u8)(byte_idx + 1u), prow);
            *q ^= mask1;
        }
    }
}
