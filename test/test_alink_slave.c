/*
 * test_alink_slave.c -- host tests for the protocol v1 slave state machine.
 *
 * Every test drives the slave the way the wire does: by encoding a master
 * frame, handing the bytes to alink_slave_feed(), and inspecting what comes
 * back. Nothing reaches into the struct to set up a state the wire could not
 * produce.
 */
#include <assert.h>
#include <stdio.h>
#include <string.h>
#include "alink.h"

static int checks = 0;
#define CHECK(cond) do { assert(cond); ++checks; } while (0)

/* Encode a master frame and feed it to the slave. */
static void feed(alink_slave_t *s, u8 type, u8 seq, u8 ack,
                 const char *payload, alink_rx_t *out)
{
    alink_frame_t f;
    u8 block[ALINK_FRAME_MAX];
    u8 n;

    memset(&f, 0, sizeof f);
    f.type = type;
    f.seq = seq;
    f.ack = ack;
    f.len = (u8)(payload ? (u8)strlen(payload) : 0u);
    if (payload) {
        memcpy(f.payload, payload, f.len);
    }
    n = alink_frame_encode(&f, block);
    alink_slave_feed(s, block, n, out);
}

static void feed_hello(alink_slave_t *s, alink_rx_t *out)
{
    alink_frame_t f;
    u8 block[ALINK_FRAME_MAX];
    u8 n;

    memset(&f, 0, sizeof f);
    f.type = ALINK_TYPE_HELLO;
    f.len = ALINK_HELLO_PAYLOAD;
    f.payload[0] = ALINK_VERSION;
    f.payload[1] = ALINK_CAPS;
    f.payload[2] = ALINK_MAX_PAYLOAD;
    n = alink_frame_encode(&f, block);
    alink_slave_feed(s, block, n, out);
}

static void test_starts_in_listen_and_ignores_link(void)
{
    alink_slave_t s;
    alink_rx_t r;

    alink_slave_init(&s);
    CHECK(s.state == ALINK_LISTEN);
    CHECK(alink_slave_tx_ready(&s) == 0);      /* not LINKED */

    feed(&s, ALINK_TYPE_LINK, 0, 0, "hello", &r);
    CHECK(r.respond == 0);
    CHECK(r.deliver_len == 0);
    CHECK(s.state == ALINK_LISTEN);
}

static void test_hello_gets_welcome_and_links(void)
{
    alink_slave_t s;
    alink_rx_t r;
    alink_frame_t resp;

    alink_slave_init(&s);
    feed_hello(&s, &r);
    CHECK(r.respond == 1);
    CHECK(r.session_reset == 1);
    CHECK(r.deliver_len == 0);                 /* negotiation never delivers */
    CHECK(s.state == ALINK_LINKED);

    alink_slave_response(&s, &resp);
    CHECK(resp.type == ALINK_TYPE_WELCOME);
    CHECK(resp.seq == 0);
    CHECK(resp.ack == 0);
    CHECK(resp.len == ALINK_HELLO_PAYLOAD);
    CHECK(resp.payload[0] == ALINK_VERSION);
    CHECK(resp.payload[1] == ALINK_CAPS);
    CHECK(resp.payload[2] == ALINK_MAX_PAYLOAD);
}

static void test_first_link_payload_is_delivered(void)
{
    alink_slave_t s;
    alink_rx_t r;
    alink_frame_t resp;

    alink_slave_init(&s);
    feed_hello(&s, &r);

    feed(&s, ALINK_TYPE_LINK, 0, 0, "abc", &r);
    CHECK(r.respond == 1);
    CHECK(r.deliver_len == 3);
    CHECK(memcmp(alink_slave_rx_payload(&s), "abc", 3) == 0);

    alink_slave_response(&s, &resp);
    CHECK(resp.type == ALINK_TYPE_LINK);
    CHECK(resp.ack == 0);                      /* echoes last accepted SEQ */
    CHECK(resp.len == 0);                      /* nothing queued upstream */
}

static void test_duplicate_is_answered_but_not_redelivered(void)
{
    alink_slave_t s;
    alink_rx_t r;

    alink_slave_init(&s);
    feed_hello(&s, &r);
    feed(&s, ALINK_TYPE_LINK, 0, 0, "abc", &r);
    CHECK(r.deliver_len == 3);

    feed(&s, ALINK_TYPE_LINK, 0, 0, "abc", &r);
    CHECK(r.respond == 1);                     /* still answered */
    CHECK(r.deliver_len == 0);                 /* but not delivered twice */

    feed(&s, ALINK_TYPE_LINK, 1, 0, "de", &r); /* new SEQ -> new payload */
    CHECK(r.deliver_len == 2);
}

/* Receivers ignore the SEQ bit of LEN=0 frames: an empty poll must never
 * advance the accepted-SEQ state, or the next real payload would be dropped. */
