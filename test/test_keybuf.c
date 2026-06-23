/*
 * test_keybuf.c -- host tests for the outgoing keyboard FIFO.
 */
#include <assert.h>
#include <stdio.h>
#include "keybuf.h"

static int checks = 0;
#define CHECK(cond) do { assert(cond); ++checks; } while (0)

static void test_empty_state(void)
{
    u8 b = 0xAA;

    keybuf_init();
    CHECK(keybuf_count() == 0);
    CHECK(keybuf_space() == KEYBUF_SIZE);
    CHECK(keybuf_read(&b, 1) == 0);
    CHECK(b == 0xAA);
}

static void test_write_read_order(void)
{
    static const u8 in[] = { 'a', 'b', 'c' };
    u8 out[3];

    keybuf_init();
    CHECK(keybuf_write(in, 3) == 3);
    CHECK(keybuf_count() == 3);
    CHECK(keybuf_read(out, 2) == 2);
    CHECK(out[0] == 'a');
    CHECK(out[1] == 'b');
    CHECK(keybuf_count() == 1);
    CHECK(keybuf_read(out, 2) == 1);
    CHECK(out[0] == 'c');
}

static void test_capacity_clamps_and_wraps(void)
{
    u8 b = 'x';
    u8 out[8];
    u8 i;

    keybuf_init();
    for (i = 0; i < KEYBUF_SIZE; ++i) {
        CHECK(keybuf_write(&b, 1) == 1);
    }
    CHECK(keybuf_space() == 0);
    CHECK(keybuf_write(&b, 1) == 0);
    CHECK(keybuf_read(out, 8) == 8);
    CHECK(keybuf_space() == 8);
    CHECK(keybuf_write((const u8 *)"ABCDEFGH", 8) == 8);
    CHECK(keybuf_count() == KEYBUF_SIZE);
}

static void test_multi_byte_write_is_atomic(void)
{
    u8 b = 'x';
    u8 out[2] = { 0, 0 };
    u8 i;

    keybuf_init();
    for (i = 0; i < KEYBUF_SIZE - 1u; ++i) {
        CHECK(keybuf_write(&b, 1) == 1);
    }
    CHECK(keybuf_space() == 1);
    CHECK(keybuf_write((const u8 *)"\x1b[A", 3) == 0);
    CHECK(keybuf_count() == KEYBUF_SIZE - 1u);
    CHECK(keybuf_read(out, 2) == 2);
    CHECK(out[0] == 'x');
    CHECK(out[1] == 'x');
}

int main(void)
{
    test_empty_state();
    test_write_read_order();
    test_capacity_clamps_and_wraps();
    test_multi_byte_write_is_atomic();
    printf("keybuf: %d checks passed\n", checks);
    return 0;
}
