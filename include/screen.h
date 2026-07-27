/*
 * screen.h -- the terminal cell-grid model (pure logic, host-tested).
 *
 * A COLSx24 grid of character cells with a cursor, a scroll region, the current
 * SGR attribute, and per-row bitmaps of dirty four-cell groups. COLS is 80 on
 * the Timex hi-res build and 40 on the plain ZX Spectrum ULA build -- see
 * below. The VT-100 parser (vtparse) drives this model; the renderer (render)
 * blits the dirty rows. This module
 * knows nothing about hardware, escape sequences, or pixels -- it is the single
 * source of truth for "what the screen should show". (Design D3.)
 *
 * Per the toolchain rule carried from the game (SDCC z80 struct-return crash),
 * every operation takes a screen_t out-pointer; nothing returns a struct.
 */
#ifndef SCREEN_H
#define SCREEN_H

#include "types.h"

/*
 * Exactly one machine define selects the geometry. The width is never passed
 * separately, so a mismatched pair -- a ZX build told it has 80 columns --
 * cannot be expressed.
 */
#if defined(TERM_TIMEX) && defined(TERM_ZX)
#error "define exactly one of TERM_TIMEX / TERM_ZX, not both"
#elif defined(TERM_TIMEX)
#define COLS 80u
#elif defined(TERM_ZX)
#define COLS 40u
#else
#error "define exactly one of TERM_TIMEX / TERM_ZX"
#endif

#define ROWS 24u

/*
 * Dirty tracking granularity. Four cells is 24 px is exactly three scanline
 * bytes in BOTH geometries, so it is the packer's natural unit and translates
 * into blits without arithmetic. One boolean per row would repaint COLS cells
 * for a one-character change.
 */
#define DIRTY_GROUP_COLS 4u
#define DIRTY_GROUPS ((COLS + DIRTY_GROUP_COLS - 1u) / DIRTY_GROUP_COLS)
#define DIRTY_BYTES ((DIRTY_GROUPS + 7u) / 8u)

/* Cell attribute bits (monochrome hi-res: colour is global, so attrs are the
 * few monochrome effects a VT-100 needs). */
#define ATTR_REVERSE   0x01u
#define ATTR_UNDERLINE 0x02u

/* Terminal mode bits (screen_t.mode). vtparse maps DEC/ANSI modes here. */
#define MODE_AUTOWRAP           0x01u  /* DECAWM ?7 */
#define MODE_CURSOR_VISIBLE     0x02u  /* DECTCEM ?25 */
#define MODE_CURSOR_APPLICATION 0x04u  /* DECCKM ?1 */
#define MODE_ORIGIN             0x08u  /* DECOM ?6 */
#define MODE_INSERT             0x10u  /* IRM 4 */
#define MODE_NEWLINE            0x20u  /* LNM 20 */

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
    u8 dirty[ROWS][DIRTY_BYTES];  /* per-row bitmap of dirty four-cell groups */
    u8 saved_cx, saved_cy, saved_attr;  /* DECSC/DECRC saved cursor + SGR */
    u8 mode;          /* MODE_* bits                                      */
    u8 wrap_pending;  /* VT-100 deferred wrap: last column written, awaiting */
    u8 scroll_seq;    /* increments when screen_scroll() moves the region    */
    u8 last_scroll_top, last_scroll_bot;
    s8 last_scroll_n;
} screen_t;

/* Mark the four-cell group containing (row, col) dirty. */
void screen_mark_cell(screen_t *s, u8 row, u8 col);

/* Mark every four-cell group touched by the inclusive column span [c0..c1] of
 * row dirty. */
void screen_mark_span(screen_t *s, u8 row, u8 c0, u8 c1);

/* Mark every four-cell group of row dirty. */
void screen_mark_row(screen_t *s, u8 row);

/* Clear all dirty marks for row. */
void screen_clear_marks(screen_t *s, u8 row);

/* Non-zero if the given four-cell group of row is marked dirty. Changing
 * this bit test? See the CONSTRAINT comment at its definition (src/screen.c)
 * -- it has an untested hand-copy in both src/blit_hires.c and
 * src/blit_ula.c. */
u8 screen_group_dirty(const screen_t *s, u8 row, u8 group);

/* Non-zero if any four-cell group of row is marked dirty. */
u8 screen_row_dirty(const screen_t *s, u8 row);

/* Reset to a blank screen: every cell a space with no attributes, cursor home,
 * scroll region the full screen, current attribute cleared, all rows marked
 * dirty (a fresh screen must be painted once). */
void screen_init(screen_t *s);

