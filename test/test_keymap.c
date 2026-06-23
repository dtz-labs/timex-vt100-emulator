/*
 * test_keymap.c -- host tests for Spectrum matrix -> byte decoding.
 */
#include <assert.h>
#include <stdio.h>
#include "keymap.h"

static int checks = 0;
#define CHECK(cond) do { assert(cond); ++checks; } while (0)

static void press(u8 rows[KEYMAP_ROWS], u8 row, u8 bit)
{
    rows[row] |= (u8)(1u << bit);
}

static u8 decode_new_key(const u8 held[KEYMAP_ROWS], u8 row, u8 bit, u8 out[KEYMAP_OUT_MAX])
{
    u8 rows[KEYMAP_ROWS];
    u8 i;

    for (i = 0; i < KEYMAP_ROWS; ++i) {
        rows[i] = held[i];
    }
    press(rows, row, bit);
    return keymap_decode_rows(rows, held, out, KEYMAP_OUT_MAX);
}

static void test_caps_space_is_plain_space(void)
{
    u8 held[KEYMAP_ROWS] = { 0 };
    u8 out[KEYMAP_OUT_MAX] = { 0xAA, 0xAA, 0xAA };

    keymap_set_cursor_application(0);
    press(held, 0, 0);                         /* CAPS held */
    CHECK(decode_new_key(held, 7, 0, out) == 1);
    CHECK(out[0] == ' ');
}

static void test_enter_sends_terminal_cr(void)
{
    u8 held[KEYMAP_ROWS] = { 0 };
    u8 out[KEYMAP_OUT_MAX] = { 0xAA, 0xAA, 0xAA };

    keymap_set_cursor_application(0);
    CHECK(decode_new_key(held, 6, 0, out) == 1); /* ENTER */
    CHECK(out[0] == KEYMAP_ENTER_CODE);
}

static void test_control_chords_require_both_shifts(void)
{
    u8 held[KEYMAP_ROWS] = { 0 };
    u8 out[KEYMAP_OUT_MAX] = { 0xAA, 0xAA, 0xAA };

    keymap_set_cursor_application(0);
    press(held, 0, 0);                         /* CAPS held */
    press(held, 7, 1);                         /* SYMBOL held */

    CHECK(decode_new_key(held, 0, 3, out) == 1); /* C */
    CHECK(out[0] == 0x03);

    CHECK(decode_new_key(held, 7, 0, out) == 1); /* C-Space */
    CHECK(out[0] == 0x00);

    CHECK(decode_new_key(held, 3, 0, out) == 1); /* deliberate ESC */
    CHECK(out[0] == 0x1B);

    CHECK(decode_new_key(held, 4, 1, out) == 1); /* BS */
    CHECK(out[0] == 0x08);

    CHECK(decode_new_key(held, 4, 0, out) == 1); /* DEL */
    CHECK(out[0] == KEYMAP_DELETE_CODE);
}

static void test_simultaneous_modifier_and_key_is_not_lost(void)
{
    u8 held[KEYMAP_ROWS] = { 0 };
    u8 rows[KEYMAP_ROWS] = { 0 };
    u8 out[KEYMAP_OUT_MAX] = { 0xAA, 0xAA, 0xAA };

    keymap_set_cursor_application(0);
    press(rows, 0, 0);                         /* CAPS and A in one scan */
    press(rows, 1, 0);
    CHECK(keymap_decode_rows(rows, held, out, KEYMAP_OUT_MAX) == 1);
    CHECK(out[0] == 'A');

    rows[1] = 0;
    press(rows, 3, 4);                         /* CAPS and 5 in one scan */
    CHECK(keymap_decode_rows(rows, held, out, KEYMAP_OUT_MAX) == 1);
    CHECK(out[0] == '%');
}

