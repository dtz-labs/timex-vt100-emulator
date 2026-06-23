/*
 * keymap.h -- target keyboard polling.
 *
 * Mapping:
 * - printable letters/digits/space
 * - CAPS+letters -> uppercase
 * - CAPS+digits -> same punctuation as SYMBOL+digits, except CAPS+0 -> BS
 * - CAPS+SYMBOL+letter/space -> Control chords
 * - CAPS+SYMBOL+5/6/7/8 -> cursor left/down/up/right
 * - CAPS+SYMBOL+9/0 -> BS/DEL
 * - SYMBOL+key -> ASCII punctuation layer
 * - SYMBOL+SPACE -> ESC prefix for Meta / vi escape
 *
 * CAPS+SPACE intentionally remains a normal space: on fast typing it is too
 * easy to hit by accident, so ESC/BREAK must be a more deliberate chord.
 */
#ifndef KEYMAP_H
#define KEYMAP_H

#include "types.h"

#define KEYMAP_ROWS 8u
#define KEYMAP_OUT_MAX 3u

#ifndef KEYMAP_ENTER_CODE
#define KEYMAP_ENTER_CODE 0x0Du
#endif

#ifndef KEYMAP_DELETE_CODE
#define KEYMAP_DELETE_CODE 0x7Fu
#endif

#ifndef KEYMAP_REPEAT_DELAY_FRAMES
#define KEYMAP_REPEAT_DELAY_FRAMES 18u
#endif

#ifndef KEYMAP_REPEAT_RATE_FRAMES
#define KEYMAP_REPEAT_RATE_FRAMES 4u
#endif

void keymap_init(void);
void keymap_set_cursor_application(u8 on);
u8 keymap_poll(u8 *out, u8 max);

#ifdef KEYMAP_TESTING
u8 keymap_decode_rows(const u8 rows[KEYMAP_ROWS],
                      const u8 prev[KEYMAP_ROWS],
                      u8 *out,
                      u8 max);
u8 keymap_poll_rows(const u8 rows[KEYMAP_ROWS], u8 *out, u8 max);
#endif

#endif /* KEYMAP_H */
