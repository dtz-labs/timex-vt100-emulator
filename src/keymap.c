/*
 * keymap.c -- Spectrum/Timex keyboard polling.
 */
#include "keymap.h"
#ifndef KEYMAP_HOST_TEST
#include <z80.h>
#endif

#ifndef KEYMAP_HOST_TEST
static const u16 row_ports[KEYMAP_ROWS] = {
    0xFEFEu, 0xFDFEu, 0xFBFEu, 0xF7FEu,
    0xEFFEu, 0xDFFEu, 0xBFFEu, 0x7FFEu
};
#endif

static u8 cursor_application_mode;
static u8 prev_rows[KEYMAP_ROWS];
static u8 repeat_row = 0xFFu;
static u8 repeat_bit;
static u8 repeat_caps;
static u8 repeat_sym;
static u8 repeat_wait;

static const char key_chars[KEYMAP_ROWS][5] = {
    { 0,   'z', 'x', 'c', 'v' },
    { 'a', 's', 'd', 'f', 'g' },
    { 'q', 'w', 'e', 'r', 't' },
    { '1', '2', '3', '4', '5' },
    { '0', '9', '8', '7', '6' },
    { 'p', 'o', 'i', 'u', 'y' },
    { '\r','l', 'k', 'j', 'h' },
    { ' ', 0,   'm', 'n', 'b' }
};

#ifndef KEYMAP_HOST_TEST
static u8 read_row(u8 row)
{
    return (u8)(~z80_inp(row_ports[row]) & 0x1Fu);
}

static void read_rows(u8 rows[KEYMAP_ROWS])
{
    u8 i;
    for (i = 0; i < KEYMAP_ROWS; ++i) {
        rows[i] = read_row(i);
    }
}

static void save_rows(const u8 rows[KEYMAP_ROWS])
{
    u8 i;
    for (i = 0; i < KEYMAP_ROWS; ++i) {
        prev_rows[i] = rows[i];
    }
}

#else
static void save_rows(const u8 rows[KEYMAP_ROWS])
{
    u8 i;
    for (i = 0; i < KEYMAP_ROWS; ++i) {
        prev_rows[i] = rows[i];
    }
}
#endif

static void clear_rows(u8 rows[KEYMAP_ROWS])
{
    u8 i;
    for (i = 0; i < KEYMAP_ROWS; ++i) {
        rows[i] = 0;
    }
}

static u8 emit_byte(u8 *out, u8 max, u8 b)
{
    if (max < 1u) {
        return 0;
    }
    out[0] = b;
    return 1;
}

static u8 emit_cursor(u8 *out, u8 max, u8 final)
{
    if (max < 3u) {
        return 0;
    }
    out[0] = 0x1B;
    out[1] = cursor_application_mode ? 'O' : '[';
    out[2] = final;
    return 3;
}

static u8 emit_control(u8 *out, u8 max, char ch)
{
    if (ch >= 'a' && ch <= 'z') {
        return emit_byte(out, max, (u8)(ch - 'a' + 1));
    }
    switch (ch) {
    case ' ':  return emit_byte(out, max, 0x00); /* C-Space / C-@ */
    case '\r': return emit_byte(out, max, 0x0D); /* C-M */
    case '0':  return emit_byte(out, max, KEYMAP_DELETE_CODE);
    case '1':  return emit_byte(out, max, 0x1B); /* deliberate ESC */
    case '5':  return emit_cursor(out, max, 'D');
    case '6':  return emit_cursor(out, max, 'B');
    case '7':  return emit_cursor(out, max, 'A');
    case '8':  return emit_cursor(out, max, 'C');
    case '9':  return emit_byte(out, max, 0x08); /* BS */
    default:   return 0;
    }
}

static u8 map_symbol(u8 *out, u8 max, char ch)
{
    switch (ch) {
    case ' ': return emit_byte(out, max, 0x1B); /* ESC prefix for Meta */
    case '1': return emit_byte(out, max, '!');
    case '2': return emit_byte(out, max, '@');
    case '3': return emit_byte(out, max, '#');
    case '4': return emit_byte(out, max, '$');
    case '5': return emit_byte(out, max, '%');
    case '6': return emit_byte(out, max, '&');
    case '7': return emit_byte(out, max, '\'');
    case '8': return emit_byte(out, max, '(');
    case '9': return emit_byte(out, max, ')');
    case '0': return emit_byte(out, max, '_');
    case 'a': return emit_byte(out, max, '`');
    case 's': return emit_byte(out, max, '>');
    case 'd': return emit_byte(out, max, '\\');
    case 'f': return emit_byte(out, max, '|');
    case 'z': return emit_byte(out, max, ':');
    case 'x': return emit_byte(out, max, '<');
    case 'c': return emit_byte(out, max, '?');
    case 'v': return emit_byte(out, max, '/');
    case 'b': return emit_byte(out, max, '*');
    case 'n': return emit_byte(out, max, ',');
    case 'm': return emit_byte(out, max, '.');
    case 'o': return emit_byte(out, max, ';');
    case 'p': return emit_byte(out, max, '"');
    case 'h': return emit_byte(out, max, '^');
    case 'j': return emit_byte(out, max, '-');
    case 'k': return emit_byte(out, max, '+');
    case 'l': return emit_byte(out, max, '=');
    case 'q': return emit_byte(out, max, '[');
    case 'w': return emit_byte(out, max, ']');
    case 'e': return emit_byte(out, max, '{');
    case 'r': return emit_byte(out, max, '}');
    case 't': return emit_byte(out, max, '~');
    default:  return 0;
    }
}