static void test_existing_caps_mappings_still_work(void)
{
    u8 held[KEYMAP_ROWS] = { 0 };
    u8 out[KEYMAP_OUT_MAX] = { 0xAA, 0xAA, 0xAA };

    keymap_set_cursor_application(0);
    press(held, 0, 0);                         /* CAPS held */

    CHECK(decode_new_key(held, 1, 0, out) == 1); /* A */
    CHECK(out[0] == 'A');

    CHECK(decode_new_key(held, 4, 0, out) == 1); /* CAPS+0 */
    CHECK(out[0] == 0x08);

    CHECK(decode_new_key(held, 3, 4, out) == 1); /* CAPS+5 */
    CHECK(out[0] == '%');
}

static void test_caps_digits_match_symbol_digits_except_backspace(void)
{
    u8 held[KEYMAP_ROWS] = { 0 };
    u8 out[KEYMAP_OUT_MAX] = { 0xAA, 0xAA, 0xAA };

    press(held, 0, 0);                         /* CAPS held */

    keymap_set_cursor_application(0);
    CHECK(decode_new_key(held, 3, 0, out) == 1); /* CAPS+1 */
    CHECK(out[0] == '!');
    CHECK(decode_new_key(held, 3, 1, out) == 1); /* CAPS+2 */
    CHECK(out[0] == '@');
    CHECK(decode_new_key(held, 3, 2, out) == 1); /* CAPS+3 */
    CHECK(out[0] == '#');
    CHECK(decode_new_key(held, 3, 3, out) == 1); /* CAPS+4 */
    CHECK(out[0] == '$');
    CHECK(decode_new_key(held, 3, 4, out) == 1); /* CAPS+5 */
    CHECK(out[0] == '%');
    CHECK(decode_new_key(held, 4, 4, out) == 1); /* CAPS+6 */
    CHECK(out[0] == '&');
    CHECK(decode_new_key(held, 4, 3, out) == 1); /* CAPS+7 */
    CHECK(out[0] == '\'');
    CHECK(decode_new_key(held, 4, 2, out) == 1); /* CAPS+8 */
    CHECK(out[0] == '(');
    CHECK(decode_new_key(held, 4, 1, out) == 1); /* CAPS+9 */
    CHECK(out[0] == ')');
    CHECK(decode_new_key(held, 4, 0, out) == 1); /* CAPS+0 */
    CHECK(out[0] == 0x08);
}

static void test_control_digit_cursor_keys(void)
{
    u8 held[KEYMAP_ROWS] = { 0 };
    u8 out[KEYMAP_OUT_MAX] = { 0xAA, 0xAA, 0xAA };

    press(held, 0, 0);                         /* CAPS held */
    press(held, 7, 1);                         /* SYMBOL held */

    keymap_set_cursor_application(0);
    CHECK(decode_new_key(held, 3, 4, out) == 3); /* CAPS+SYMBOL+5 left */
    CHECK(out[0] == 0x1B);
    CHECK(out[1] == '[');
    CHECK(out[2] == 'D');
    CHECK(decode_new_key(held, 4, 4, out) == 3); /* CAPS+SYMBOL+6 down */
    CHECK(out[0] == 0x1B);
    CHECK(out[1] == '[');
    CHECK(out[2] == 'B');
    CHECK(decode_new_key(held, 4, 3, out) == 3); /* CAPS+SYMBOL+7 up */
    CHECK(out[0] == 0x1B);
    CHECK(out[1] == '[');
    CHECK(out[2] == 'A');
    CHECK(decode_new_key(held, 4, 2, out) == 3); /* CAPS+SYMBOL+8 right */
    CHECK(out[0] == 0x1B);
    CHECK(out[1] == '[');
    CHECK(out[2] == 'C');

    keymap_set_cursor_application(1);
    CHECK(decode_new_key(held, 3, 4, out) == 3); /* application left */
    CHECK(out[0] == 0x1B);
    CHECK(out[1] == 'O');
    CHECK(out[2] == 'D');

    keymap_set_cursor_application(0);
}

