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
    VT_S_CSI,          /* ESC [ seen, collecting params / awaiting final   */
    VT_S_CHARSET_G0,   /* ESC ( seen, awaiting the G0 charset selector     */
    VT_S_CHARSET_G1    /* ESC ) seen, awaiting the G1 charset selector     */
};

void vt_init(vtparse_t *vt)
{
    u8 i;
    vt->state = VT_S_GROUND;
    vt->nparams = 0;
    vt->has_digit = 0;
    vt->priv = 0;
    vt->g0 = 'B';                  /* both charsets default to ASCII */
    vt->g1 = 'B';
    vt->gl = 0;                    /* GL = G0 */
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

/* Translate a printable byte through the active charset. In DEC special
 * graphics ('0'), bytes 0x5F-0x7E become the line-drawing glyphs, mapped to a
 * high-bit font page (ch | 0x80) so the cell model stays {ch, attr}; render.c
 * / font.c (Task C) hold the box-drawing glyphs at 0x80-0xFE. ASCII passes
 * through unchanged. */
static u8 charset_translate(const vtparse_t *vt, u8 b)
{
    u8 active = vt->gl ? vt->g1 : vt->g0;
    if (active == '0' && b >= 0x5F && b <= 0x7E) {
        return (u8)(b | 0x80);
    }
    return b;
}

/* GROUND: a printable goes to the grid; recognised C0 controls act; the rest
 * are ignored. ESC is handled by the caller (state transition). */
static void ground_byte(vtparse_t *vt, screen_t *s, u8 b)
{
    if (b >= 0x20 && b != 0x7F) {     /* printable (incl. 0xA0-0xFF for now) */
        screen_putc(s, charset_translate(vt, b));
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
    case 0x0E:                        /* SO: select G1 into GL */
        vt->gl = 1;
        break;
    case 0x0F:                        /* SI: select G0 into GL */
        vt->gl = 0;
        break;
    default:                          /* BEL (0x07), DEL, other C0: ignore */
        break;
    }
}

/* CSI param at idx with the "default 1" rule (omitted or 0 -> 1): cursor moves,
 * CUP/HVP coordinates, IL/DL/ICH/DCH counts. */
static u8 param1(const vtparse_t *vt, u8 idx)
{
    u8 v = (idx < vt->nparams) ? vt->params[idx] : 0;
    return (v == 0) ? 1 : v;
}

/* CSI param at idx with the "default 0" rule (omitted -> 0): ED/EL modes, SGR. */
static u8 param0(const vtparse_t *vt, u8 idx)
{
    return (idx < vt->nparams) ? vt->params[idx] : 0;
}

/* Append a reply byte for the host, dropping it if the buffer is full. */
static void emit(vtparse_t *vt, u8 b)
{
    if (vt->nout < VT_OUT_MAX) {
        vt->out[vt->nout++] = b;
    }
}

/* Append a 0..255 value as decimal ASCII. */
static void emit_num(vtparse_t *vt, u8 n)
{
    u8 d[3];
    u8 i = 0;
    if (n == 0) {
        emit(vt, '0');
        return;
    }
    while (n > 0) {
        d[i++] = (u8)('0' + (n % 10u));
        n = (u8)(n / 10u);
    }
    while (i > 0) {
        emit(vt, d[--i]);
    }
}

/* Move the cursor by (dy, dx), clamped to the grid (never wraps). */
static void cursor_move(screen_t *s, int dy, int dx)
{
    int ny = (int)s->cy + dy;
    int nx = (int)s->cx + dx;
    if (ny < 0) { ny = 0; }
    if (nx < 0) { nx = 0; }
    if (ny > (int)ROWS - 1) { ny = (int)ROWS - 1; }
    if (nx > (int)COLS - 1) { nx = (int)COLS - 1; }
    screen_cup(s, (u8)ny, (u8)nx);
}

/* Begin a fresh CSI sequence: clear params and the private marker. */
static void csi_reset(vtparse_t *vt)
{
    u8 i;
    vt->nparams = 0;
    vt->has_digit = 0;
    vt->priv = 0;
    for (i = 0; i < VT_MAX_PARAMS; ++i) {
        vt->params[i] = 0;
    }
}

/* Act on a complete CSI sequence (final byte b). Unhandled finals are ignored;
 * more are added in later slices (erase, edit, SGR, modes, queries). */
static void csi_dispatch(vtparse_t *vt, screen_t *s, u8 b)
{
    switch (b) {
    case 'A': cursor_move(s, -(int)param1(vt, 0), 0); break;  /* CUU */
    case 'B': cursor_move(s,  (int)param1(vt, 0), 0); break;  /* CUD */
    case 'C': cursor_move(s, 0,  (int)param1(vt, 0)); break;  /* CUF */
    case 'D': cursor_move(s, 0, -(int)param1(vt, 0)); break;  /* CUB */
    case 'H':                                                 /* CUP */
    case 'f':                                                 /* HVP */
        screen_cup(s, (u8)(param1(vt, 0) - 1u), (u8)(param1(vt, 1) - 1u));
        break;
    case 'J': screen_erase_display(s, param0(vt, 0)); break;  /* ED  */
    case 'K': screen_erase_line(s, param0(vt, 0));    break;  /* EL  */
    case 'L': screen_insert_lines(s, param1(vt, 0));  break;  /* IL  */
    case 'M': screen_delete_lines(s, param1(vt, 0));  break;  /* DL  */
    case '@': screen_insert_chars(s, param1(vt, 0));  break;  /* ICH */
    case 'P': screen_delete_chars(s, param1(vt, 0));  break;  /* DCH */
    case 'n':                                                 /* DSR */
        if (param0(vt, 0) == 6) {           /* cursor position report */
            emit(vt, 0x1B);
            emit(vt, '[');
            emit_num(vt, (u8)(s->cy + 1u));
            emit(vt, ';');
            emit_num(vt, (u8)(s->cx + 1u));
            emit(vt, 'R');
        }
        break;
    case 'c':                                                 /* DA */
        if (vt->priv != '>') {              /* primary DA -> VT-100 identity */
            emit(vt, 0x1B);
            emit(vt, '[');
            emit(vt, '?');
            emit(vt, '1');
            emit(vt, ';');
            emit(vt, '0');
            emit(vt, 'c');
        }
        break;
    case 'm':                                                 /* SGR */
        if (vt->nparams == 0) {
            screen_set_attr(s, 0);          /* ESC[m == ESC[0m */
        } else {
            u8 i;
            for (i = 0; i < vt->nparams; ++i) {
                screen_set_attr(s, vt->params[i]);
            }
        }
        break;
    case 'r': {                                               /* DECSTBM */
        u8 top = param1(vt, 0);             /* default 1 */
        u8 botp = param0(vt, 1);
        u8 bot = (botp == 0) ? (u8)ROWS : botp;   /* default = last line */
        screen_set_scroll_region(s, (u8)(top - 1u), (u8)(bot - 1u));
        break;
    }
    case 'h':                                                 /* SM  */
    case 'l': {                                               /* RM  */
        u8 on = (b == 'h');
        if (vt->priv == '?') {              /* only the DEC private modes (?Pn) */
            u8 i;
            for (i = 0; i < vt->nparams; ++i) {
                switch (vt->params[i]) {
                case 7:  screen_set_mode(s, MODE_AUTOWRAP, on);       break;  /* DECAWM */
                case 25: screen_set_mode(s, MODE_CURSOR_VISIBLE, on); break;  /* DECTCEM */
                default: break;             /* ?1 DECCKM realised in keymap; rest ignored */
                }
            }
        }
        /* non-private ANSI modes: none in the §6 subset -> ignored */
        break;
    }
    default:
        break;
    }
}

/* CSI state: collect numeric params (';'-separated), the '?' private marker,
 * and ignore intermediates, until a final byte dispatches the sequence. */
static void csi_byte(vtparse_t *vt, screen_t *s, u8 b)
{
    if (b >= '0' && b <= '9') {
        u16 v;
        if (vt->nparams == 0) {
            vt->nparams = 1;
        }
        v = (u16)((u16)vt->params[vt->nparams - 1] * 10u + (u16)(b - '0'));
        vt->params[vt->nparams - 1] = (v > 255u) ? 255u : (u8)v;
        vt->has_digit = 1;
        return;
    }
    if (b == ';') {
        if (vt->nparams == 0) {
            vt->nparams = 1;              /* empty first param -> default */
        }
        if (vt->nparams < VT_MAX_PARAMS) {
            vt->nparams++;
            vt->params[vt->nparams - 1] = 0;
        }
        vt->has_digit = 0;
        return;
    }
    if (b >= 0x3C && b <= 0x3F) {
        vt->priv = b;                     /* private prefix: < = > ? (store byte) */
        return;
    }
    if (b >= 0x20 && b <= 0x2F) {
        return;                           /* intermediate bytes: ignored */
    }
    csi_dispatch(vt, s, b);               /* final byte (0x40-0x7E) */
    vt->state = VT_S_GROUND;
}

/* Charset designator: 'B' (ASCII) or '0' (DEC special graphics); anything else
 * is treated as ASCII. gx points at g0 or g1. */
static void charset_byte(vtparse_t *vt, u8 *gx, u8 b)
{
    *gx = (b == '0') ? (u8)'0' : (u8)'B';
    vt->state = VT_S_GROUND;
}

/* ESC state: dispatch the single-byte escape finals of the §6 subset, enter the
 * CSI state, or begin a G0/G1 charset designation. */
static void esc_byte(vtparse_t *vt, screen_t *s, u8 b)
{
    switch (b) {
    case '[':  vt->state = VT_S_CSI; csi_reset(vt); return;  /* CSI entry */
    case '(':  vt->state = VT_S_CHARSET_G0; return;          /* designate G0 */
    case ')':  vt->state = VT_S_CHARSET_G1; return;          /* designate G1 */
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
    case VT_S_CSI:
        csi_byte(vt, s, b);
        break;
    case VT_S_CHARSET_G0:
        charset_byte(vt, &vt->g0, b);
        break;
    case VT_S_CHARSET_G1:
        charset_byte(vt, &vt->g1, b);
        break;
    case VT_S_GROUND:
    default:
        ground_byte(vt, s, b);
        break;
    }
}
