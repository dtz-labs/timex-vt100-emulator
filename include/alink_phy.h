/*
 * alink_phy.h -- audio link physical layer (target only).
 *
 * Emits and decodes the wire format described in
 * docs/superpowers/specs/2026-08-02-audio-link-design.md:
 *
 *   [48 x pilot 2168 T] [sync 667 T] [sync 735 T] [frame] [leadout] [closing edge]
 *
 * Data is MSB first, one bit per pair of equal half-pulses: 855 T for a 0,
 * 1710 T for a 1 -- the ROM's own bit timings. No ROM routine is called: the
 * design's decision D1 rules them out, which is what keeps this working on a
 * TS2068 whose ROM has none of them at the usual addresses.
 *
 * TARGET ONLY. This file touches absolute ports and its loops are
 * cycle-counted, so it is never linked into a host test -- the same rule
 * blit_hires.c / blit_ula.c follow. tools/alink/phy.py is the host-testable
 * mirror of the same format.
 *
 * The closing edge is not decoration. A receiver measures a half-pulse
 * BETWEEN two edges, so the final half-pulse of a transmission has no width
 * until something ends it; without the closing edge the last bit never
 * completes and the frame is never delivered.
 */
#ifndef ALINK_PHY_H
#define ALINK_PHY_H

#include "types.h"

/* Pulse widths in T-states at 3.5 MHz. These are the targets; the delay-loop
 * iteration counts that produce them are measured, not derived -- see
 * docs/perf/benchmarks.md, "Audio link PHY timings". */
#define ALINK_PHY_PILOT_T  2168u
#define ALINK_PHY_SYNC1_T   667u
#define ALINK_PHY_SYNC2_T   735u
#define ALINK_PHY_ZERO_T    855u
#define ALINK_PHY_ONE_T    1710u

#define ALINK_PHY_PREAMBLE_PULSES 48u
#define ALINK_PHY_LEADOUT          0u    /* measured; see src/alink_phy.c */

/* Port 0xFE: bit 3 is MIC, bit 4 the speaker, bits 0-2 the border. */
#define ALINK_PHY_MIC_BIT 0x08u

/* Capture the border colour the port writes must preserve. Call once at
 * startup, before any send. */
void alink_phy_init(void);

/* Transmit one block: preamble, sync pair, `n` bytes, ALINK_PHY_LEADOUT zero
 * bytes and the closing edge. Runs with interrupts disabled throughout and
 * re-enables them on exit, so the IM2 keyboard scanner resumes immediately.
 * `n` must be at most ALINK_FRAME_MAX. */
void alink_phy_send(const u8 *block, u8 n);

#endif /* ALINK_PHY_H */