/* Write one printable character at the cursor using the current attribute,
 * mark the written cell's four-cell group dirty, and advance the cursor one
 * column. In the last
 * column the cursor parks: with MODE_AUTOWRAP the wrap is deferred (VT-100
 * style) and fires on the next printable (CR+LF, scrolling at the region
 * bottom); without it the cell is overwritten in place. Any explicit cursor
 * move (cup/cr/lf/ri) clears the pending wrap. */
void screen_putc(screen_t *s, u8 ch);

/* Move the cursor to (row, col), 0-based, clamped to the screen. (vtparse
 * converts the 1-based VT-100 CUP/HVP parameters before calling.) */
void screen_cup(screen_t *s, u8 row, u8 col);

/* Scroll the current scroll region [top..bot] by n lines, blanking the freed
 * rows (space, attr 0) and marking them fully dirty; surviving rows keep the
 * dirty marks of the content that moved with them. The cursor is not moved
 * (callers manage it). n > 0 scrolls up (content moves toward the top, blanks
 * appear at the bottom); n < 0 scrolls down. |n| is clamped to the region
 * height. Rows outside the region are untouched. */
void screen_scroll(screen_t *s, s8 n);

/* Carriage return: cursor to column 0 of the current row. */
void screen_cr(screen_t *s);

/* Line feed / index (IND): cursor down one row; if already at the bottom of the
 * scroll region, scroll the region up by one instead. Column is unchanged. */
void screen_lf(screen_t *s);

/* Reverse index (RI): cursor up one row; if already at the top of the scroll
 * region, scroll the region down by one instead. Column is unchanged. */
void screen_ri(screen_t *s);

/* Erase in line (EL). mode 0: cursor..end of line; 1: start of line..cursor
 * (inclusive); 2: whole line. Erased cells become space/attr 0; row dirtied. */
void screen_erase_line(screen_t *s, u8 mode);

/* Erase in display (ED). mode 0: cursor..end of screen; 1: start of
 * screen..cursor (inclusive); 2: whole screen. Erased cells -> space/attr 0. */
void screen_erase_display(screen_t *s, u8 mode);

/* Insert n blank lines at the cursor row (IL): lines [cy..bot] scroll down by n,
 * blanks appear at the cursor row, lines pushed past bot are lost. No-op if the
 * cursor is outside the scroll region. Cursor position unchanged. */
void screen_insert_lines(screen_t *s, u8 n);

/* Delete n lines at the cursor row (DL): lines below scroll up into [cy..bot],
 * blanks appear at the bottom of the region. No-op if cursor outside region.
 * Cursor position unchanged. */
void screen_delete_lines(screen_t *s, u8 n);

/* Insert n blank chars at the cursor (ICH): cells [cx..end] shift right by n,
 * blanks at the cursor, cells pushed past the line end are lost. Row only. */
void screen_insert_chars(screen_t *s, u8 n);

/* Delete n chars at the cursor (DCH): cells to the right shift left by n,
 * blanks appear at the line end. Row only. */
void screen_delete_chars(screen_t *s, u8 n);

/* Apply one SGR (Select Graphic Rendition) code to the current attribute
 * (screen_t.attr), which putc copies into each cell. Recognised codes:
 *   0  -> reset all attributes      7  -> reverse on    27 -> reverse off
 *   4  -> underline on             24 -> underline off
 * Any other code (bold 1, colours 30-47, ...) is accepted and ignored.
 * vtparse feeds the parsed `m` parameters here one at a time. */
void screen_set_attr(screen_t *s, u8 sgr);

/* Save cursor position and current SGR attribute (DECSC, ESC 7). A later
 * screen_restore_cursor brings them back. screen_init seeds the saved slot
 * with home (0,0) and attr 0, so a restore before any save homes the cursor
 * (VT-100 power-on default). */
void screen_save_cursor(screen_t *s);

/* Restore the cursor position and SGR attribute saved by screen_save_cursor
 * (DECRC, ESC 8). */
void screen_restore_cursor(screen_t *s);

/* Set (on != 0) or clear (on == 0) the given MODE_* bit(s). vtparse calls this
 * for the SM/RM private modes it tracks. */
void screen_set_mode(screen_t *s, u8 bits, u8 on);

/* Set the scroll region to rows [top..bot] (0-based, inclusive) and home the
 * cursor (DECSTBM, CSI top;bot r). In origin mode, "home" means the top margin;
 * otherwise it is absolute screen home. Ignored if the region is degenerate
 * (top >= bot); bot past the last row is clamped. vtparse converts the 1-based
 * VT-100 params and passes (0, ROWS-1) for the reset-to-full-screen form. */
void screen_set_scroll_region(screen_t *s, u8 top, u8 bot);

#endif /* SCREEN_H */
