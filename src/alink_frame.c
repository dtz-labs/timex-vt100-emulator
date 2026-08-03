/*
 * alink_frame.c -- protocol v1 frame codec.
 *
 * Pure logic: no hardware, no absolute addresses, no machine define. Compiled
 * natively by test/run.sh and with z88dk for the target from this same source.
 *
 * The CRC is computed bitwise rather than from a table: 256 entries would cost
 * 512 bytes of a Timex image that has under 1,800 to spare (see the memory
 * budget in docs/superpowers/specs/2026-08-02-audio-link-design.md), and at
 * ~68 bytes per frame against a link that spends 0.4 s per frame on the wire,
 * the table would buy nothing measurable.
 */
#include "alink.h"

u16 alink_crc16(const u8 *data, u8 n)
{
    u16 crc = 0xFFFFu;
    u8 i, b;

    for (i = 0; i < n; ++i) {
        crc ^= (u16)((u16)data[i] << 8);
        for (b = 0; b < 8u; ++b) {
            if (crc & 0x8000u) {
                crc = (u16)((u16)(crc << 1) ^ 0x1021u);
            } else {
                crc = (u16)(crc << 1);
            }
        }
    }
    return crc;
}

u8 alink_frame_encode(const alink_frame_t *f, u8 *out)
{
    u16 crc;
    u8 i;

    if (f->len > ALINK_MAX_PAYLOAD) {
        return 0;
    }

    out[0] = (u8)(((u8)(f->type & 0x07u) << ALINK_CTRL_TYPE_SHIFT)
                  | (u8)(f->seq ? ALINK_CTRL_SEQ : 0u)
                  | (u8)(f->ack ? ALINK_CTRL_ACK : 0u));
    out[1] = f->len;
    for (i = 0; i < f->len; ++i) {
        out[2u + i] = f->payload[i];
    }

    crc = alink_crc16(out, (u8)(2u + f->len));
    out[2u + f->len] = (u8)(crc >> 8);
    out[3u + f->len] = (u8)(crc & 0xFFu);
    return (u8)(4u + f->len);
}

u8 alink_frame_decode(const u8 *block, u8 block_len, u8 max_payload,
                      alink_frame_t *out)
{
    u16 crc, want;
    u8 len, type, i;

    if (block_len < 4u) {
        return 0;
    }

    len = block[1];
    if (len > ALINK_MAX_PAYLOAD) {          /* rule 3, absolute cap */
        return 0;
    }
    if (block_len != (u8)(len + 4u)) {      /* rule 1 */
        return 0;
    }

    crc = alink_crc16(block, (u8)(2u + len));                       /* rule 2 */
    want = (u16)(((u16)block[2u + len] << 8) | block[3u + len]);
    if (crc != want) {
        return 0;
    }

    type = (u8)((block[0] >> ALINK_CTRL_TYPE_SHIFT) & 0x07u);
    if (type > ALINK_TYPE_BYE) {            /* 4-7 reserved: ignore the frame */
        return 0;
    }
    if (type == ALINK_TYPE_LINK && len > max_payload) {   /* rule 3, LINK only */
        return 0;
    }
    if ((type == ALINK_TYPE_HELLO || type == ALINK_TYPE_WELCOME)
            && len < ALINK_HELLO_PAYLOAD) {                          /* rule 4 */
        return 0;
    }

    out->type = type;
    out->seq = (u8)((block[0] & ALINK_CTRL_SEQ) ? 1u : 0u);
    out->ack = (u8)((block[0] & ALINK_CTRL_ACK) ? 1u : 0u);
    out->len = len;
    for (i = 0; i < len; ++i) {
        out->payload[i] = block[2u + i];
    }
    return 1;
}