static void test_symbol_shift_punctuation(void)
{
    u8 held[KEYMAP_ROWS] = { 0 };
    u8 out[KEYMAP_OUT_MAX] = { 0xAA, 0xAA, 0xAA };

    keymap_set_cursor_application(0);
    press(held, 7, 1);                         /* SYMBOL held */

    CHECK(decode_new_key(held, 0, 1, out) == 1); /* Symbol+Z */
    CHECK(out[0] == ':');

    CHECK(decode_new_key(held, 0, 4, out) == 1); /* Symbol+V */
    CHECK(out[0] == '/');

    CHECK(decode_new_key(held, 7, 2, out) == 1); /* Symbol+M */
    CHECK(out[0] == '.');

    CHECK(decode_new_key(held, 3, 0, out) == 1); /* Symbol+1 */
    CHECK(out[0] == '!');

    CHECK(decode_new_key(held, 4, 0, out) == 1); /* Symbol+0 */
    CHECK(out[0] == '_');

    CHECK(decode_new_key(held, 5, 0, out) == 1); /* Symbol+P */
    CHECK(out[0] == '"');

    CHECK(decode_new_key(held, 7, 0, out) == 1); /* Symbol+Space */
    CHECK(out[0] == 0x1B);

    CHECK(decode_new_key(held, 0, 2, out) == 1); /* Symbol+X */
    CHECK(out[0] == '<');

    CHECK(decode_new_key(held, 1, 1, out) == 1); /* Symbol+S */
    CHECK(out[0] == '>');

    CHECK(decode_new_key(held, 1, 2, out) == 1); /* Symbol+D */
    CHECK(out[0] == '\\');

    CHECK(decode_new_key(held, 1, 3, out) == 1); /* Symbol+F */
    CHECK(out[0] == '|');

    CHECK(decode_new_key(held, 1, 0, out) == 1); /* Symbol+A */
    CHECK(out[0] == '`');
}

static void test_key_repeat_for_held_keys(void)
{
    u8 rows[KEYMAP_ROWS] = { 0 };
    u8 out[KEYMAP_OUT_MAX] = { 0xAA, 0xAA, 0xAA };
    u8 i;

    keymap_set_cursor_application(0);
    keymap_init();

    press(rows, 1, 0);                         /* A */
    CHECK(keymap_poll_rows(rows, out, KEYMAP_OUT_MAX) == 1);
    CHECK(out[0] == 'a');

    for (i = 0; i < KEYMAP_REPEAT_DELAY_FRAMES; ++i) {
        CHECK(keymap_poll_rows(rows, out, KEYMAP_OUT_MAX) == 0);
    }
    CHECK(keymap_poll_rows(rows, out, KEYMAP_OUT_MAX) == 1);
    CHECK(out[0] == 'a');

    for (i = 0; i < KEYMAP_REPEAT_RATE_FRAMES; ++i) {
        CHECK(keymap_poll_rows(rows, out, KEYMAP_OUT_MAX) == 0);
    }
    CHECK(keymap_poll_rows(rows, out, KEYMAP_OUT_MAX) == 1);
    CHECK(out[0] == 'a');

    rows[1] = 0;
    CHECK(keymap_poll_rows(rows, out, KEYMAP_OUT_MAX) == 0);
    press(rows, 1, 0);
    CHECK(keymap_poll_rows(rows, out, KEYMAP_OUT_MAX) == 1);
    CHECK(out[0] == 'a');
}

int main(void)
{
    test_caps_space_is_plain_space();
    test_enter_sends_terminal_cr();
    test_control_chords_require_both_shifts();
    test_simultaneous_modifier_and_key_is_not_lost();
    test_existing_caps_mappings_still_work();
    test_caps_digits_match_symbol_digits_except_backspace();
    test_control_digit_cursor_keys();
    test_symbol_shift_punctuation();
    test_key_repeat_for_held_keys();
    printf("keymap: %d checks passed\n", checks);
    return 0;
}
