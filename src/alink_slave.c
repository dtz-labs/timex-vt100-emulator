/*
 * alink_slave.c -- protocol v1 slave session state machine.
 *
 * Pure logic, compiled both natively and for the target. The rule numbers in
 * the comments refer to the "Slave rules" list in the protocol v1 spec
 * (dtz-labs/zx-audio-link).
 *
 * alink_slave_feed() never partially applies a block: validation happens
 * first, into a local frame, and a rejected block leaves the whole struct
 * untouched. That is what lets the caller treat "invalid" and "never arrived"
 * as the same thing, which the spec requires.
 */
#include "alink.h"

#define ALINK_RESP_NONE    0u
#define ALINK_RESP_WELCOME 1u
#define ALINK_RESP_LINK    2u

/* Clear everything a session owns. Leaves max_payload alone: it is
 * configuration, not session state. */
static void session_reset(alink_slave_t *s)
{
    s->last_rx_seq = 1u;    /* so the session's first SEQ=0 counts as new */
    s->tx_seq = 0u;
    s->tx_pending = 0u;
    s->tx_len = 0u;
    s->resp_type = ALINK_RESP_NONE;
}

void alink_slave_init(alink_slave_t *s)
{
    u8 i;

    s->state = ALINK_LISTEN;
    s->max_payload = ALINK_MAX_PAYLOAD;
    for (i = 0; i < ALINK_MAX_PAYLOAD; ++i) {
        s->tx_payload[i] = 0u;
    }
    s->rx.type = 0u;
    s->rx.seq = 0u;
    s->rx.ack = 0u;
    s->rx.len = 0u;
    for (i = 0; i < ALINK_MAX_PAYLOAD; ++i) {
        s->rx.payload[i] = 0u;
    }
    session_reset(s);
}

void alink_slave_feed(alink_slave_t *s, const u8 *block, u8 block_len,
                      alink_rx_t *out)
{
    alink_frame_t f;
    u8 i;

    out->respond = 0u;
    out->deliver_len = 0u;
    out->session_reset = 0u;
    out->carrier_lost = 0u;

    /* Decode into a local first, so a rejected block cannot disturb s->rx. */
    if (!alink_frame_decode(block, block_len, s->max_payload, &f)) {
        return;
    }

    /* Rule 4: a valid HELLO in any state resets the session. Its payload is
     * consumed by negotiation and must never enter the byte stream. */
    if (f.type == ALINK_TYPE_HELLO) {
        session_reset(s);
        s->state = ALINK_LINKED;
        s->resp_type = ALINK_RESP_WELCOME;
        out->session_reset = 1u;
        out->respond = 1u;
        return;
    }

    /* Rule 1: while in LISTEN, silently ignore everything except HELLO. */
    if (s->state != ALINK_LINKED) {
        return;
    }

    if (f.type == ALINK_TYPE_BYE) {
        session_reset(s);
        s->state = ALINK_LISTEN;
        out->carrier_lost = 1u;
        return;                      /* no reply required */
    }

    /* WELCOME is a slave-to-master frame; a slave never acts on one. */
    if (f.type != ALINK_TYPE_LINK) {
        return;
    }

    s->rx.type = f.type;
    s->rx.seq = f.seq;
    s->rx.ack = f.ack;
    s->rx.len = f.len;
    for (i = 0; i < f.len; ++i) {
        s->rx.payload[i] = f.payload[i];
    }

    /* Rule 3: our outstanding payload is released when the master echoes its
     * SEQ back as ACK. Only then does our SEQ advance. */
    if (s->tx_pending && s->rx.ack == s->tx_seq) {
        s->tx_pending = 0u;
        s->tx_len = 0u;
        s->tx_seq = (u8)(s->tx_seq ^ 1u);
    }

    /* Rule 2: accept a downstream payload only from a LEN>0 frame whose SEQ
     * differs from the last accepted one. The LEN>0 test is also what
     * implements "receivers ignore the SEQ bit of LEN=0 frames" -- an empty
     * poll must never advance last_rx_seq, or the next real payload carrying
     * that same SEQ would be discarded as a duplicate. */
    if (s->rx.len > 0u && s->rx.seq != s->last_rx_seq) {
        s->last_rx_seq = s->rx.seq;
        out->deliver_len = s->rx.len;
    }

    /* Rule 1: while LINKED, respond to every valid master frame. */
    s->resp_type = ALINK_RESP_LINK;
    out->respond = 1u;
}

const u8 *alink_slave_rx_payload(const alink_slave_t *s)
{
    return s->rx.payload;
}

u8 alink_slave_tx_ready(const alink_slave_t *s)
{
    return (u8)((s->state == ALINK_LINKED && s->tx_pending == 0u) ? 1u : 0u);
}

void alink_slave_tx_set(alink_slave_t *s, const u8 *buf, u8 n)
{
    u8 i;

    if (!alink_slave_tx_ready(s)) {
        return;               /* rule 3: the outstanding payload must repeat */
    }
    if (n > ALINK_MAX_PAYLOAD) {
        n = ALINK_MAX_PAYLOAD;
    }
    for (i = 0; i < n; ++i) {
        s->tx_payload[i] = buf[i];
    }
    s->tx_len = n;
    s->tx_pending = (u8)((n > 0u) ? 1u : 0u);
}

void alink_slave_response(const alink_slave_t *s, alink_frame_t *resp)
{
    u8 i;

    if (s->resp_type == ALINK_RESP_WELCOME) {
        resp->type = ALINK_TYPE_WELCOME;
        resp->seq = 0u;                        /* negotiation is SEQ=0, ACK=0 */
        resp->ack = 0u;
        resp->len = ALINK_HELLO_PAYLOAD;
        resp->payload[0] = ALINK_VERSION;
        resp->payload[1] = ALINK_CAPS;
        resp->payload[2] = s->max_payload;
        return;
    }

    resp->type = ALINK_TYPE_LINK;
    resp->seq = s->tx_seq;
    resp->ack = s->last_rx_seq;                /* rule 1: always echo it */
    resp->len = (u8)(s->tx_pending ? s->tx_len : 0u);
    for (i = 0; i < resp->len; ++i) {
        resp->payload[i] = s->tx_payload[i];
    }
}
