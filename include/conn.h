/*
 * conn.h -- terminal connection interface.
 *
 * The terminal sees two byte streams:
 * - RX: bytes arriving from the host, consumed by the VT parser.
 * - TX: bytes produced by the keyboard/parser replies, sent to the host.
 *
 * The default backend is a loopback used by tests and the local demo. A real
 * ZX Interface 1 RS-232 backend is selected at build time with
 * CONN_BACKEND_IF1, and an audio backend with CONN_BACKEND_AUDIO. It uses the Interface 1 ROM RS-232 hooks directly so it
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
/* Audio backend: the master sent BYE, or the link went dead. The session is
 * back in LISTEN and will relink on the next HELLO. */
#define CONN_STATUS_CARRIER_LOST 0x10u

#define CONN_ZRCP_INJECT_MAX 32u
#define CONN_ZRCP_OUTPUT_MAX 32u
#define CONN_ZRCP_BRIDGE_ENABLE 0x01u
#define CONN_ZRCP_BRIDGE_LOCAL_ECHO 0x02u
#define CONN_ZRCP_BRIDGE_LOCAL_ECHO_CRLF 0x04u

#if !defined(CONN_BACKEND_IF1) && !defined(CONN_BACKEND_AUDIO)
extern volatile u8 conn_zrcp_bridge_flags;
extern volatile u8 conn_zrcp_inject_len;
extern volatile u8 conn_zrcp_inject_data[CONN_ZRCP_INJECT_MAX];
extern volatile u8 conn_zrcp_output_len;
extern volatile u8 conn_zrcp_output_data[CONN_ZRCP_OUTPUT_MAX];
#endif

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
