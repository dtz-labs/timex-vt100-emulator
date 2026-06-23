/*
 * conn.h -- terminal connection interface.
 *
 * The terminal sees two byte streams:
 * - RX: bytes arriving from the host, consumed by the VT parser.
 * - TX: bytes produced by the keyboard/parser replies, sent to the host.
 *
 * The default backend is a loopback used by tests and the local demo. A real
 * ZX Interface 1 RS-232 backend is selected at build time with
 * CONN_BACKEND_IF1. It uses the Interface 1 ROM RS-232 hooks directly so it
 * can stay on the same z88dk sdcc_iy build as the default Timex TAP.
 */
#ifndef CONN_H
#define CONN_H

#include "types.h"

#define CONN_BUF_SIZE 128u

#define CONN_STATUS_RX_OVERFLOW 0x01u
#define CONN_STATUS_TX_BLOCKED  0x02u
#define CONN_STATUS_IF1_MISSING 0x04u
#define CONN_STATUS_INIT_ERROR  0x08u

void conn_init(void);
void conn_poll(void);

u8 conn_rx_count(void);
u8 conn_rx_read(u8 *buf, u8 max);

u8 conn_tx_count(void);
u8 conn_tx_space(void);
u8 conn_tx_write(const u8 *buf, u8 n);
u8 conn_tx_write_byte(u8 b);
u8 conn_tx_write_text(const char *p);

u8 conn_status(void);
u8 conn_take_status(void);

/* Compatibility wrappers for the old loopback-only API. */
u8 conn_space(void);
u8 conn_read(u8 *buf, u8 max);
u8 conn_write(const u8 *buf, u8 n);
u8 conn_write_byte(u8 b);
u8 conn_write_text(const char *p);

#endif /* CONN_H */
