/*
 * test_alink_frame.c -- host tests for the protocol v1 frame codec.
 */
#include <assert.h>
#include <stdio.h>
#include <string.h>
#include "alink.h"

static int checks = 0;
#define CHECK(cond) do { assert(cond); ++checks; } while (0)

/* The published CRC-16/CCITT-FALSE check value: "123456789" -> 0x29B1. */
static void test_crc_published_vector(void)
{
    const u8 v[] = { '1','2','3','4','5','6','7','8','9' };
    CHECK(alink_crc16(v, 9) == 0x29B1u);
    CHECK(alink_crc16(v, 0) == 0xFFFFu);   /* init value, nothing consumed */
}

static void test_encode_layout(void)
{
    alink_frame_t f;
    u8 out[ALINK_FRAME_MAX];
    u16 crc;
    u8 n;

    memset(&f, 0, sizeof f);
    f.type = ALINK_TYPE_LINK;
    f.seq = 1;
    f.ack = 1;
    f.len = 2;
    f.payload[0] = 'h';
    f.payload[1] = 'i';

    n = alink_frame_encode(&f, out);
    CHECK(n == 6);
    CHECK(out[0] == (u8)((ALINK_TYPE_LINK << ALINK_CTRL_TYPE_SHIFT)
                         | ALINK_CTRL_SEQ | ALINK_CTRL_ACK));
    CHECK(out[1] == 2);
    CHECK(out[2] == 'h');
    CHECK(out[3] == 'i');
    crc = alink_crc16(out, 4);
    CHECK(out[4] == (u8)(crc >> 8));      /* high byte first */
    CHECK(out[5] == (u8)(crc & 0xFFu));
}

static void test_encode_rejects_oversize(void)
{
    alink_frame_t f;
    u8 out[ALINK_FRAME_MAX];

    memset(&f, 0, sizeof f);
    f.type = ALINK_TYPE_LINK;
    f.len = ALINK_MAX_PAYLOAD + 1u;
    CHECK(alink_frame_encode(&f, out) == 0);
}

static void test_round_trip_all_lengths(void)
{
    alink_frame_t in, out;
    u8 block[ALINK_FRAME_MAX];
    u8 len, i, n;

    for (len = 0; len <= ALINK_MAX_PAYLOAD; ++len) {
        memset(&in, 0, sizeof in);
        memset(&out, 0, sizeof out);
        in.type = ALINK_TYPE_LINK;
        in.seq = (u8)(len & 1u);
        in.ack = (u8)((len >> 1) & 1u);
        in.len = len;
        for (i = 0; i < len; ++i) {
            in.payload[i] = (u8)(i * 7u + 1u);
        }
        n = alink_frame_encode(&in, block);
        CHECK(n == (u8)(len + 4u));
        CHECK(alink_frame_decode(block, n, ALINK_MAX_PAYLOAD, &out) == 1);
        CHECK(out.type == in.type);
        CHECK(out.seq == in.seq);
        CHECK(out.ack == in.ack);
        CHECK(out.len == in.len);
        CHECK(memcmp(out.payload, in.payload, len) == 0);
    }
}

/* Validation rule 1: physical block length must equal LEN + 4. */
static void test_reject_length_mismatch(void)
{
    alink_frame_t f, out;
    u8 block[ALINK_FRAME_MAX];
    u8 n;

    memset(&f, 0, sizeof f);
    f.type = ALINK_TYPE_LINK;
    f.len = 4;
    n = alink_frame_encode(&f, block);
    CHECK(n == 8);
    CHECK(alink_frame_decode(block, n, ALINK_MAX_PAYLOAD, &out) == 1);
    CHECK(alink_frame_decode(block, (u8)(n - 1u), ALINK_MAX_PAYLOAD, &out) == 0);
    CHECK(alink_frame_decode(block, (u8)(n + 1u), ALINK_MAX_PAYLOAD, &out) == 0);
    CHECK(alink_frame_decode(block, 3, ALINK_MAX_PAYLOAD, &out) == 0);
}

/* Validation rule 2: any single flipped bit must be rejected. */
static void test_reject_bit_flips(void)
{
    alink_frame_t f, out;
    u8 block[ALINK_FRAME_MAX];
    u8 good[ALINK_FRAME_MAX];
    u8 n, i, b;

    memset(&f, 0, sizeof f);
    f.type = ALINK_TYPE_LINK;
    f.len = 3;
    f.payload[0] = 'a';
    f.payload[1] = 'b';
    f.payload[2] = 'c';
    n = alink_frame_encode(&f, good);

    for (i = 0; i < n; ++i) {
        for (b = 0; b < 8u; ++b) {
            memcpy(block, good, n);
            block[i] ^= (u8)(1u << b);
            /* Flipping a TYPE bit can produce a reserved type, which is also
             * a rejection -- either way decode must return 0. */
            CHECK(alink_frame_decode(block, n, ALINK_MAX_PAYLOAD, &out) == 0);
        }
    }
}

