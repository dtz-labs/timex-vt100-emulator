/*
 * conn.c -- split RX/TX byte-stream connection.
 *
 * Default backend: local loopback, used by host tests and ZEsarUX smoke tests.
 * IF1 backend: ZX Interface 1 RS-232 ROM hooks, selected with
 * -DCONN_BACKEND_IF1.
 */
#include "conn.h"

#ifdef CONN_BACKEND_AUDIO
#include "alink.h"
#include "alink_phy.h"
#endif

#ifdef CONN_BACKEND_IF1
#include <z80.h>

#define RS_BAUD_50   0x00u
#define RS_BAUD_75   0x01u
#define RS_BAUD_110  0x02u
#define RS_BAUD_134_5 0x03u
#define RS_BAUD_150  0x04u
#define RS_BAUD_300  0x05u
#define RS_BAUD_600  0x06u
#define RS_BAUD_1200 0x07u
#define RS_BAUD_2400 0x08u
#define RS_BAUD_4800 0x09u
#define RS_BAUD_9600 0x0Au
#define RS_BAUD_19200 0x0Bu
#define RS_BAUD_38400 0x0Cu
#define RS_PAR_NONE  0x00u
#define RS_ERR_OK    0x00u
#define RS_ERR_NO_DATA 0x04u

#define IF1_BAUD_SYSVAR 0x5CC3u
#define IF1_SER_FL      0x5CC7u
#endif

#ifndef CONN_POLL_TX_MAX
#define CONN_POLL_TX_MAX 8u
#endif

#ifndef CONN_POLL_RX_MAX
#define CONN_POLL_RX_MAX 2u
#endif

#ifdef CONN_BACKEND_IF1
#ifndef CONN_IF1_BAUD
#define CONN_IF1_BAUD RS_BAUD_9600
#endif

#if CONN_IF1_BAUD == RS_BAUD_50
#define CONN_IF1_BAUD_DIVISOR 0x0A82u
#elif CONN_IF1_BAUD == RS_BAUD_75
#define CONN_IF1_BAUD_DIVISOR 0x0701u
#elif CONN_IF1_BAUD == RS_BAUD_110
#define CONN_IF1_BAUD_DIVISOR 0x04C5u
#elif CONN_IF1_BAUD == RS_BAUD_134_5
#define CONN_IF1_BAUD_DIVISOR 0x03E6u
#elif CONN_IF1_BAUD == RS_BAUD_150
#define CONN_IF1_BAUD_DIVISOR 0x037Fu
#elif CONN_IF1_BAUD == RS_BAUD_300
#define CONN_IF1_BAUD_DIVISOR 0x01BEu
#elif CONN_IF1_BAUD == RS_BAUD_600
#define CONN_IF1_BAUD_DIVISOR 0x00DEu
#elif CONN_IF1_BAUD == RS_BAUD_1200
#define CONN_IF1_BAUD_DIVISOR 0x006Eu
#elif CONN_IF1_BAUD == RS_BAUD_2400
#define CONN_IF1_BAUD_DIVISOR 0x0036u
#elif CONN_IF1_BAUD == RS_BAUD_4800
#define CONN_IF1_BAUD_DIVISOR 0x001Au
#elif CONN_IF1_BAUD == RS_BAUD_9600
#define CONN_IF1_BAUD_DIVISOR 0x000Cu
#elif CONN_IF1_BAUD == RS_BAUD_19200
#define CONN_IF1_BAUD_DIVISOR 0x0005u
#elif CONN_IF1_BAUD == RS_BAUD_38400
#define CONN_IF1_BAUD_DIVISOR 0x0002u
#else
#error "Unsupported CONN_IF1_BAUD"
#endif
#endif

typedef struct {
    u8 *buf;
    u8 size;
    u8 head;
    u8 tail;
    u8 used;
} conn_ring_t;

static u8 rx_buf[CONN_BUF_SIZE];
static u8 tx_buf[CONN_BUF_SIZE];
static conn_ring_t rx_ring = { rx_buf, CONN_BUF_SIZE, 0, 0, 0 };
static conn_ring_t tx_ring = { tx_buf, CONN_BUF_SIZE, 0, 0, 0 };
static u8 conn_flags;

