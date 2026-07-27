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
#include "machine.h"
#include <z80.h>
#include <intrinsic.h>
#include <stdint.h>
#include <string.h>

#ifdef TERM_TIMEX
#define BANNER_HW "Timex (SCLD)  80x24 hi-res"
#else
#define BANNER_HW "ZX Spectrum (ULA)  40x24"
#endif

/*
 * RULE_TOP/RULE_BOTTOM are sentinel bytes standing in for the box's two
 * horizontal rules, which main()'s feed loop draws with feed_hrule() (below)
 * instead of a COLS-wide run of 'q' literals -- see feed_hrule for why. Both
 * codes are C0 values with no meaning to the VT parser (ground_byte's
 * `default: ignore` in vtparse.c), so the feed loop must intercept them
 * itself and never hand them to vt_feed; they are not escape sequences and
 * carry no other significance. The interior content lines keep their fixed
 * 80-column layout: shortening them for the 40-column ZX build is a separate
 * concern from closing the box, and out of scope here.
 */
#define RULE_TOP    0x01u
#define RULE_BOTTOM 0x02u

static const u8 demo_stream[] =
    "\x1b[2J"
    "\x1b[H"
    "\x1b(0"
    "\x01"
    "x VT-102 TERMINAL EMULATOR                         80 columns, TT3000 6x8 font x\r\n"
    "x   \x1b[7mREVERSE\x1b[0m \x1b[4mUNDERLINE\x1b[0m test                                                     x\r\n"
    "\x02"
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

#ifdef TERM_TIMEX
/* The terminal's renderer is not initialised, and on a machine without an SCLD
 * it would paint an unreadable half-image. Use the boot-time ULA text mode. */
static void guard_refuse(void) __naked
{
    __asm
        di
        ld      hl,#guard_msg
guard_loop:
        ld      a,(hl)
        or      a
        jr      z,guard_halt
        inc     hl
        push    hl
        rst     #0x10
        pop     hl
        jr      guard_loop
guard_halt:
        halt
        jr      guard_halt
guard_msg:
        .ascii  "THIS BUILD NEEDS A TIMEX (SCLD)."
        .db     13
        .ascii  "USE THE -ZX TAP, OR HOLD CAPS SHIFT."
        .db     13,0
    __endasm;
}
#endif

/* Used after the demo_stream feed loop in main(): feeds a NUL-terminated
 * C string through the parser one byte at a time. */
static void vt_feed_text(vtparse_t *v, screen_t *s, const char *p)
{
    while (*p != '\0') {
        vt_feed(v, s, (u8)*p);
        ++p;
    }
}

/*
 * Draws one horizontal box rule: a corner, COLS - 2 DEC special-graphics
 * horizontal glyphs ('q'), the other corner, and an optional trailing CRLF
 * (the original literal's top rule ended the line; the bottom rule instead
 * ran straight into "\x1b(B"). A loop instead of a COLS-wide string literal
 * is what lets the same source close the box exactly at the right margin on
 * both the 80-column Timex build and the 40-column ZX build -- a literal
 * sized for one geometry would either fall short or overrun the other. Must
 * run with the DEC special graphics charset already selected (ESC(0),
 * selected once near the start of demo_stream and still active at both
 * RULE_TOP/RULE_BOTTOM sentinels), since 'q' only becomes the
 * horizontal-line glyph under that charset -- see charset_translate() in
 * vtparse.c.
 */
static void feed_hrule(vtparse_t *v, screen_t *s, char left, char right, u8 crlf)
{
    u8 i;

    vt_feed(v, s, (u8)left);
    for (i = 0; i < (u8)(COLS - 2u); ++i) {
        vt_feed(v, s, (u8)'q');
    }
    vt_feed(v, s, (u8)right);
    if (crlf) {
        vt_feed(v, s, (u8)'\r');
        vt_feed(v, s, (u8)'\n');
    }
}

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
 * File-scope, not locals of main(), for the same reason blit_hires.c hoists
 * its per-row scratch state to file scope: a Z80 stack frame holding them
 * would be enormous, and the linker cannot see stack usage at all, only BSS.
 * (Before Task 5, blit_hires.c held that state in file-scope
 * row_glyphs/row_attrs/row_pixels arrays; Task 5's per-group dirty rendering
 * replaced them with small per-group locals, so that specific example no
 * longer exists in the source, but the underlying principle -- and this
 * scr/vt hoist -- still does.) screen_t is 24*80 cell_t plus its scalars
 * (COLS=80, ROWS=24) and vtparse_t sits beside it; together they were the dominant part of a
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

#ifdef TERM_TIMEX
    /* This build sends half its pixels to 0x6000, which a plain Spectrum's
     * ULA never displays -- an unreadable half-image, with no clue why, after
     * a tape load that costs minutes on real hardware. Refuse instead. Must
     * run before video_init: the probe writes port 0xFF, the display mode
     * register, and CAPS SHIFT is the documented bypass for clones whose port
     * decoding fools the probe.
     *
     * machine_has_scld() documents (machine.h) that it must run with
     * interrupts disabled, since it toggles port 0xFF -- so di/ei bracket
     * only the probe itself, not video_clear() or anything after. If the
     * guard fires, guard_refuse() never returns (it di's again, prints, and
     * halts), so the ei below is reached only on a genuine Timex. */
    intrinsic_di();
    if (!machine_has_scld() && !machine_caps_shift_held()) {
        guard_refuse();     /* prints and halts; never returns */
    }
    intrinsic_ei();
#endif

    /* Init hardware: hi-res white-on-black, clear screen. */
    video_init(1);  /* white-on-black = 1 */
    video_clear();

    /* Init software: screen grid and VT parser. */
    screen_init(&scr);
    vt_init(&vt);
    conn_init();
    keybuf_init();
    sync_keyboard_modes(&scr);

    /* Feed demo stream through parser. Printable bytes and escape sequences
     * go through vt_feed one at a time as before; the RULE_TOP/RULE_BOTTOM
     * sentinels instead call feed_hrule() so the box's horizontal rules are
     * generated for the actual COLS width rather than baked into the
     * literal at one fixed width. */
    while (demo_stream[i] != 0) {
        switch (demo_stream[i]) {
        case RULE_TOP:
            feed_hrule(&vt, &scr, 'l', 'k', 1);
            break;
        case RULE_BOTTOM:
            feed_hrule(&vt, &scr, 'm', 'j', 0);
            break;
        default:
            vt_feed(&vt, &scr, demo_stream[i]);
            break;
        }
        ++i;
    }

    /* Banner names the build (compile-time fact), not a measurement -- so
     * naming stays correct even if a clone's port decoding fools the probe. */
    vt_feed_text(&vt, &scr, "\r\nHW: " BANNER_HW "\r\n");

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
