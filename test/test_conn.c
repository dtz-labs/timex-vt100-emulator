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

#ifndef CONN_BACKEND_IF1
static void test_zrcp_inject_mailbox_targets_rx(void)
{
    u8 out[4];

    conn_init();
    conn_zrcp_inject_data[0] = 'Z';
    conn_zrcp_inject_data[1] = 'R';
    conn_zrcp_inject_data[2] = 'C';
    conn_zrcp_inject_data[3] = 'P';
    conn_zrcp_inject_len = 4;

    conn_poll();

    CHECK(conn_zrcp_inject_len == 0);
    CHECK(conn_rx_read(out, 4) == 4);
    CHECK(out[0] == 'Z');
    CHECK(out[1] == 'R');
    CHECK(out[2] == 'C');
    CHECK(out[3] == 'P');
}

static void test_zrcp_bridge_mailbox_targets_output(void)
{
    conn_init();
    conn_zrcp_bridge_flags = CONN_ZRCP_BRIDGE_ENABLE;

    CHECK(conn_tx_write_text("OUT") == 3);
    conn_poll();

    CHECK(conn_rx_count() == 0);
    CHECK(conn_tx_count() == 0);
    CHECK(conn_zrcp_output_len == 3);
    CHECK(conn_zrcp_output_data[0] == 'O');
    CHECK(conn_zrcp_output_data[1] == 'U');
    CHECK(conn_zrcp_output_data[2] == 'T');

    CHECK(conn_tx_write_text("X") == 1);
    conn_poll();
    CHECK(conn_tx_count() == 1);
    CHECK((conn_take_status() & CONN_STATUS_TX_BLOCKED) != 0);

    conn_zrcp_output_len = 0;
    conn_poll();
    CHECK(conn_tx_count() == 0);
    CHECK(conn_zrcp_output_len == 1);
    CHECK(conn_zrcp_output_data[0] == 'X');
}

static void test_zrcp_bridge_local_echo_raw(void)
{
    u8 out[2];

    conn_init();
    conn_zrcp_bridge_flags = CONN_ZRCP_BRIDGE_ENABLE | CONN_ZRCP_BRIDGE_LOCAL_ECHO;

    CHECK(conn_tx_write_text("E!") == 2);
    conn_poll();

    CHECK(conn_zrcp_output_len == 2);
    CHECK(conn_zrcp_output_data[0] == 'E');
    CHECK(conn_zrcp_output_data[1] == '!');
    CHECK(conn_rx_read(out, 2) == 2);
    CHECK(out[0] == 'E');
    CHECK(out[1] == '!');
}

static void test_zrcp_bridge_local_echo_crlf(void)
{
    static const u8 msg[4] = { 'A', '\r', '\n', 'B' };
    u8 out[4];

    conn_init();
    conn_zrcp_bridge_flags = CONN_ZRCP_BRIDGE_ENABLE |
                             CONN_ZRCP_BRIDGE_LOCAL_ECHO |
                             CONN_ZRCP_BRIDGE_LOCAL_ECHO_CRLF;

    CHECK(conn_tx_write(msg, sizeof msg) == sizeof msg);
    conn_poll();

    CHECK(conn_zrcp_output_len == 4);
    CHECK(conn_zrcp_output_data[0] == 'A');
    CHECK(conn_zrcp_output_data[1] == '\r');
    CHECK(conn_zrcp_output_data[2] == '\n');
    CHECK(conn_zrcp_output_data[3] == 'B');
    CHECK(conn_rx_read(out, 4) == 4);
    CHECK(out[0] == 'A');
    CHECK(out[1] == '\r');
    CHECK(out[2] == '\n');
    CHECK(out[3] == 'B');

    conn_zrcp_output_len = 0;
    CHECK(conn_tx_write_byte('\r') == 1);
    conn_poll();
    CHECK(conn_zrcp_output_len == 1);
    CHECK(conn_zrcp_output_data[0] == '\r');
    CHECK(conn_rx_read(out, 2) == 2);
    CHECK(out[0] == '\r');
    CHECK(out[1] == '\n');
}
#endif

int main(void)
{
    test_empty_read();
    test_loopback_poll_moves_tx_to_rx();
    test_capacity_clamps_writes();
    test_compat_wrappers_target_tx();
#ifndef CONN_BACKEND_IF1
    test_zrcp_inject_mailbox_targets_rx();
    test_zrcp_bridge_mailbox_targets_output();
    test_zrcp_bridge_local_echo_raw();
    test_zrcp_bridge_local_echo_crlf();
#endif
    printf("conn: %d checks passed\n", checks);
    return 0;
}
