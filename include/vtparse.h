/*
 * vtparse.h -- VT-100/ANSI escape-sequence state machine (pure, host-tested).
 *
 * Consumes the incoming host byte stream one byte at a time and drives the
 * cell-grid model (screen.c). It owns no hardware and no pixels: it translates
 * bytes into screen_* calls (the §6 VT-100 subset). Query replies (DSR, DA)
 * are accumulated in an output buffer for main to drain to the host via conn,
 * so the parser stays pure and host-testable.
 *
 * Per the toolchain rule (SDCC z80 struct-return crash), state is passed via a
 * vtparse_t out-pointer; nothing returns a struct.
 */
#ifndef VTPARSE_H
#define VTPARSE_H

#include "types.h"
#include "screen.h"

#define VT_MAX_PARAMS 16u   /* CSI numeric params kept (excess dropped)      */
#define VT_OUT_MAX    16u   /* reply bytes buffered (DSR report ~ ESC[24;64R) */
#define VT_TAB_BYTES  ((COLS + 7u) / 8u)  /* programmable HT stops bitmap     */

typedef struct {
    u8 state;                      /* parser state (internal VT_S_* codes)   */
    u8 params[VT_MAX_PARAMS];      /* collected CSI numeric parameters        */
    u8 nparams;                    /* number of params seen (>=1 once parsing)*/
    u8 has_digit;                  /* a digit seen for the current param      */
    u8 priv;                       /* CSI private prefix byte 0x3C-0x3F, or 0 */
    u8 g0, g1;                      /* designated charsets: 'B' ASCII / '0' graph */
    u8 gl;                          /* active charset in GL: 0 = G0, 1 = G1    */
    u8 tabs[VT_TAB_BYTES];          /* horizontal tab stops, one bit per column */
    u8 out[VT_OUT_MAX];            /* pending reply bytes for the host        */
    u8 nout;                       /* number of pending reply bytes           */
} vtparse_t;

/* Reset the parser to the GROUND state with empty buffers. */
void vt_init(vtparse_t *vt);

/* Feed one byte from the host: advances the state machine, mutating the screen
 * grid and/or appending reply bytes to vt->out. */
void vt_feed(vtparse_t *vt, screen_t *s, u8 byte);

#endif /* VTPARSE_H */
