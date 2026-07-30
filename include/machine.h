/*
 * machine.h -- SCLD presence probe and the boot-time override (target only).
 *
 * Ported from the reference detector in the attribute-wars repository,
 * docs/hardware/detect_machine_ay.asm. Only the SCLD half survives here: with
 * one TAP per machine (see the Makefile's TERM_TIMEX/TERM_ZX split), detection
 * no longer selects a mode, so the AY probes that told a TC2048 from a TS2068
 * are cut -- they changed no pixel and existed only to print a nicer name.
 *
 * Port 0xFF is the Timex SCLD's display mode register: bits 0-2 select the
 * screen mode, bits 3-5 the palette, bit 6 is hardware DI, bit 7 unused. On a
 * plain Spectrum this port is unattached -- writes are harmless, and a
 * read-back never matches what was written. machine_has_scld() exploits
 * exactly that: it toggles the palette bits (3-5) three times, restores the
 * original value, and reports whether every write read back unchanged.
 *
 * Both functions are target-only hardware (they call z80_inp/z80_outp) and
 * must never be linked into a host test build.
 */
#ifndef MACHINE_H
#define MACHINE_H

#include "types.h"

/*
 * Probe port 0xFF once, on first call, and cache the result. Toggles only the
 * palette bits (0x08, 0x10, 0x20) and restores the original value on both
 * exits, so it is non-destructive on a Timex and harmless on a Spectrum.
 *
 * MUST be called before video_init() and before the IM2 handler is installed,
 * with interrupts disabled -- it writes the display mode register.
 *
 * Returns non-zero if an SCLD answered the probe (Timex TC2048/TC2068/TS2068),
 * zero otherwise (plain Spectrum, or a clone whose port decoding does not
 * echo the write).
 */
u8 machine_has_scld(void);

/*
 * Read keyboard half-row 0xFEFE (CAPS SHIFT, V, C, X, Z) directly -- the
 * keymap and the IM2 handler are not running this early in boot. Bit 0 is
 * CAPS SHIFT, active low.
 *
 * Returns non-zero if CAPS SHIFT is held down.
 */
u8 machine_caps_shift_held(void);

#endif /* MACHINE_H */
