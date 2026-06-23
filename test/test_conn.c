/*
 * test_conn.c -- host tests for the split RX/TX connection buffer.
 */
#include <assert.h>
#include <stdio.h>
#include "conn.h"

static int checks = 0;
#define CHECK(cond) do { assert(cond); ++checks; } while (0)

static void test_empty_read(void)
{
    u8 b = 0xAA;
    conn_init();
    CHECK(conn_tx_space() == CONN_BUF_SIZE);
    CHECK(conn_tx_count() == 0);
    CHECK(conn_rx_count() == 0);
    CHECK(conn_rx_read(&b, 1) == 0);
    CHECK(b == 0xAA);
}

static void test_loopback_poll_moves_tx_to_rx(void)
{
    u8 out[4];

    conn_init();
    CHECK(conn_tx_write_text("ABC") == 3);
    CHECK(conn_tx_space() == CONN_BUF_SIZE - 3);
    CHECK(conn_rx_read(out, 2) == 0);
    conn_poll();
    CHECK(conn_tx_count() == 0);
    CHECK(conn_rx_read(out, 2) == 2);
    CHECK(out[0] == 'A');
    CHECK(out[1] == 'B');
    CHECK(conn_tx_write_byte('D') == 1);
    conn_poll();
    CHECK(conn_rx_read(out, 4) == 2);
    CHECK(out[0] == 'C');
    CHECK(out[1] == 'D');
}

static void test_capacity_clamps_writes(void)
{
    u8 b = 'x';
    u8 i;
    u8 read_buf[128];

    conn_init();
    for (i = 0; i < CONN_BUF_SIZE; ++i) {
        CHECK(conn_tx_write_byte(b) == 1);
    }
    CHECK(conn_tx_write_byte('y') == 0);
    CHECK(conn_tx_space() == 0);
    conn_poll();
    CHECK(conn_rx_read(read_buf, CONN_BUF_SIZE) == CONN_BUF_SIZE);
    CHECK(conn_rx_read(read_buf, 1) == 0);
}

static void test_compat_wrappers_target_tx(void)
{
    u8 out[2];

    conn_init();
    CHECK(conn_write_text("OK") == 2);
    CHECK(conn_space() == CONN_BUF_SIZE - 2);
    conn_poll();
    CHECK(conn_read(out, 2) == 2);
    CHECK(out[0] == 'O');
    CHECK(out[1] == 'K');
}

int main(void)
{
    test_empty_read();
    test_loopback_poll_moves_tx_to_rx();
    test_capacity_clamps_writes();
    test_compat_wrappers_target_tx();
    printf("conn: %d checks passed\n", checks);
    return 0;
}
