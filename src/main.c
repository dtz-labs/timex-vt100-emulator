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
 * RULE_TOP/RULE_BOTTOM/ROW_TITLE/ROW_SGR are sentinel bytes standing in for
 * the box's two horizontal rules and two interior content lines, which
 * main()'s feed loop routes to feed_hrule()/feed_row() (below) instead of
 * COLS-wide literal text -- see those functions for why. All four codes are
 * C0 values with no meaning to the VT parser (ground_byte's `default:
 * ignore` in vtparse.c), so the feed loop must intercept them itself and
 * never hand them to vt_feed; they are not escape sequences and carry no
 * other significance.
 *
 * The two interior strings (row_title, row_sgr) are shared, unpadded text --
 * feed_row() pads them to whichever COLS the build actually has. They must
 * fit within COLS - 2 visible columns on the *narrower* build (38, at
 * COLS=40u), since that is the tightest constraint either geometry imposes;
 * a string that fits at 80 but wraps at 40 would be the same "looks broken
 * at 40 columns" bug this rewrite exists to fix, just moved from the rules
 * into the content. row_title (24 visible chars) and row_sgr (25, not
 * counting its two SGR escape pairs, which feed_row measures around rather
 * than counts -- see feed_row) both clear that with room to spare; the
 * original row_title's "80 columns, TT3000 6x8 font" aside was dropped
 * (it was also flatly wrong on the ZX build, which is not 80 columns) since
 * the HW line printed after the box already states the actual width.
 */
#define RULE_TOP    0x01u
#define RULE_BOTTOM 0x02u
#define ROW_TITLE   0x03u
#define ROW_SGR     0x04u

static const char row_title[] = "VT-102 TERMINAL EMULATOR";
static const char row_sgr[]   = "   \x1b[7mREVERSE\x1b[0m \x1b[4mUNDERLINE\x1b[0m test";

/*
 * G1 (not G0) is designated as DEC special graphics here, once, and stays
 * that way for the program's whole run -- G0 stays the default ASCII
 * (vt_init() never changes it). feed_hrule()/feed_row() toggle GL between
 * the two with SO (0x0E, invoke G1) / SI (0x0F, invoke G0) around just the
 * corner/rule/vertical-bar bytes, and always leave GL on G0 (ASCII) when
 * they return. This replaces an earlier version that designated G0 itself
 * as graphics (ESC(0) for the whole box, borders *and* interior text, then
 * reset it once at the end (ESC(B) -- which meant every printable byte in
 * 0x5F-0x7E inside the interior lines (most lowercase letters and some
 * punctuation) was silently rendered as a DEC line-drawing glyph instead of
 * its own letter, on both builds, the whole time the interior text was
 * printed. Confirmed by a direct screen_t memory dump during Task 10's
 * verification: 'l' inside "columns" rendered as a corner glyph, 'x' inside
 * "6x8" as a vertical bar, etc. -- see charset_translate() in vtparse.c for
 * the exact translation this must avoid applying to plain text.
 */
static const u8 demo_stream[] =
    "\x1b[2J"
    "\x1b[H"
    "\x1b)0"
    "\x01"
    "\x03"
    "\x04"
    "\x02"
    "\x1b[5;1H"
    "Version v" APP_VERSION_STR "  " APP_GIT_COMMIT "\r\n"
    "Built " APP_BUILD_DATE "\r\n"
    "\r\n"
#ifdef CONN_BACKEND_AUDIO
    /* The bridge help describes the ZRCP workflow, which this build does not
     * have -- and the Timex audio image has no room to carry text about a
     * backend it was compiled without. */
    "Audio link. Waiting for HELLO.\r\n"
    "  ENTER sends CR. CAPS+0=Backspace.\r\n"
    "\r\n"
    "Ready.";
#else
    "Bridge quick help:\r\n"
    "  host -> target: pipe text via bridge.\r\n"
    "  target -> host: typed keys to host.\r\n"
    "  make bridge-zrcp: keys + local echo.\r\n"
    "  Text files: --input-newline crlf.\r\n"
    "  ENTER sends CR. CAPS+0=Backspace.\r\n"
    "  SYMBOL+0=underscore, raw mode opt.\r\n"
    "\r\n"
    "Ready.";
#endif

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
 * Draws one horizontal box rule: SO (invoke G1/graphics), a corner, COLS - 2
 * DEC special-graphics horizontal glyphs ('q'), the other corner, SI (back to
 * G0/ASCII), and an optional trailing CRLF (the original literal's top rule
 * ended the line; the bottom rule instead ran straight into the next thing).
 * A loop instead of a COLS-wide string literal is what lets the same source
 * close the box exactly at the right margin on both the 80-column Timex
 * build and the 40-column ZX build -- a literal sized for one geometry would
 * either fall short or overrun the other. Always leaves GL on G0 (ASCII)
 * when it returns, so whatever comes next -- another feed_row(), or plain
 * text after the box -- does not need to know or care that this function
 * used graphics glyphs internally.
 */
static void feed_hrule(vtparse_t *v, screen_t *s, char left, char right, u8 crlf)
{
    u8 i;

    vt_feed(v, s, 0x0Eu);   /* SO: invoke G1 (graphics) into GL */
    vt_feed(v, s, (u8)left);
    for (i = 0; i < (u8)(COLS - 2u); ++i) {
        vt_feed(v, s, (u8)'q');
    }
    vt_feed(v, s, (u8)right);
    vt_feed(v, s, 0x0Fu);   /* SI: invoke G0 (ASCII) into GL */
    if (crlf) {
        vt_feed(v, s, (u8)'\r');
        vt_feed(v, s, (u8)'\n');
    }
}

/*
 * Draws one interior content line: a graphics vertical bar, `text` (plain
 * ASCII, may embed SGR sequences), spaces padding out to column COLS - 1,
 * a closing graphics vertical bar, then CRLF -- all COLS columns wide on
 * either build, so the line never wraps and the closing bar always lands on
 * the right margin.
 *
 * The padding loop compares against scr->cx (the cursor's actual column)
 * rather than strlen(text), because `text` may contain SGR escape sequences
 * (the REVERSE/UNDERLINE demo does) that vt_feed consumes without moving the
 * cursor; measuring by strlen() would count those bytes as columns and pad
 * too little. This relies on being called with the cursor at column 0 (true
 * right after a preceding CRLF or CUP, which is how main() uses it) and on
 * `text`'s visible width being at most COLS - 2 -- the caller's job, since
 * this function has no way to detect an overlong string other than letting
 * it overrun the right margin.
 */
static void feed_row(vtparse_t *v, screen_t *s, const char *text)
{
    vt_feed(v, s, 0x0Eu);   /* SO: left vertical bar in graphics */
    vt_feed(v, s, (u8)'x');
    vt_feed(v, s, 0x0Fu);   /* SI: interior text in plain ASCII */
    vt_feed_text(v, s, text);
    while (s->cx < (u8)(COLS - 1u)) {
        vt_feed(v, s, (u8)' ');
    }
    vt_feed(v, s, 0x0Eu);   /* SO: right vertical bar in graphics */
    vt_feed(v, s, (u8)'x');
    vt_feed(v, s, 0x0Fu);   /* SI: back to ASCII for whatever follows */
    vt_feed(v, s, (u8)'\r');
    vt_feed(v, s, (u8)'\n');
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
                } else {
                    /* blit_scroll_region() only handles n == +-1 today (see
                     * blit_hires.c/blit_ula.c); a return of 0 means it moved
                     * no hardware pixels at all for this scroll -- e.g. a
                     * future SU/SD (CSI S / CSI T) with |n| > 1. The model
                     * already shifted rows via scroll_region()'s dirty-mark
                     * migration, which assumes the hardware scroll it rides
                     * on happened; if it did not, the moved region must be
                     * re-dirtied here or this silently reintroduces C1 (IL/DL
                     * corrupting the display) for any n the blitter refuses. */
                    u8 r;
                    for (r = scr->last_scroll_top; r <= scr->last_scroll_bot; ++r) {
                        screen_mark_row(scr, r);
                    }
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
     * and ROW_TITLE/ROW_SGR sentinels instead call feed_hrule()/feed_row()
     * so the whole box -- rules and interior content alike -- is generated
     * for the actual COLS width rather than baked into the literal at one
     * fixed width. */
    while (demo_stream[i] != 0) {
        switch (demo_stream[i]) {
        case RULE_TOP:
            feed_hrule(&vt, &scr, 'l', 'k', 1);
            break;
        case RULE_BOTTOM:
            feed_hrule(&vt, &scr, 'm', 'j', 0);
            break;
        case ROW_TITLE:
            feed_row(&vt, &scr, row_title);
            break;
        case ROW_SGR:
            feed_row(&vt, &scr, row_sgr);
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
