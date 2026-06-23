/*
 * main.c -- M3 terminal emulator main loop.
 *
 * Initializes hi-res video, screen grid, and VT parser; then feeds a demo
 * VT-100 stream through the parser and renders to the display.
 *
 * Keyboard sampling runs from the frame interrupt; main drains the captured
 * bytes to conn TX and does the slower terminal/render work.
 *
 * Demo stream: box with line-drawing, SGR test, then a scroll-region test.
 * After the baked stream, the default conn backend loops keyboard bytes back.
 * A real Interface 1 RS-232 backend can be selected at build time.
 */
#include "video.h"
#include "screen.h"
#include "vtparse.h"
#include "render.h"
#include "conn.h"
#include "keymap.h"
#include "keybuf.h"
#include <z80.h>
#include <intrinsic.h>

static const u8 demo_stream[] =
    "\x1b[2J"
    "\x1b[H"
    "\x1b(0"
    "lqqqqqqqqqqqqqqqqqqqqqqqqqk\r\n"
    "x VT-100 TERMINAL         x\r\n"
    "x   \x1b[7mREVERSE\x1b[0m \x1b[4mUNDER\x1b[0m         x\r\n"
    "mqqqqqqqqqqqqqqqqqqqqqqqqqj"
    "\x1b(B"
    "\x1b[6;1H"
    "Scrolling region below:"
    "\x1b[7;24r"
    "\x1b[7;1H"
    "scroll 01\r\n"
    "scroll 02\r\n"
    "scroll 03\r\n"
    "scroll 04\r\n"
    "scroll 05\r\n"
    "scroll 06\r\n"
    "scroll 07\r\n"
    "scroll 08\r\n"
    "scroll 09\r\n"
    "scroll 10\r\n"
    "scroll 11\r\n"
    "scroll 12\r\n"
    "scroll 13\r\n"
    "scroll 14\r\n"
    "scroll 15\r\n"
    "scroll 16\r\n"
    "scroll 17\r\n"
    "scroll 18\r\n"
    "scroll 19\r\n"
    "scroll 20\r\n"
    "scroll 21\r\n"
    "scroll 22\r\n"
    "scroll 23\r\n"
    "scroll 24\r\n"
    "scroll 25\r\n"
    "scroll 26\r\n"
    "scroll 27\r\n"
    "scroll 28\r\n"
    "scroll 29\r\n"
    "scroll 30\r\n"
    "scroll 31\r\n"
    "scroll 32\r\n"
    "scroll 33\r\n"
    "scroll 34\r\n"
    "scroll 35\r\n"
    "scroll 36\r\n"
    "scroll 37\r\n"
    "scroll 38\r\n"
    "scroll 39\r\n"
    "scroll 40";

static volatile u8 key_overrun;
static volatile u8 keyboard_settle_frames;
static u8 irq_keybuf_tmp[KEYMAP_OUT_MAX];
static u8 irq_nkeys;

#define IO_BATCH_SIZE 32u

static void pump_vt_replies(vtparse_t *vt)
{
    u8 n;
    u8 i;

    if (vt->nout == 0) {
        return;
    }

    n = conn_tx_write(vt->out, vt->nout);
    if (n == 0) {
        return;
    }

    vt->nout = (u8)(vt->nout - n);
    for (i = 0; i < vt->nout; ++i) {
        vt->out[i] = vt->out[i + n];
    }
}

static u8 pump_conn(vtparse_t *vt, screen_t *scr)
{
    static u8 buf[IO_BATCH_SIZE];
    u8 n;
    u8 i;
    u8 changed = 0;
    u8 old_scroll_seq;

    do {
        n = conn_rx_read(buf, (u8)(sizeof buf));
        if (n != 0) {
            scr->dirty[scr->cy] = 1;  /* erase the previously rendered cursor */
        }
        for (i = 0; i < n; ++i) {
            if (buf[i] == '\n' || (scr->wrap_pending && buf[i] >= 0x20u)) {
                render_flush(scr);
            }
            old_scroll_seq = scr->scroll_seq;
            vt_feed(vt, scr, buf[i]);
            if (scr->scroll_seq != old_scroll_seq) {
                if (render_scroll_region(scr, scr->last_scroll_top,
                                         scr->last_scroll_bot,
                                         scr->last_scroll_n)) {
                    changed = 1;
                }
            }
            pump_vt_replies(vt);
        }
        if (n != 0) {
            scr->dirty[scr->cy] = 1;  /* draw the cursor at its new position */
            changed = 1;
        }
    } while (n != 0);

    return changed;
}

