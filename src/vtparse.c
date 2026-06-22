/*
 * vtparse.c -- VT-100/ANSI escape-sequence state machine. See vtparse.h.
 *
 * Structured as a small explicit state machine (the VT500 parser shape, pared
 * to the §6 subset): GROUND handles printables and C0 controls; ESC and CSI
 * states collect and dispatch escape sequences. Each sequence group is added
 * test-first.
 */
#include "vtparse.h"

/* Internal parser states. GROUND == 0 so vt_init's zeroing lands here. */
enum {
    VT_S_GROUND = 0,
    VT_S_ESC,          /* ESC seen, awaiting the next byte                 */
    VT_S_CSI           /* ESC [ seen, collecting params / awaiting final   */
};

void vt_init(vtparse_t *vt)
{
    u8 i;
    vt->state = VT_S_GROUND;
    vt->nparams = 0;
    vt->has_digit = 0;
    vt->priv = 0;
    vt->nout = 0;
    for (i = 0; i < VT_MAX_PARAMS; ++i) {
        vt->params[i] = 0;
    }
}

/* Advance the cursor to the next horizontal tab stop (every 8 columns),
 * clamped to the last column. */
static void do_tab(screen_t *s)
{
    u8 stop = (u8)(((s->cx / 8u) + 1u) * 8u);
    if (stop >= COLS) {
        stop = COLS - 1;
    }
    screen_cup(s, s->cy, stop);
}

/* GROUND: a printable goes to the grid; recognised C0 controls act; the rest
 * are ignored. ESC is handled by the caller (state transition). */
static void ground_byte(screen_t *s, u8 b)
{
    if (b >= 0x20 && b != 0x7F) {     /* printable (incl. 0xA0-0xFF for now) */
        screen_putc(s, b);
        return;
    }
    switch (b) {
    case 0x08:                        /* BS: left one, stop at column 0 */
        if (s->cx > 0) {
            screen_cup(s, s->cy, (u8)(s->cx - 1));
        }
        break;
    case 0x09:                        /* HT */
        do_tab(s);
        break;
    case 0x0A:                        /* LF */
    case 0x0B:                        /* VT  -> treated as LF */
    case 0x0C:                        /* FF  -> treated as LF */
        screen_lf(s);
        break;
    case 0x0D:                        /* CR */
        screen_cr(s);
        break;
    default:                          /* BEL (0x07), DEL, other C0: ignore */
        break;
    }
}

/* ESC state: dispatch the single-byte escape finals of the §6 subset. (CSI
 * '[' and charset designators are added in later slices.) */
static void esc_byte(vtparse_t *vt, screen_t *s, u8 b)
{
    switch (b) {
    case 'D':  screen_lf(s); break;                 /* IND  */
    case 'M':  screen_ri(s); break;                 /* RI   */
    case 'E':  screen_cr(s); screen_lf(s); break;   /* NEL  */
    case '7':  screen_save_cursor(s); break;        /* DECSC */
    case '8':  screen_restore_cursor(s); break;     /* DECRC */
    case 'c':  screen_init(s); break;               /* RIS  */
    default:   break;                               /* unsupported: ignore */
    }
    vt->state = VT_S_GROUND;
}

void vt_feed(vtparse_t *vt, screen_t *s, u8 b)
{
    if (b == 0x1B) {                  /* ESC anywhere (re)starts a sequence */
        vt->state = VT_S_ESC;
        return;
    }
    switch (vt->state) {
    case VT_S_ESC:
        esc_byte(vt, s, b);
        break;
    case VT_S_GROUND:
    default:
        ground_byte(s, b);
        break;
    }
}
