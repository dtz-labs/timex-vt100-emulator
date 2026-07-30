/*
 * machine.c -- SCLD presence probe and the boot-time override (target only).
 *
 * Ported from the reference detector in the attribute-wars repository,
 * docs/hardware/detect_machine_ay.asm. Only the SCLD half is kept: with one TAP
 * per machine there is no mode to select, so the AY probes that told a TC2048
 * from a TS2068 would have served nothing but a banner string.
 *
 * MUST run before video initialisation and before the IM2 handler is installed,
 * with interrupts disabled: it writes port 0xFF, which on a Timex is the display
 * mode register.
 *
 * The probe toggles ONLY bits 3..5 -- the palette bits -- so screen mode,
 * interrupt control and EXROM/DOCK selection are preserved, and it restores the
 * original value on both exits. On a Spectrum port 0xFF is unattached: the write
 * is harmless and the read-back does not match.
 */
#include "machine.h"
#include <z80.h>

#define PORT_SCLD 0x00FFu
#define PORT_KEY_CAPS 0xFEFEu

static u8 has_scld;
static u8 probed;

u8 machine_has_scld(void)
{
    static const u8 bits[3] = { 0x08u, 0x10u, 0x20u };
    u8 original;
    u8 pattern;
    u8 i;

    if (probed) {
        return has_scld;
    }

    original = (u8)z80_inp(PORT_SCLD);
    has_scld = 1u;

    for (i = 0; i < 3u; ++i) {
        pattern = (u8)(original ^ bits[i]);
        z80_outp(PORT_SCLD, pattern);
        if ((u8)z80_inp(PORT_SCLD) != pattern) {
            has_scld = 0u;
            break;
        }
    }

    z80_outp(PORT_SCLD, original);   /* harmless on a Spectrum */
    probed = 1u;
    return has_scld;
}

u8 machine_caps_shift_held(void)
{
    /* Read directly: keymap and the IM2 handler are not running this early. */
    return (u8)(((u8)z80_inp(PORT_KEY_CAPS) & 0x01u) == 0u);
}