static void test_empty_poll_does_not_touch_seq_state(void)
{
    alink_slave_t s;
    alink_rx_t r;
    alink_frame_t resp;

    alink_slave_init(&s);
    feed_hello(&s, &r);

    feed(&s, ALINK_TYPE_LINK, 1, 0, NULL, &r);   /* empty poll, SEQ=1 */
    CHECK(r.respond == 1);
    CHECK(r.deliver_len == 0);
    alink_slave_response(&s, &resp);
    CHECK(resp.ack == 1);        /* still the reset value, nothing accepted */

    feed(&s, ALINK_TYPE_LINK, 0, 0, "x", &r);    /* the session's first payload */
    CHECK(r.deliver_len == 1);
}

static void test_upstream_repeats_until_acked(void)
{
    alink_slave_t s;
    alink_rx_t r;
    alink_frame_t resp;

    alink_slave_init(&s);
    feed_hello(&s, &r);

    CHECK(alink_slave_tx_ready(&s) == 1);
    alink_slave_tx_set(&s, (const u8 *)"KEY", 3);
    CHECK(alink_slave_tx_ready(&s) == 0);

    /* The master's ACK starts at 1 -- its "last accepted" reset value -- so a
     * poll before it has accepted anything does NOT acknowledge our SEQ=0
     * payload. A master sending ACK=0 here would be claiming to have accepted
     * a payload it has not yet seen. */
    feed(&s, ALINK_TYPE_LINK, 0, 1, NULL, &r);
    alink_slave_response(&s, &resp);
    CHECK(resp.len == 3);
    CHECK(resp.seq == 0);
    CHECK(memcmp(resp.payload, "KEY", 3) == 0);

    /* Still unacknowledged: repeat the identical payload. */
    feed(&s, ALINK_TYPE_LINK, 0, 1, NULL, &r);
    alink_slave_response(&s, &resp);
    CHECK(resp.len == 3);
    CHECK(resp.seq == 0);
    CHECK(memcmp(resp.payload, "KEY", 3) == 0);
    CHECK(alink_slave_tx_ready(&s) == 0);

    /* Now the master echoes our SEQ back: slot frees and SEQ advances. */
    feed(&s, ALINK_TYPE_LINK, 0, 0, NULL, &r);
    CHECK(alink_slave_tx_ready(&s) == 1);
    alink_slave_response(&s, &resp);
    CHECK(resp.len == 0);
    CHECK(resp.seq == 1);
}

/*
 * The session's reset values are asymmetric: our SEQ starts at 0 while "last
 * accepted" starts at 1. A master that has accepted nothing therefore sends
 * ACK=1, and ACK=0 genuinely means "I accepted your SEQ=0 payload".
 *
 * Reading that backwards -- treating the first poll's ACK as an
 * acknowledgement -- releases an upstream payload that was never delivered,
 * losing the keystrokes in it silently. This test pins the distinction.
 */
static void test_ack_reset_value_does_not_release_payload(void)
{
    alink_slave_t s;
    alink_rx_t r;
    alink_frame_t resp;

    alink_slave_init(&s);
    feed_hello(&s, &r);
    alink_slave_tx_set(&s, (const u8 *)"K", 1);

    feed(&s, ALINK_TYPE_LINK, 0, 1, NULL, &r);   /* ACK at its reset value */
    CHECK(alink_slave_tx_ready(&s) == 0);        /* payload still in flight */
    alink_slave_response(&s, &resp);
    CHECK(resp.len == 1);
    CHECK(resp.seq == 0);

    feed(&s, ALINK_TYPE_LINK, 0, 0, NULL, &r);   /* a genuine acknowledgement */
    CHECK(alink_slave_tx_ready(&s) == 1);
    alink_slave_response(&s, &resp);
    CHECK(resp.seq == 1);                        /* advanced exactly once */
}

/* Rule 3: the outstanding payload must repeat unchanged. A tx_set() while a
 * payload is still in flight must not corrupt it. */
static void test_tx_set_ignored_while_pending(void)
{
    alink_slave_t s;
    alink_rx_t r;
    alink_frame_t resp;

    alink_slave_init(&s);
    feed_hello(&s, &r);
    alink_slave_tx_set(&s, (const u8 *)"AAA", 3);
    alink_slave_tx_set(&s, (const u8 *)"BBBBB", 5);   /* must be ignored */

    feed(&s, ALINK_TYPE_LINK, 0, 1, NULL, &r);   /* ACK=1: not an ack of ours */
    alink_slave_response(&s, &resp);
    CHECK(resp.len == 3);
    CHECK(memcmp(resp.payload, "AAA", 3) == 0);
}

