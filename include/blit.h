/*
 * blit.h -- write the rendered glyph rows to the display file (hardware).
 *
 * Exactly one implementation is linked: src/blit_hires.c for the Timex
 * 80-column build (writes both hi-res display files), src/blit_ula.c for the
 * ZX 40-column build. They export the same names, so they are alternatives
 * and are never linked together.
 *
 * Hardware part (ZEsarUX-tested only, may include z80.h).
 */
#ifndef BLIT_H
#define BLIT_H

#include "types.h"
#include "screen.h"

/*
 * Blit all dirty rows from the screen to the display file.
 * For each dirty row, render each cell and write it to video RAM.
 * Clears dirty flags after rendering.
 *
 * Hardware-only: uses direct memory writes.
 */
void blit_flush(screen_t *s);

/*
 * Fast hardware scroll for a full-width text-row region already reflected in
 * the screen model. Handles one text row up/down; returns non-zero if applied.
 *
 * Deliberately does NOT clear the region's dirty marks: the screen model's
 * own scroll (screen_scroll) already migrated each surviving row's marks
 * alongside its cells and fully marked the blanked rows, and some of those
 * marks may cover changes that were never written to video RAM (e.g. a
 * deferred wrap). Clearing them here would discard those pending writes.
 */
u8 blit_scroll_region(screen_t *s, u8 top, u8 bot, s8 n);

/*
 * XOR the cursor cell's six pixels in place. Idempotent in pairs: call once to
 * show the cursor, once more to hide it, and the screen is bit-identical to
 * before. Does NOT mark anything dirty -- that is the point. When
 * MODE_CURSOR_VISIBLE is clear it does nothing, so pairs still balance.
 *
 * Hardware-only.
 */
void blit_cursor_toggle(const screen_t *s);

#endif /* BLIT_H */
