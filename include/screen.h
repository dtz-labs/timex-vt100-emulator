/*
 * screen.h -- the terminal cell-grid model (pure logic, host-tested).
 *
 * A 64x24 grid of character cells with a cursor, a scroll region, the current
 * SGR attribute, and per-row dirty flags. The VT-100 parser (vtparse) drives
 * this model; the hi-res renderer (render) blits the dirty rows. This module
 * knows nothing about hardware, escape sequences, or pixels -- it is the single
 * source of truth for "what the screen should show". (Design D3.)
 *
 * Per the toolchain rule carried from the game (SDCC z80 struct-return crash),
 * every operation takes a screen_t out-pointer; nothing returns a struct.
 */
#ifndef SCREEN_H
#define SCREEN_H

#include "types.h"

#define COLS 64u
#define ROWS 24u

/* Cell attribute bits (monochrome hi-res: colour is global, so attrs are the
 * few monochrome effects a VT-100 needs). */
#define ATTR_REVERSE   0x01u
#define ATTR_UNDERLINE 0x02u

#define BLANK_CH 0x20u   /* ASCII space: an "empty" cell */

typedef struct {
    u8 ch;     /* character code in the cell                 */
    u8 attr;   /* ATTR_* bits in effect for this cell        */
} cell_t;

typedef struct {
    cell_t cells[ROWS][COLS];
    u8 cx, cy;        /* cursor column (0..COLS-1), row (0..ROWS-1) */
    u8 top, bot;      /* scroll region: rows [top..bot] inclusive    */
    u8 attr;          /* current SGR attribute, copied into cells on putc */
    u8 dirty[ROWS];   /* per-row dirty flag: non-zero => needs repaint    */
} screen_t;

/* Reset to a blank screen: every cell a space with no attributes, cursor home,
 * scroll region the full screen, current attribute cleared, all rows marked
 * dirty (a fresh screen must be painted once). */
void screen_init(screen_t *s);

/* Write one printable character at the cursor using the current attribute,
 * mark the cursor's row dirty, and advance the cursor one column. (Last-column
 * / wrap behaviour is added in a later step.) */
void screen_putc(screen_t *s, u8 ch);

/* Move the cursor to (row, col), 0-based, clamped to the screen. (vtparse
 * converts the 1-based VT-100 CUP/HVP parameters before calling.) */
void screen_cup(screen_t *s, u8 row, u8 col);

/* Scroll the current scroll region [top..bot] by n lines, blanking the freed
 * rows (space, attr 0) and marking the whole region dirty. The cursor is not
 * moved (callers manage it). n > 0 scrolls up (content moves toward the top,
 * blanks appear at the bottom); n < 0 scrolls down. |n| is clamped to the
 * region height. Rows outside the region are untouched. */
void screen_scroll(screen_t *s, s8 n);

#endif /* SCREEN_H */