static void test_tx_set_clamps_to_max_payload(void)
{
    alink_slave_t s;
    alink_rx_t r;
    alink_frame_t resp;
    u8 big[ALINK_MAX_PAYLOAD + 8u];
    u8 i;

    for (i = 0; i < sizeof big; ++i) {
        big[i] = (u8)(i + 1u);
    }
    alink_slave_init(&s);
    feed_hello(&s, &r);
    alink_slave_tx_set(&s, big, (u8)ALINK_MAX_PAYLOAD);

    feed(&s, ALINK_TYPE_LINK, 0, 1, NULL, &r);   /* ACK=1: not an ack of ours */
    alink_slave_response(&s, &resp);
    CHECK(resp.len == ALINK_MAX_PAYLOAD);
    CHECK(memcmp(resp.payload, big, ALINK_MAX_PAYLOAD) == 0);
}

/* Rule 4: a valid HELLO in any state resets the session and drops pending
 * state in both directions. */
static void test_hello_resets_mid_session(void)
{
    alink_slave_t s;
    alink_rx_t r;
    alink_frame_t resp;

    alink_slave_init(&s);
    feed_hello(&s, &r);
    feed(&s, ALINK_TYPE_LINK, 0, 0, "abc", &r);
    alink_slave_tx_set(&s, (const u8 *)"KEY", 3);
    CHECK(alink_slave_tx_ready(&s) == 0);

    feed_hello(&s, &r);
    CHECK(r.session_reset == 1);
    CHECK(r.respond == 1);
    CHECK(s.state == ALINK_LINKED);
    CHECK(alink_slave_tx_ready(&s) == 1);        /* in-flight payload dropped */
    alink_slave_response(&s, &resp);
    CHECK(resp.type == ALINK_TYPE_WELCOME);

    /* Sequence state was reset, so SEQ=0 counts as new again. */
    feed(&s, ALINK_TYPE_LINK, 0, 0, "abc", &r);
    CHECK(r.deliver_len == 3);
}

static void test_bye_drops_to_listen_without_replying(void)
{
    alink_slave_t s;
    alink_rx_t r;

    alink_slave_init(&s);
    feed_hello(&s, &r);
    feed(&s, ALINK_TYPE_BYE, 0, 0, NULL, &r);
    CHECK(r.carrier_lost == 1);
    CHECK(r.respond == 0);                       /* no reply required */
    CHECK(s.state == ALINK_LISTEN);

    feed(&s, ALINK_TYPE_LINK, 0, 0, "abc", &r);  /* LISTEN ignores LINK */
    CHECK(r.respond == 0);
    CHECK(r.deliver_len == 0);
}

/* A slave never acts on a WELCOME -- that is a slave-to-master frame. */
static void test_welcome_from_master_is_ignored(void)
{
    alink_slave_t s;
    alink_rx_t r;
    alink_frame_t f;
    u8 block[ALINK_FRAME_MAX];
    u8 n;

    alink_slave_init(&s);
    feed_hello(&s, &r);

    memset(&f, 0, sizeof f);
    f.type = ALINK_TYPE_WELCOME;
    f.len = ALINK_HELLO_PAYLOAD;
    f.payload[0] = ALINK_VERSION;
    f.payload[2] = ALINK_MAX_PAYLOAD;
    n = alink_frame_encode(&f, block);
    alink_slave_feed(&s, block, n, &r);
    CHECK(r.respond == 0);
    CHECK(r.deliver_len == 0);
    CHECK(s.state == ALINK_LINKED);
}

/* A corrupted block must leave the slave exactly as it was. */
static void test_invalid_block_changes_nothing(void)
{
    alink_slave_t s, before;
    alink_rx_t r;
    alink_frame_t f;
    u8 block[ALINK_FRAME_MAX];
    u8 n;

    alink_slave_init(&s);
    feed_hello(&s, &r);
    feed(&s, ALINK_TYPE_LINK, 0, 0, "abc", &r);

    memset(&f, 0, sizeof f);
    f.type = ALINK_TYPE_LINK;
    f.seq = 1;
    f.len = 2;
    f.payload[0] = 'z';
    f.payload[1] = 'z';
    n = alink_frame_encode(&f, block);
    block[3] ^= 0x40u;                           /* corrupt the payload */

    memcpy(&before, &s, sizeof s);
    alink_slave_feed(&s, block, n, &r);
    CHECK(r.respond == 0);
    CHECK(r.deliver_len == 0);
    CHECK(memcmp(&before, &s, sizeof s) == 0);
}

int main(void)
{
    test_starts_in_listen_and_ignores_link();
    test_hello_gets_welcome_and_links();
    test_first_link_payload_is_delivered();
    test_duplicate_is_answered_but_not_redelivered();
    test_empty_poll_does_not_touch_seq_state();
    test_upstream_repeats_until_acked();
    test_ack_reset_value_does_not_release_payload();
    test_tx_set_ignored_while_pending();
    test_tx_set_clamps_to_max_payload();
    test_hello_resets_mid_session();
    test_bye_drops_to_listen_without_replying();
    test_welcome_from_master_is_ignored();
    test_invalid_block_changes_nothing();
    printf("alink_slave: %d checks passed\n", checks);
    return 0;
}
