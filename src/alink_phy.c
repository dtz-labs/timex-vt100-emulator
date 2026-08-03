/*
 * alink_phy.c -- audio link physical layer (target only).
 *
 * TARGET ONLY: absolute ports, cycle-counted loops. Never linked into a host
 * test -- the same rule blit_hires.c / blit_ula.c follow.
 * tools/alink/phy.py is the host-testable mirror of this format.
 *
 * HOW THE TIMING WORKS
 *
 * A "pulse width" is the time from one OUT to the next OUT, so it includes
 * whatever bookkeeping the caller does in between. That bookkeeping differs
 * between the preamble, the sync pair and the data bits, so each has its own
 * delay constant rather than sharing one -- a single constant would produce
 * three different widths.
 *
 * Each constant is the DJNZ iteration count N in a loop costing 13 T per
 * iteration and 8 T on the last, so the delay is 13N - 5, and the width is
 * 13N - 5 plus that path's fixed overhead. The values below are computed from
 * those overheads and then confirmed by measurement; see docs/perf/benchmarks.md,
 * "Audio link PHY timings".
 *
 * WHY THE BIT-WIDTH SELECTION IS BRANCHLESS
 *
 * `rlc d` puts the next bit in carry; `sbc a,a` turns that into 0x00 or 0xFF
 * without a jump. A JR would cost 12 T when taken and 7 T when not, making a
 * one bit 5 T wider than a zero bit for reasons that have nothing to do with
 * the encoding. The decoder would survive that -- it classifies on the sum of
 * a half-pulse pair against a threshold with enormous margin -- but a timing
 * routine whose output depends on its data is a bad thing to have to reason
 * about later.
 */
#include "alink.h"
#include "alink_phy.h"

/*
 * Delay-loop iteration counts. Derived per path, then measured:
 *
 *   preamble  width = 13N + 33   -> N = 164 gives 2,165 T (target 2,168)
 *   sync 1    width = 13N + 33   -> N =  49 gives   670 T (target   667)
 *   sync 2    width = 13N + 33   -> N =  54 gives   735 T (target   735)
 *   bit 0     width = 13N + 27   -> N =  64 gives   859 T (target   855)
 *   bit 1     width = 13N + 27   -> N = 129 gives 1,704 T (target 1,710)
 *
 * Every one is inside 0.6% of its target, and the decoder derives all its
 * thresholds from the measured pilot width rather than from absolute time, so
 * a uniform error would cancel anyway. What must NOT drift apart is the ratio
 * between the pilot and the bit widths, which is what the classification
 * threshold is built from.
 */
/*
 * These reach the assembler through the preprocessor, so they must be bare
 * numbers: a `u` suffix or a parenthesised expression arrives verbatim and
 * sdasz80 rejects it. The #if below is what keeps them honest, since the
 * assembler cannot check them.
 */
#define N_PILOT 164
#define N_SYNC1  49
#define N_SYNC2  54
#define N_ZERO   64
#define N_ONE   129
#define N_DIFF   65             /* N_ONE - N_ZERO, precomputed for the asm */

/*
 * Leadout bytes after the CRC. MEASURED AT ZERO, not inherited.
 *
 * The proof of concept pads with 32 zero bytes because capture truncated a
 * transmission's tail by 8-16 bytes. Measured against real --aofile captures
 * (2026-08-03): 98/98 frames decoded WITH the leadout and 234/234 WITHOUT it,
 * in the same amount of audio. Nothing truncates on this path, and our frames
 * end by LENGTH rather than by silence, so a decoder stops needing input the
 * moment LEN+4 bytes have arrived -- the leadout could not have helped even if
 * something did.
 *
 * It cost 0.125 s on every upstream frame: 21% of a transaction, and the
 * difference between 106 and 134 B/s. Raise this if real-hardware capture
 * (as opposed to the emulator dump) ever proves to truncate.
 */
#define N_LEADOUT 0

/* Port value with MIC low. Bit 0 rides along with the MIC bit so the border
 * flashes during a transmission -- the same visual feedback the ROM's SAVE
 * gives, and the quickest way to see on screen that the link is alive. */
#define PORT_IDLE 0x00
#define PORT_XOR  0x09          /* ALINK_PHY_MIC_BIT | border bit 0 */