#ifdef CONN_BACKEND_AUDIO
/*
 * File scope, not locals. A previous version of this project put a 3,840-byte
 * screen_t on the Z80 stack and it overflowed into the IM2 vector table; the
 * rule since then is that anything this size lives in BSS. See include/im2.h.
 */
static alink_slave_t audio_slave;
static alink_frame_t audio_resp;
/*
 * One buffer, three uses, because the link is half duplex and they never
 * overlap: a frame is fully received and consumed before the response is
 * built, and the upstream payload is staged out of the ring only after the
 * received payload has already been copied into rx_ring. On the Timex build
 * this saves 132 bytes of an image that has under 1,800 to spare.
 */
static u8 audio_block[ALINK_FRAME_MAX];
#endif

#if !defined(CONN_BACKEND_IF1) && !defined(CONN_BACKEND_AUDIO)
volatile u8 conn_zrcp_bridge_flags;
volatile u8 conn_zrcp_inject_len;
volatile u8 conn_zrcp_inject_data[CONN_ZRCP_INJECT_MAX];
volatile u8 conn_zrcp_output_len;
volatile u8 conn_zrcp_output_data[CONN_ZRCP_OUTPUT_MAX];
static u8 zrcp_local_echo_suppress_lf;
#endif

#ifdef CONN_BACKEND_IF1
static u8 if1_ready;

static u8 if1_create_sysvars(void) __naked
{
    __asm
        rst     #0x08
        defb    #0x31
        ld      hl,#0x0000
        ret
    __endasm;
}

static u8 if1_rs232_get(u8 *byte) __z88dk_fastcall __naked
{
    (void)byte;
    __asm
        push    hl
        rst     #0x08
        defb    #0x1D
        pop     de
        ld      hl,#0x0004
        ret     nc
        ld      (de),a
        ld      hl,#0x0000
        ret
    __endasm;
}

static u8 if1_rs232_put(u8 byte) __z88dk_fastcall __naked
{
    (void)byte;
    __asm
        ld      a,l
        rst     #0x08
        defb    #0x1E
        ld      hl,#0x0000
        ret
    __endasm;
}
#endif

static u8 next_index(const conn_ring_t *r, u8 i)
{
    ++i;
    return (i == r->size) ? 0 : i;
}

static void ring_reset(conn_ring_t *r)
{
    r->head = 0;
    r->tail = 0;
    r->used = 0;
}

static u8 ring_space(const conn_ring_t *r)
{
    return (u8)(r->size - r->used);
}

static u8 ring_read(conn_ring_t *r, u8 *buf, u8 max)
{
    u8 n = 0;

    while (n < max && r->used != 0) {
        buf[n++] = r->buf[r->tail];
        r->tail = next_index(r, r->tail);
        --r->used;
    }
    return n;
}

static u8 ring_write(conn_ring_t *r, const u8 *buf, u8 n)
{
    u8 written = 0;

    while (written < n && r->used < r->size) {
        r->buf[r->head] = buf[written++];
        r->head = next_index(r, r->head);
        ++r->used;
    }
    return written;
}

static u8 ring_write_byte(conn_ring_t *r, u8 b)
{
    return ring_write(r, &b, 1);
}

static u8 ring_peek(const conn_ring_t *r)
{
    return r->buf[r->tail];
}

static void ring_drop_one(conn_ring_t *r)
{
    if (r->used != 0) {
        r->tail = next_index(r, r->tail);
        --r->used;
    }
}

#if !defined(CONN_BACKEND_IF1) && !defined(CONN_BACKEND_AUDIO)
static void poll_zrcp_inject(void)
{
    u8 i;
    u8 n = conn_zrcp_inject_len;

    if (n == 0) {
        return;
    }
    if (n > CONN_ZRCP_INJECT_MAX) {
        n = CONN_ZRCP_INJECT_MAX;
        conn_flags |= CONN_STATUS_RX_OVERFLOW;
    }
    if (ring_space(&rx_ring) < n) {
        return;
    }
    for (i = 0; i < n; ++i) {
        ring_write_byte(&rx_ring, conn_zrcp_inject_data[i]);
    }
    conn_zrcp_inject_len = 0;
}