static u8 map_key(u8 *out, u8 max, u8 row, u8 bit, u8 caps_down, u8 sym_down)
{
    char ch = key_chars[row][bit];
    u8 ctrl_down = caps_down && sym_down;

    if (row == 0u && bit == 0u) {  /* CAPS SHIFT alone */
        return 0;
    }
    if (row == 7u && bit == 1u) {  /* SYMBOL SHIFT alone */
        return 0;
    }
    if (ctrl_down) {
        return emit_control(out, max, ch);
    }
    if (sym_down) {
        return map_symbol(out, max, ch);
    }
    if (caps_down && (row == 3u || row == 4u)) {
        return map_symbol(out, max, ch);
    }
    if (ch == '\r') {
        return emit_byte(out, max, KEYMAP_ENTER_CODE);
    }
    if (ch >= 'a' && ch <= 'z' && caps_down) {
        ch = (char)(ch - ('a' - 'A'));
    }
    return emit_byte(out, max, (u8)ch);
}

static u8 decode_new_mapped_key(const u8 rows[KEYMAP_ROWS],
                                const u8 prev[KEYMAP_ROWS],
                                u8 *out,
                                u8 max,
                                u8 *out_row,
                                u8 *out_bit)
{
    u8 caps_down = (rows[0] & 0x01u) != 0;
    u8 sym_down = (rows[7] & 0x02u) != 0;
    u8 row, bit;

    for (row = 0; row < KEYMAP_ROWS; ++row) {
        u8 newly = (u8)(rows[row] & (u8)~prev[row]);
        for (bit = 0; bit < 5u; ++bit) {
            if (newly & (u8)(1u << bit)) {
                u8 n = map_key(out, max, row, bit, caps_down, sym_down);
                if (n != 0) {
                    *out_row = row;
                    *out_bit = bit;
                    return n;
                }
            }
        }
    }
    return 0;
}

static void repeat_start(u8 row, u8 bit, u8 caps_down, u8 sym_down)
{
    repeat_row = row;
    repeat_bit = bit;
    repeat_caps = caps_down;
    repeat_sym = sym_down;
    repeat_wait = KEYMAP_REPEAT_DELAY_FRAMES;
}

static void repeat_stop(void)
{
    repeat_row = 0xFFu;
    repeat_wait = 0;
}

u8 keymap_decode_rows(const u8 rows[KEYMAP_ROWS],
                      const u8 prev[KEYMAP_ROWS],
                      u8 *out,
                      u8 max)
{
    u8 row, bit;

    return decode_new_mapped_key(rows, prev, out, max, &row, &bit);
}

u8 keymap_poll_rows(const u8 rows[KEYMAP_ROWS], u8 *out, u8 max)
{
    u8 caps_down = (rows[0] & 0x01u) != 0;
    u8 sym_down = (rows[7] & 0x02u) != 0;
    u8 row, bit;
    u8 n;

    n = decode_new_mapped_key(rows, prev_rows, out, max, &row, &bit);
    if (n != 0) {
        save_rows(rows);
        repeat_start(row, bit, caps_down, sym_down);
        return n;
    }

    if (repeat_row != 0xFFu &&
        (rows[repeat_row] & (u8)(1u << repeat_bit)) != 0 &&
        caps_down == repeat_caps &&
        sym_down == repeat_sym) {
        if (repeat_wait != 0) {
            --repeat_wait;
            save_rows(rows);
            return 0;
        }
        n = map_key(out, max, repeat_row, repeat_bit, caps_down, sym_down);
        repeat_wait = KEYMAP_REPEAT_RATE_FRAMES;
        save_rows(rows);
        return n;
    }

    repeat_stop();
    save_rows(rows);
    return 0;
}

void keymap_set_cursor_application(u8 on)
{
    cursor_application_mode = (on != 0);
}

void keymap_init(void)
{
#ifndef KEYMAP_HOST_TEST
    read_rows(prev_rows);
#else
    clear_rows(prev_rows);
#endif
    repeat_stop();
}

#ifndef KEYMAP_HOST_TEST
u8 keymap_poll(u8 *out, u8 max)
{
    u8 rows[KEYMAP_ROWS];

    read_rows(rows);
    return keymap_poll_rows(rows, out, max);
}
#endif
