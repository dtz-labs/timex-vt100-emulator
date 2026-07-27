/*
 * main.c -- M3 terminal emulator main loop.
 *
 * Initializes hi-res video, screen grid, and VT parser; then feeds a startup
 * help stream through the parser and renders it to the display.
 *
 * Keyboard sampling runs from the frame interrupt; main drains the captured
 * bytes to conn TX and does the slower terminal/render work.
 *
 * Startup stream: box with line-drawing, SGR test, and bridge usage notes.
 * After the baked stream, the default conn backend loops keyboard bytes back.
 * A real Interface 1 RS-232 backend can be selected at build time.
 */
#include "video.h"
#include "screen.h"
#include "vtparse.h"
#include "render.h"
#include "blit.h"
#include "conn.h"
#include "keymap.h"
#include "keybuf.h"
#include "build_meta.h"
#include "im2.h"
#include <z80.h>
#include <intrinsic.h>
#include <stdint.h>
#include <string.h>

static const u8 demo_stream[] =
    "\x1b[2J"
    "\x1b[H"
    "\x1b(0"
    "lqqqqqqqqqqqqqqqqqqqqqqqqqqqqqqqqqqqqqqqqqqqqqqqqqqqqqqqqqqqqqqqqqqqqqqqqqqqqqqk\r\n"
    "x VT-102 TERMINAL EMULATOR                         80 columns, TT3000 6x8 font x\r\n"
    "x   \x1b[7mREVERSE\x1b[0m \x1b[4mUNDERLINE\x1b[0m test                                                     x\r\n"
    "mqqqqqqqqqqqqqqqqqqqqqqqqqqqqqqqqqqqqqqqqqqqqqqqqqqqqqqqqqqqqqqqqqqqqqqqqqqqqqqj"
    "\x1b(B"
    "\x1b[5;1H"
    "Version v" APP_VERSION_STR "  " APP_GIT_COMMIT "\r\n"
    "Built " APP_BUILD_DATE "\r\n"
    "\r\n"
    "Bridge quick help:\r\n"
    "  macOS -> Timex: pipe text through bridge.\r\n"
    "  Timex -> macOS: type here; bridge writes stdout.\r\n"
    "  make bridge-zrcp: immediate keys + local echo.\r\n"
    "  For text files, use --input-newline crlf.\r\n"
    "  ENTER sends CR. CAPS+0 sends Ctrl-H backspace.\r\n"
    "  SYMBOL+0 sends underscore (_). Raw mode is optional.\r\n"
    "\r\n"
    "Ready.";

static volatile u8 key_overrun;
static volatile u8 keyboard_settle_frames;
static u8 irq_keybuf_tmp[KEYMAP_OUT_MAX];
static u8 irq_nkeys;

#define IO_BATCH_SIZE 32u

static void beep_bell(void);
static void beep_overrun(void);

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
        for (i = 0; i < n; ++i) {
            if (buf[i] == '\n' || (scr->wrap_pending && buf[i] >= 0x20u)) {
                blit_flush(scr);
            }
            old_scroll_seq = scr->scroll_seq;
            vt_feed(vt, scr, buf[i]);
            if (vt_take_bell(vt)) {
                beep_bell();
            }
            if (scr->scroll_seq != old_scroll_seq) {
                if (blit_scroll_region(scr, scr->last_scroll_top,
                                         scr->last_scroll_bot,
                                         scr->last_scroll_n)) {
                    changed = 1;
                }
            }
            pump_vt_replies(vt);
        }
        if (n != 0) {
            changed = 1;
        }
    } while (n != 0);

    return changed;
}

static void beep_tone(u8 cycles, u8 delay)
{
    u8 i, j;
    for (i = 0; i < cycles; ++i) {
        z80_outp(0xFEu, 0x10u);
        for (j = 0; j < delay; ++j) {
            (void)z80_inp(0xFEu);
        }
        z80_outp(0xFEu, 0x00u);
        for (j = 0; j < delay; ++j) {
            (void)z80_inp(0xFEu);
        }
    }
}

static void beep_bell(void)
{
    beep_tone(60u, 36u);
}

static void beep_overrun(void)
{
    beep_tone(80u, 24u);
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

static void install_keyboard_im2(void)
{
    u8 *table = (u8 *)(uintptr_t)IM2_TABLE_BASE;
    u8 *tramp = (u8 *)(uintptr_t)IM2_TRAMPOLINE;
    u16 isr = (u16)(uintptr_t)&keyboard_im2_isr;

    intrinsic_di();

    /* 257 entries: the vector read can land on the last table byte and still
     * needs a high byte after it. */
    memset(table, IM2_TABLE_FILL, 257u);

    tramp[0] = 0xC3u;              /* JP nnnn */
    tramp[1] = (u8)(isr & 0xFFu);
    tramp[2] = (u8)(isr >> 8);

    __asm
        ld      a,#IM2_VECTOR_PAGE
        ld      i,a
        im      2
        ei
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

/*
 * File-scope, not locals of main(), for the same reason blit_hires.c keeps
 * row_glyphs/row_attrs/row_pixels off the stack: a Z80 stack frame holding
 * them would be enormous, and the linker cannot see stack usage at all, only
 * BSS. screen_t is 24*80 cell_t plus its scalars (COLS=80, ROWS=24) and
 * vtparse_t sits beside it; together they were the dominant part of a
 * measured 4,123-byte main() stack frame (SP seed 0xFF58 down to a low-water
 * mark of 0xEF3D -- see include/im2.h and the fix-wave report) even though
 * main() never recurses and has no other large locals. Moving them here
 * turns that frame into BSS that tools/check_image_limit.py's __BSS_END_tail
 * reading actually accounts for, instead of stack depth the linker never
 * modeled. Safe because main() runs once, is never re-entered, and both are
 * always passed by pointer to the functions that use them.
 */
static screen_t scr;
static vtparse_t vt;

int main(void)
{
    u16 i = 0;
    u8 conn_flags;

    /* Init hardware: hi-res white-on-black, clear screen. */
    video_init(1);  /* white-on-black = 1 */
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
    blit_flush(&scr);
    blit_cursor_toggle(&scr);
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
        blit_cursor_toggle(&scr);          /* hide: the parser may move it */
        if (pump_conn(&vt, &scr)) {
            sync_keyboard_modes(&scr);
            blit_flush(&scr);
        }
        blit_cursor_toggle(&scr);          /* show at its (possibly new) place */
        pump_vt_replies(&vt);
        conn_poll();
    }
}
