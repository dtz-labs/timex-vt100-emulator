/*
 * keybuf.h -- outgoing keyboard FIFO.
 *
 * This buffers bytes produced by keymap before they are handed to conn. It is
 * pure logic so it can be host-tested; target-only overrun feedback lives in
 * main.c.
 */
#ifndef KEYBUF_H
#define KEYBUF_H

#include "types.h"

#define KEYBUF_SIZE 128u

void keybuf_init(void);
u8 keybuf_count(void);
u8 keybuf_space(void);
u8 keybuf_write(const u8 *buf, u8 n);
u8 keybuf_read(u8 *buf, u8 max);

#endif /* KEYBUF_H */
