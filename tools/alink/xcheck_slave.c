/*
 * xcheck_slave.c -- run the real C slave against the Python master.
 *
 * Protocol on stdin/stdout: one length byte, then that many bytes of block.
 * A response is written back the same way; a block the slave chooses not to
 * answer produces a single 0x00 length byte, so the driver can tell "no
 * response" from "still thinking" without a timeout.
 *
 * The length prefix is NOT part of the wire protocol. Block boundaries come
 * from the physical layer (protocol v1, "Assumptions about the physical
 * layer", point 3); this harness stands in for that layer, which is also what
 * lets a test hand the slave a deliberately wrong block length.
 *
 * A command byte 0xFF followed by a length and payload queues upstream data,
 * standing in for the keyboard buffer.
 *
 * Every downstream payload is echoed back as upstream data, so one assertion
 * on the Python side covers both directions at once.
 */
#include <stdio.h>
#include "alink.h"

#define CMD_QUEUE_TX 0xFFu

static int read_exact(u8 *buf, unsigned n)
{
    unsigned got = 0;
    int c;

    while (got < n) {
        c = getchar();
        if (c == EOF) {
            return 0;
        }
        buf[got++] = (u8)c;
    }
    return 1;
}

int main(void)
{
    static alink_slave_t slave;
    alink_rx_t rx;
    alink_frame_t resp;
    u8 block[256];
    u8 out[ALINK_FRAME_MAX];
    u8 pending_tx[ALINK_MAX_PAYLOAD];
    u8 pending_tx_len = 0;
    int c;
    unsigned n, i, w;

    alink_slave_init(&slave);

    for (;;) {
        c = getchar();
        if (c == EOF) {
            break;
        }

        if ((u8)c == CMD_QUEUE_TX) {
            c = getchar();
            if (c == EOF) {
                break;
            }
            n = (unsigned)c;
            if (n > ALINK_MAX_PAYLOAD) {
                break;
            }
            if (n > 0 && !read_exact(pending_tx, n)) {
                break;
            }
            pending_tx_len = (u8)n;
            continue;
        }

        n = (unsigned)c;
        if (n > 0 && !read_exact(block, n)) {
            break;
        }

        alink_slave_feed(&slave, block, (u8)n, &rx);

        /* Mirror the integration order the header documents: consume the
         * delivered payload, then top up the upstream slot, then answer. */
        if (rx.deliver_len > 0u) {
            if (alink_slave_tx_ready(&slave)) {
                alink_slave_tx_set(&slave, alink_slave_rx_payload(&slave),
                                   rx.deliver_len);
            }
        } else if (pending_tx_len > 0u && alink_slave_tx_ready(&slave)) {
            alink_slave_tx_set(&slave, pending_tx, pending_tx_len);
            pending_tx_len = 0u;
        }

        if (!rx.respond) {
            putchar(0);
            fflush(stdout);
            continue;
        }

        alink_slave_response(&slave, &resp);
        i = alink_frame_encode(&resp, out);
        putchar((int)i);
        for (w = 0; w < i; ++w) {
            putchar((int)out[w]);
        }
        fflush(stdout);
    }

    return 0;
}
