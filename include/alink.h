/*
 * alink.h -- Half-Duplex Terminal Protocol v1.
 *
 * Wire format:
 *
 *     +------+------+-----------+--------+
 *     | CTRL | LEN  | PAYLOAD   | CRC-16 |
 *     | 1 B  | 1 B  | 0..64 B   | 2 B    |
 *     +------+------+-----------+--------+
 *
 * CTRL bits:  [TYPE:7-5] [reserved:4-2] [SEQ:1] [ACK:0]
 *
 * CRC-16/CCITT-FALSE (poly 0x1021, init 0xFFFF, no reflection, xorout 0x0000)
 * over CTRL, LEN and PAYLOAD, transmitted high byte first.
 *
 * This header and its two implementation files are pure logic: they are
 * compiled natively by test/run.sh and with z88dk for the target from the
 * same source. The physical layer (src/alink_phy.c) is target-only.
 *
 * Protocol specified in dtz-labs/zx-audio-link; design decisions for this
 * implementation in docs/superpowers/specs/2026-08-02-audio-link-design.md.
 */
#ifndef ALINK_H
#define ALINK_H

#include "types.h"

#define ALINK_MAX_PAYLOAD    64u
#define ALINK_FRAME_MAX      (4u + ALINK_MAX_PAYLOAD)   /* 68 */
#define ALINK_HELLO_PAYLOAD  3u

#define ALINK_TYPE_HELLO     0u
#define ALINK_TYPE_WELCOME   1u
#define ALINK_TYPE_LINK      2u
#define ALINK_TYPE_BYE       3u

#define ALINK_CTRL_ACK        0x01u
#define ALINK_CTRL_SEQ        0x02u
#define ALINK_CTRL_TYPE_SHIFT 5u

#define ALINK_VERSION 1u
#define ALINK_CAPS    0x00u

typedef struct {
    u8 type;
    u8 seq;                          /* 0 or 1 */
    u8 ack;                          /* 0 or 1 */
    u8 len;
    u8 payload[ALINK_MAX_PAYLOAD];
} alink_frame_t;

u16 alink_crc16(const u8 *data, u8 n);

/* Serialise `f` into `out` (must hold ALINK_FRAME_MAX bytes).
 * Returns the number of bytes written, or 0 if f->len exceeds the cap. */
u8 alink_frame_encode(const alink_frame_t *f, u8 *out);

/* Validate and parse one received block. `max_payload` is what THIS receiver
 * advertised, used for validation rule 3's LINK-specific cap.
 * Returns 1 and fills `out` on accept; returns 0 on any rejection, in which
 * case `out` is untouched and the caller must behave as if nothing arrived. */
u8 alink_frame_decode(const u8 *block, u8 block_len, u8 max_payload,
                      alink_frame_t *out);

/* ---- slave session state machine ---- */

#define ALINK_LISTEN 0u
#define ALINK_LINKED 1u

typedef struct {
    u8 state;                        /* ALINK_LISTEN or ALINK_LINKED */
    u8 last_rx_seq;                  /* SEQ of the last accepted downstream
                                      * payload; reset to 1 so that the
                                      * session's first SEQ=0 counts as new */
    u8 tx_seq;                       /* SEQ carried by our upstream payload */
    u8 tx_pending;                   /* 1 while tx_payload is unacknowledged */
    u8 tx_len;
    u8 tx_payload[ALINK_MAX_PAYLOAD];
    u8 max_payload;                  /* what we advertise; v1 = 64 */
    u8 resp_type;                    /* type of the pending response */
    alink_frame_t rx;                /* last successfully decoded LINK frame */
} alink_slave_t;

typedef struct {
    u8 respond;                      /* 1 -> call alink_slave_response() */
    u8 deliver_len;                  /* >0 -> that many bytes at
                                      * alink_slave_rx_payload() go to the
                                      * VT parser */
    u8 session_reset;                /* 1 -> a HELLO reset the session */
    u8 carrier_lost;                 /* 1 -> BYE: show NO CARRIER, LISTEN */
} alink_rx_t;

void alink_slave_init(alink_slave_t *s);

/*
 * Feed one received block. Never partially applies: a block that fails
 * validation leaves every field of *s untouched and clears *out.
 *
 * alink_slave_feed() and alink_slave_response() are two calls rather than one
 * because protocol v1 requires the slave to consume a payload BEFORE replying
 * -- that is the flow-control mechanism. Splitting them lets the caller render
 * the delivered payload and then top up the upstream slot in the same
 * transaction, so a payload the master just acknowledged can be refilled
 * without wasting a round trip. The integration sequence is:
 *
 *     alink_slave_feed()
 *     render alink_slave_rx_payload() for rx.deliver_len bytes
 *     if (alink_slave_tx_ready()) alink_slave_tx_set(...)
 *     alink_slave_response()
 */
void alink_slave_feed(alink_slave_t *s, const u8 *block, u8 block_len,
                      alink_rx_t *out);

const u8 *alink_slave_rx_payload(const alink_slave_t *s);

/* 1 when LINKED with no unacknowledged upstream payload, i.e. when
 * alink_slave_tx_set() would be accepted rather than silently dropped. */
u8 alink_slave_tx_ready(const alink_slave_t *s);

/* Load the next upstream payload. Bytes beyond ALINK_MAX_PAYLOAD are dropped.
 * Has no effect unless alink_slave_tx_ready() is 1 -- protocol v1 slave rule 3
 * requires the outstanding payload to repeat unchanged until acknowledged. */
void alink_slave_tx_set(alink_slave_t *s, const u8 *buf, u8 n);

/* Build the response for the frame most recently fed. Call only when
 * alink_rx_t.respond was 1. */
void alink_slave_response(const alink_slave_t *s, alink_frame_t *resp);

#endif /* ALINK_H */
