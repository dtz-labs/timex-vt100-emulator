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

/* PURE: produce 8 pixel rows for one cell. */
void render_cell_bytes(u8 ch, u8 attr, u8 out[8])
{
    const u8 *glyph = font_glyph(ch);
    u8 i;

    /* Copy glyph bytes. */
    for (i = 0; i < 8; ++i) {
        out[i] = glyph[i];
    }

    /* Apply REVERSE: invert all rows. */
    if (attr & ATTR_REVERSE) {
        for (i = 0; i < 8; ++i) {
            out[i] = ~out[i];
        }
    }

    /* Apply UNDERLINE: set bottom row to full ink. */
    if (attr & ATTR_UNDERLINE) {
        out[7] = 0xFF;
    }
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

/* Hardware: blit dirty rows to display. */
static void render_cell_to(u8 *dst[8], u8 bx, u8 ch, u8 attr)
{
    const u8 *glyph = font_glyph(ch);

    if (attr == 0) {
        dst[0][bx] = glyph[0];
        dst[1][bx] = glyph[1];
        dst[2][bx] = glyph[2];
        dst[3][bx] = glyph[3];
        dst[4][bx] = glyph[4];
        dst[5][bx] = glyph[5];
        dst[6][bx] = glyph[6];
        dst[7][bx] = glyph[7];
    } else {
        u8 b0 = glyph[0];
        u8 b1 = glyph[1];
        u8 b2 = glyph[2];
        u8 b3 = glyph[3];
        u8 b4 = glyph[4];
        u8 b5 = glyph[5];
        u8 b6 = glyph[6];
        u8 b7 = glyph[7];

        if (attr & ATTR_REVERSE) {
            b0 = ~b0;
            b1 = ~b1;
            b2 = ~b2;
            b3 = ~b3;
            b4 = ~b4;
            b5 = ~b5;
            b6 = ~b6;
            b7 = ~b7;
        }
        if (attr & ATTR_UNDERLINE) {
            b7 = 0xFFu;
        }

        dst[0][bx] = b0;
        dst[1][bx] = b1;
        dst[2][bx] = b2;
        dst[3][bx] = b3;
        dst[4][bx] = b4;
        dst[5][bx] = b5;
        dst[6][bx] = b6;
        dst[7][bx] = b7;
    }
}

static void render_row_fast(const screen_t *s, u8 r)
{
    u8 *even_dst[8];
    u8 *odd_dst[8];
    cell_t *cell = (cell_t *)&s->cells[r][0];
    u8 bx;
    u8 i;

    for (i = 0; i < 8u; ++i) {
        u8 prow = (u8)((r << 3) + i);
        u16 off = ((u16)(prow & 0xC0u) << 5)
                | ((u16)(prow & 0x07u) << 8)
                | ((u16)(prow & 0x38u) << 2);
        even_dst[i] = (u8 *)(uintptr_t)(HIRES_FILE0 + off);
        odd_dst[i] = (u8 *)(uintptr_t)(HIRES_FILE1 + off);
    }

    for (bx = 0; bx < 32u; ++bx) {
        render_cell_to(even_dst, bx, cell->ch, cell->attr);
        ++cell;
        render_cell_to(odd_dst, bx, cell->ch, cell->attr);
        ++cell;
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

/* Hardware: render cursor at current position. */
void render_cursor(const screen_t *s)
{
    u8 cx = s->cx;
    u8 cy = s->cy;
    u8 i;
    u8 ch = s->cells[cy][cx].ch;
    u8 attr = s->cells[cy][cx].attr;
    u8 cell_bytes[8];

    /* Only draw if cursor is visible. */
    if (!(s->mode & MODE_CURSOR_VISIBLE)) {
        return;
    }

    /* Get current cell bytes. */
    render_cell_bytes(ch, attr, cell_bytes);

    /* Invert the cell to show cursor. */
    for (i = 0; i < 8; ++i) {
        u16 addr = hires_addr(cx, (u8)(cy * 8 + i));
        *(u8 *)(uintptr_t)addr = ~cell_bytes[i];
    }
}
