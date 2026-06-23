/*
 * keybuf.c -- outgoing keyboard FIFO.
 */
#include "keybuf.h"

static u8 keybuf[KEYBUF_SIZE];
static volatile u8 keybuf_head;
static volatile u8 keybuf_tail;
static volatile u8 keybuf_used;

static u8 next_index(u8 i)
{
    ++i;
    return (i == KEYBUF_SIZE) ? 0 : i;
}

void keybuf_init(void)
{
    keybuf_head = 0;
    keybuf_tail = 0;
    keybuf_used = 0;
}

u8 keybuf_count(void)
{
    return keybuf_used;
}

u8 keybuf_space(void)
{
    return (u8)(KEYBUF_SIZE - keybuf_used);
}

u8 keybuf_write(const u8 *buf, u8 n)
{
    u8 written = 0;

    if (n > keybuf_space()) {
        return 0;
    }

    while (written < n && keybuf_used < KEYBUF_SIZE) {
        keybuf[keybuf_head] = buf[written++];
        keybuf_head = next_index(keybuf_head);
        ++keybuf_used;
    }
    return written;
}

u8 keybuf_read(u8 *buf, u8 max)
{
    u8 n = 0;

    while (n < max && keybuf_used != 0) {
        buf[n++] = keybuf[keybuf_tail];
        keybuf_tail = next_index(keybuf_tail);
        --keybuf_used;
    }
    return n;
}