/* Validation rule 3: absolute cap, then the LINK-only advertised cap. */
static void test_reject_oversize_len(void)
{
    alink_frame_t out;
    u8 block[80];

    memset(block, 0, sizeof block);
    block[0] = (u8)(ALINK_TYPE_LINK << ALINK_CTRL_TYPE_SHIFT);
    block[1] = 65;                       /* over the absolute cap */
    CHECK(alink_frame_decode(block, 69, ALINK_MAX_PAYLOAD, &out) == 0);
}

static void test_reject_over_advertised_payload(void)
{
    alink_frame_t f, out;
    u8 block[ALINK_FRAME_MAX];
    u8 n;

    memset(&f, 0, sizeof f);
    f.type = ALINK_TYPE_LINK;
    f.len = 40;
    n = alink_frame_encode(&f, block);
    CHECK(alink_frame_decode(block, n, ALINK_MAX_PAYLOAD, &out) == 1);
    CHECK(alink_frame_decode(block, n, 32, &out) == 0);   /* over advertised */
    CHECK(alink_frame_decode(block, n, 40, &out) == 1);   /* exactly at it */
}

/* Rule 3's LINK-only wording: a HELLO larger than the advertised payload is
 * still accepted, because the cap applies to LINK frames. */
static void test_advertised_cap_is_link_only(void)
{
    alink_frame_t f, out;
    u8 block[ALINK_FRAME_MAX];
    u8 n;

    memset(&f, 0, sizeof f);
    f.type = ALINK_TYPE_HELLO;
    f.len = 40;
    f.payload[0] = ALINK_VERSION;
    f.payload[2] = ALINK_MAX_PAYLOAD;
    n = alink_frame_encode(&f, block);
    CHECK(alink_frame_decode(block, n, 8, &out) == 1);
}

/* Validation rule 4: HELLO/WELCOME need LEN >= 3; extra bytes are ignored. */
static void test_hello_minimum_length(void)
{
    alink_frame_t f, out;
    u8 block[ALINK_FRAME_MAX];
    u8 n, len;

    for (len = 0; len < ALINK_HELLO_PAYLOAD; ++len) {
        memset(&f, 0, sizeof f);
        f.type = ALINK_TYPE_HELLO;
        f.len = len;
        n = alink_frame_encode(&f, block);
        CHECK(alink_frame_decode(block, n, ALINK_MAX_PAYLOAD, &out) == 0);
        f.type = ALINK_TYPE_WELCOME;
        n = alink_frame_encode(&f, block);
        CHECK(alink_frame_decode(block, n, ALINK_MAX_PAYLOAD, &out) == 0);
    }

    memset(&f, 0, sizeof f);
    f.type = ALINK_TYPE_HELLO;
    f.len = 5;                            /* longer than 3: extras ignored */
    f.payload[0] = ALINK_VERSION;
    f.payload[1] = ALINK_CAPS;
    f.payload[2] = ALINK_MAX_PAYLOAD;
    f.payload[3] = 0xDE;
    f.payload[4] = 0xAD;
    n = alink_frame_encode(&f, block);
    CHECK(alink_frame_decode(block, n, ALINK_MAX_PAYLOAD, &out) == 1);
    CHECK(out.len == 5);
    CHECK(out.payload[0] == ALINK_VERSION);
    CHECK(out.payload[2] == ALINK_MAX_PAYLOAD);
}

/* TYPE 4-7 are reserved: the receiver ignores the frame. */
static void test_reject_reserved_types(void)
{
    alink_frame_t out;
    u8 block[4];
    u16 crc;
    u8 t;

    for (t = 4; t < 8u; ++t) {
        block[0] = (u8)(t << ALINK_CTRL_TYPE_SHIFT);
        block[1] = 0;
        crc = alink_crc16(block, 2);
        block[2] = (u8)(crc >> 8);
        block[3] = (u8)(crc & 0xFFu);
        CHECK(alink_frame_decode(block, 4, ALINK_MAX_PAYLOAD, &out) == 0);
    }
}

/* Reserved CTRL bits 4-2 are sent as 0 and ignored on receive: a frame with
 * them set must still decode, and to the same fields. */
static void test_reserved_ctrl_bits_ignored(void)
{
    alink_frame_t out;
    u8 block[4];
    u16 crc;

    block[0] = (u8)((ALINK_TYPE_LINK << ALINK_CTRL_TYPE_SHIFT)
                    | 0x1Cu | ALINK_CTRL_SEQ);
    block[1] = 0;
    crc = alink_crc16(block, 2);
    block[2] = (u8)(crc >> 8);
    block[3] = (u8)(crc & 0xFFu);
    CHECK(alink_frame_decode(block, 4, ALINK_MAX_PAYLOAD, &out) == 1);
    CHECK(out.type == ALINK_TYPE_LINK);
    CHECK(out.seq == 1);
    CHECK(out.ack == 0);
    CHECK(out.len == 0);
}

int main(void)
{
    test_crc_published_vector();
    test_encode_layout();
    test_encode_rejects_oversize();
    test_round_trip_all_lengths();
    test_reject_length_mismatch();
    test_reject_bit_flips();
    test_reject_oversize_len();
    test_reject_over_advertised_payload();
    test_advertised_cap_is_link_only();
    test_hello_minimum_length();
    test_reject_reserved_types();
    test_reserved_ctrl_bits_ignored();
    printf("alink_frame: %d checks passed\n", checks);
    return 0;
}