#if N_DIFF != (N_ONE - N_ZERO)
#error "N_DIFF must equal N_ONE - N_ZERO"
#endif
#if PORT_XOR != (ALINK_PHY_MIC_BIT | 0x01u)
#error "PORT_XOR must toggle the MIC bit and border bit 0"
#endif

static const u8 *phy_src;
static u8 phy_count;
static u8 phy_bitn;              /* delay count for the bit being sent */

void alink_phy_init(void)
{
    phy_src = 0;
    phy_count = 0;
    phy_bitn = N_ZERO;
}

/*
 * The transmitter, as one uninterrupted assembly routine.
 *
 * No comments inside the __asm block: this project's existing inline asm
 * (src/conn.c) carries none, and sdasz80 as invoked here rejects `;` comment
 * lines outright. The structure is:
 *
 *   alink_pre       48 pilot pulses          delay N_PILOT
 *   alink_s1/s2     the two sync pulses      delay N_SYNC1 / N_SYNC2
 *                   four NOPs after each, so the sync path costs the same
 *                   per-pulse overhead as the preamble path (DEC D + JR NZ)
 *   alink_byte      next frame byte, or fall through to the leadout
 *   alink_leadout   ALINK_PHY_LEADOUT zero bytes
 *   alink_bit       one bit: width chosen branchlessly, then two identical
 *                   half-pulses
 *   alink_done      the closing edge, then MIC and border back to idle
 *
 * Register use: C holds the live port value, D the byte being shifted out,
 * E the frame bytes remaining, HL the source pointer, B the delay counter.
 * IY is untouchable -- -clib=sdcc_iy reserves it -- so the bit and leadout
 * counters live in the two bytes declared at the end of the routine.
 */
static void phy_send_asm(void) __naked
{
    __asm
        di
        ld      c,#PORT_IDLE

        ld      d,#48
alink_pre:
        ld      a,c
        xor     #PORT_XOR
        ld      c,a
        out     (#0xfe),a
        ld      b,#N_PILOT
alink_pre_d:
        djnz    alink_pre_d
        dec     d
        jr      nz,alink_pre

        ld      a,c
        xor     #PORT_XOR
        ld      c,a
        out     (#0xfe),a
        ld      b,#N_SYNC1
alink_s1:
        djnz    alink_s1
        nop
        nop
        nop
        nop

        ld      a,c
        xor     #PORT_XOR
        ld      c,a
        out     (#0xfe),a
        ld      b,#N_SYNC2
alink_s2:
        djnz    alink_s2
        nop
        nop
        nop
        nop

        ld      hl,(_phy_src)
        ld      a,(_phy_count)
        ld      e,a
        ld      a,#N_LEADOUT
        ld      (_alink_leadout),a

alink_byte:
        ld      a,e
        or      a
        jr      z,alink_lead
        dec     e
        ld      d,(hl)
        inc     hl
        jr      alink_bits

alink_lead:
        ld      a,(_alink_leadout)
        or      a
        jr      z,alink_done
        dec     a
        ld      (_alink_leadout),a
        ld      d,#0

alink_bits:
        ld      a,#8
        ld      (_alink_bitcount),a

alink_bit:
        rlc     d
        sbc     a,a
        and     #N_DIFF
        add     a,#N_ZERO
        ld      (_phy_bitn),a

        ld      a,c
        xor     #PORT_XOR
        ld      c,a
        out     (#0xfe),a
        ld      a,(_phy_bitn)
        ld      b,a
alink_h1:
        djnz    alink_h1

        ld      a,c
        xor     #PORT_XOR
        ld      c,a
        out     (#0xfe),a
        ld      a,(_phy_bitn)
        ld      b,a
alink_h2:
        djnz    alink_h2

        ld      a,(_alink_bitcount)
        dec     a
        ld      (_alink_bitcount),a
        jr      nz,alink_bit
        jr      alink_byte

alink_done:
        ld      a,c
        xor     #PORT_XOR
        out     (#0xfe),a
        xor     a
        out     (#0xfe),a
        ei
        ret

_alink_bitcount:
        .db     0
_alink_leadout:
        .db     0
    __endasm;
}

void alink_phy_send(const u8 *block, u8 n)
{
    if (n > ALINK_FRAME_MAX) {
        return;
    }
    phy_src = block;
    phy_count = n;
    phy_send_asm();
}