static void zrcp_local_echo_write(u8 b)
{
    if (ring_space(&rx_ring) == 0) {
        conn_flags |= CONN_STATUS_RX_OVERFLOW;
        return;
    }
    ring_write_byte(&rx_ring, b);
}

static void zrcp_local_echo_byte(u8 b)
{
    if ((conn_zrcp_bridge_flags & CONN_ZRCP_BRIDGE_LOCAL_ECHO) == 0) {
        return;
    }
    if ((conn_zrcp_bridge_flags & CONN_ZRCP_BRIDGE_LOCAL_ECHO_CRLF) == 0) {
        zrcp_local_echo_write(b);
        return;
    }

    if (zrcp_local_echo_suppress_lf) {
        zrcp_local_echo_suppress_lf = 0;
        if (b == '\n') {
            return;
        }
    }
    if (b == '\r') {
        zrcp_local_echo_write('\r');
        zrcp_local_echo_write('\n');
        zrcp_local_echo_suppress_lf = 1;
        return;
    }
    if (b == '\n') {
        zrcp_local_echo_write('\r');
        zrcp_local_echo_write('\n');
        return;
    }
    zrcp_local_echo_write(b);
}

static void poll_zrcp_output(void)
{
    u8 n = 0;

    if ((conn_zrcp_bridge_flags & CONN_ZRCP_BRIDGE_ENABLE) == 0) {
        return;
    }
    if (conn_zrcp_output_len != 0) {
        if (tx_ring.used != 0) {
            conn_flags |= CONN_STATUS_TX_BLOCKED;
        }
        return;
    }
    while (n < CONN_ZRCP_OUTPUT_MAX && tx_ring.used != 0) {
        u8 b = ring_peek(&tx_ring);
        conn_zrcp_output_data[n] = b;
        zrcp_local_echo_byte(b);
        ring_drop_one(&tx_ring);
        ++n;
    }
    if (n != 0) {
        conn_zrcp_output_len = n;
    }
}
#endif

void conn_init(void)
{
    ring_reset(&rx_ring);
    ring_reset(&tx_ring);
    conn_flags = 0;

#ifdef CONN_BACKEND_AUDIO
    alink_slave_init(&audio_slave);
    alink_phy_init();
#elif defined(CONN_BACKEND_IF1)
    if1_ready = 0;
    if (if1_create_sysvars() != RS_ERR_OK) {
        conn_flags |= CONN_STATUS_INIT_ERROR;
        return;
    }
    *((volatile u16 *)IF1_BAUD_SYSVAR) = CONN_IF1_BAUD_DIVISOR;
    *((volatile u8 *)IF1_SER_FL) = 0;
    if1_ready = 1;
#else
    conn_zrcp_bridge_flags = 0;
    conn_zrcp_inject_len = 0;
    conn_zrcp_output_len = 0;
    zrcp_local_echo_suppress_lf = 0;
#endif
}

