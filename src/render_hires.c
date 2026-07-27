/*
 * render_hires.c -- the Timex 80-column geometry: where a text cell lands in
 * a hi-res scanline, and how a full scanline packs into the two 32-byte
 * display-file rows that hold it.
 *
 * See render_geom.h for the interface both geometries implement.
 */
#include "render_geom.h"
#include "render.h"

/*
 * Screen geometry. 80 cells of 6 px are centred in the 512-px hi-res line:
 * a 16-px (2-byte) margin each side, cells occupying scanline bytes 2..61.
 * The margin is chosen byte-aligned so column 0 starts on a byte boundary.
 */
#define RENDER_LEFT_MARGIN_PX 16u
#define RENDER_CELL_PX        6u

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
 * PURE: pack one full scanline into the two 32-byte display-file rows that
 * hold it.
 *
 * Cells are walked in groups of eight because eight cells span six bytes,
 * which is three CONSECUTIVE indices in each display file -- so both files
 * are written sequentially with no parity test in the loop.
 *
 * g80: COLS attribute-applied glyph bytes for one scanline (cell in bits
 *      7..2), left to right.
 * ev, od: the 32-byte destination rows for the even (file0) and odd (file1)
 *         display files; margin indices 0 and 31 are left untouched.
 */
void render_row_bytes(const u8 *g80, u8 *ev, u8 *od)
{
    u8 j;

    for (j = 0; j < 10u; ++j) {
        u8 base = (u8)(j * 8u);   /* first cell of this group          */
        u8 fi = (u8)(1u + j * 3u); /* first file index of this group    */
        u8 packed[3];

        /* Scanline bytes 2+6j .. 4+6j. */
        render_pack4(&g80[base], packed);
        ev[fi] = packed[0];
        od[fi] = packed[1];
        ev[fi + 1u] = packed[2];

        /* Scanline bytes 5+6j .. 7+6j. */
        render_pack4(&g80[base + 4u], packed);
        od[fi + 1u] = packed[0];
        ev[fi + 2u] = packed[1];
        od[fi + 2u] = packed[2];
    }
}

/*
 * PURE: destination bytes for four-cell dirty group `g`.
 *
 * Eight cells span six scanline bytes -- three consecutive indices in each
 * display file, exactly as render_row_bytes lays them out above. Group `g` is
 * half of one such eight-cell block: an even group takes the block's first
 * three destination writes (ev[fi], od[fi], ev[fi+1]); an odd group takes the
 * remaining three (od[fi+1], ev[fi+2], od[fi+2]), where fi = 1 + 3*(g/2).
 *
 * file3[k] == 0 selects the even file (ev), 1 selects the odd file (od) --
 * indices/files returned rather than pointers, so this needs no display
 * memory and is host-testable.
 */
void render_group_bytes(u8 g, u8 *idx3, u8 *file3)
{
    u8 fi = (u8)(1u + 3u * (g >> 1));

    if ((g & 1u) == 0u) {
        idx3[0] = fi;       file3[0] = 0u;   /* ev[fi]      */
        idx3[1] = fi;       file3[1] = 1u;   /* od[fi]      */
        idx3[2] = (u8)(fi + 1u); file3[2] = 0u; /* ev[fi+1] */
    } else {
        idx3[0] = (u8)(fi + 1u); file3[0] = 1u; /* od[fi+1] */
        idx3[1] = (u8)(fi + 2u); file3[1] = 0u; /* ev[fi+2] */
        idx3[2] = (u8)(fi + 2u); file3[2] = 1u; /* od[fi+2] */
    }
}