static void beep_overrun(void)
{
    u8 i, j;
    for (i = 0; i < 80u; ++i) {
        z80_outp(0xFEu, 0x10u);
        for (j = 0; j < 24u; ++j) {
            (void)z80_inp(0xFEu);
        }
        z80_outp(0xFEu, 0x00u);
        for (j = 0; j < 24u; ++j) {
            (void)z80_inp(0xFEu);
        }
    }
}

void keyboard_frame_tick(void)
{
    if (keyboard_settle_frames != 0) {
        keymap_init();
        --keyboard_settle_frames;
        return;
    }

    irq_nkeys = keymap_poll(irq_keybuf_tmp, KEYMAP_OUT_MAX);
    if (irq_nkeys != 0 && keybuf_write(irq_keybuf_tmp, irq_nkeys) != irq_nkeys) {
        key_overrun = 1;
    }
}

void keyboard_im2_isr(void) __naked
{
    __asm
        push    af
        push    bc
        push    de
        push    hl
        ex      af,af
        exx
        push    af
        push    bc
        push    de
        push    hl
        push    ix
        push    iy
        call    _keyboard_frame_tick
        pop     iy
        pop     ix
        pop     hl
        pop     de
        pop     bc
        pop     af
        exx
        ex      af,af
        pop     hl
        pop     de
        pop     bc
        pop     af
        ei
        reti
    __endasm;
}

static void install_keyboard_im2(void) __naked
{
    __asm
        di
        ld      hl,#0xD300
        ld      de,#0xD301
        ld      bc,#257
        ld      a,#0xD4
        ld      (hl),a
        ldir
        ld      hl,#0xD4D4
        ld      (hl),#0xC3
        inc     hl
        ld      de,#_keyboard_im2_isr
        ld      (hl),e
        inc     hl
        ld      (hl),d
        ld      a,#0xD3
        ld      i,a
        im      2
        ei
        ret
    __endasm;
}

static void pump_keybuf_to_conn(void)
{
    static u8 buf[IO_BATCH_SIZE];
    u8 want;
    u8 n;

    while (keybuf_count() != 0 && conn_tx_space() != 0) {
        want = conn_tx_space();
        if (want > (u8)(sizeof buf)) {
            want = (u8)(sizeof buf);
        }
        intrinsic_di();
        n = keybuf_read(buf, want);
        intrinsic_ei();
        if (n == 0) {
            return;
        }
        conn_tx_write(buf, n);
    }
}

static void init_keyboard_interrupts(void)
{
    intrinsic_di();
    key_overrun = 0;
    keyboard_settle_frames = 50;
    keymap_init();
    install_keyboard_im2();
}

static void sync_keyboard_modes(const screen_t *scr)
{
    keymap_set_cursor_application((scr->mode & MODE_CURSOR_APPLICATION) != 0);
}

int main(void)
{
    screen_t scr;
    vtparse_t vt;
    u16 i = 0;
    u8 conn_flags;

    /* Init hardware: hi-res white-on-black, clear screen. */
    video_hires_on(1);  /* white-on-black = 1 */
    video_clear();

    /* Init software: screen grid and VT parser. */
    screen_init(&scr);
    vt_init(&vt);
    conn_init();
    keybuf_init();
    sync_keyboard_modes(&scr);

    /* Feed demo stream through parser. */
    while (demo_stream[i] != 0) {
        vt_feed(&vt, &scr, demo_stream[i]);
        ++i;
    }

    /* Render everything to display. */
    render_flush(&scr);
    render_cursor(&scr);
#ifndef KEYBOARD_NO_IM2
    init_keyboard_interrupts();
#endif

    for (;;) {
        conn_flags = conn_take_status();
        if (conn_flags & CONN_STATUS_RX_OVERFLOW) {
            beep_overrun();
        }
        if (key_overrun) {
            key_overrun = 0;
            beep_overrun();
        }
        pump_keybuf_to_conn();
        conn_poll();
        if (pump_conn(&vt, &scr)) {
            sync_keyboard_modes(&scr);
            render_flush(&scr);
            render_cursor(&scr);
        }
        pump_vt_replies(&vt);
        conn_poll();
    }

    return 0;
}