void conn_poll(void)
{
#ifdef CONN_BACKEND_AUDIO
    alink_rx_t rx;
    u8 n;

    /*
     * The order here is fixed by the protocol and documented in alink.h:
     * feed, deliver, top up, respond. The slave must consume a payload BEFORE
     * replying -- that is the flow-control mechanism -- and topping the
     * upstream slot up between the two is what lets a payload the master just
     * acknowledged be refilled without wasting a round trip.
     */
    if (!alink_phy_carrier()) {
        return;                     /* line idle; interrupts stayed enabled */
    }
    n = alink_phy_receive(audio_block, (u8)(sizeof audio_block));
    if (n == 0) {
        return;                     /* noise, or a frame that stopped short */
    }

    alink_slave_feed(&audio_slave, audio_block, n, &rx);

    if (rx.deliver_len != 0) {
        if (ring_space(&rx_ring) < rx.deliver_len) {
            conn_flags |= CONN_STATUS_RX_OVERFLOW;
        } else {
            ring_write(&rx_ring, alink_slave_rx_payload(&audio_slave),
                       rx.deliver_len);
        }
    }
    if (rx.carrier_lost) {
        conn_flags |= CONN_STATUS_CARRIER_LOST;
    }

    if (tx_ring.used != 0 && alink_slave_tx_ready(&audio_slave)) {
        n = tx_ring.used;
        if (n > ALINK_MAX_PAYLOAD) {
            n = ALINK_MAX_PAYLOAD;
        }
        n = ring_read(&tx_ring, audio_block, n);
        alink_slave_tx_set(&audio_slave, audio_block, n);
    }

    if (rx.respond) {
        alink_slave_response(&audio_slave, &audio_resp);
        n = alink_frame_encode(&audio_resp, audio_block);
        if (n != 0) {
            alink_phy_send(audio_block, n);
        }
    }
#elif defined(CONN_BACKEND_IF1)
    u8 i;
    u8 b;
    u8 status;

    if (!if1_ready) {
        return;
    }

    for (i = 0; i < CONN_POLL_RX_MAX; ++i) {
        if (ring_space(&rx_ring) == 0) {
            conn_flags |= CONN_STATUS_RX_OVERFLOW;
            break;
        }
        status = if1_rs232_get(&b);
        if (status == RS_ERR_NO_DATA) {
            break;
        }
        if (status != RS_ERR_OK) {
            break;
        }
        ring_write_byte(&rx_ring, b);
    }

    for (i = 0; i < CONN_POLL_TX_MAX && tx_ring.used != 0; ++i) {
        if ((z80_inp(0xEFu) & 0x08u) == 0) {
            conn_flags |= CONN_STATUS_TX_BLOCKED;
            break;
        }
        b = ring_peek(&tx_ring);
        if (if1_rs232_put(b) != RS_ERR_OK) {
            conn_flags |= CONN_STATUS_TX_BLOCKED;
            break;
        }
        ring_drop_one(&tx_ring);
    }
#else
    poll_zrcp_inject();

    if ((conn_zrcp_bridge_flags & CONN_ZRCP_BRIDGE_ENABLE) != 0) {
        poll_zrcp_output();
        return;
    }

    while (tx_ring.used != 0 && ring_space(&rx_ring) != 0) {
        u8 b = ring_peek(&tx_ring);
        ring_drop_one(&tx_ring);
        ring_write_byte(&rx_ring, b);
    }
    if (tx_ring.used != 0 && ring_space(&rx_ring) == 0) {
        conn_flags |= CONN_STATUS_RX_OVERFLOW;
    }
#endif
}

u8 conn_rx_count(void)
{
    return rx_ring.used;
}

u8 conn_rx_read(u8 *buf, u8 max)
{
    return ring_read(&rx_ring, buf, max);
}

u8 conn_tx_count(void)
{
    return tx_ring.used;
}

u8 conn_tx_space(void)
{
    return ring_space(&tx_ring);
}

u8 conn_tx_write(const u8 *buf, u8 n)
{
    return ring_write(&tx_ring, buf, n);
}

u8 conn_tx_write_byte(u8 b)
{
    return ring_write_byte(&tx_ring, b);
}

u8 conn_tx_write_text(const char *p)
{
    u8 written = 0;

    while (*p != '\0') {
        if (!conn_tx_write_byte((u8)(unsigned char)*p)) {
            break;
        }
        ++p;
        ++written;
    }
    return written;
}

u8 conn_status(void)
{
    return conn_flags;
}

u8 conn_take_status(void)
{
    u8 flags = conn_flags;
    conn_flags = 0;
    return flags;
}

u8 conn_space(void)
{
    return conn_tx_space();
}

u8 conn_read(u8 *buf, u8 max)
{
    return conn_rx_read(buf, max);
}

u8 conn_write(const u8 *buf, u8 n)
{
    return conn_tx_write(buf, n);
}

u8 conn_write_byte(u8 b)
{
    return conn_tx_write_byte(b);
}

u8 conn_write_text(const char *p)
{
    return conn_tx_write_text(p);
}
