/*
 * alink_txprobe.c -- target-only probe: transmit one known frame on MIC,
 * forever, with a gap between transmissions.
 *
 * This exists because the terminal's own upstream transmissions cannot be
 * captured while a tape is playing. ZEsarUX has no MIC capture: what --aofile
 * records is the audio output, and the MIC bit only reaches it through the
 * ULA's EAR feedback (`IN A,(0xFE)` bit 6 reads back the last MIC bit written
 * when nothing is connected to EAR -- issue 2/3 behaviour, which ZEsarUX
 * emulates). Insert a --realtape and EAR comes from the tape instead, so the
 * feedback is gone and every upstream frame is invisible for the whole run.
 * Measured, not assumed: see test/alink_zesarux.py's module docstring.
 *
 * So the two directions are probed separately. test/alink_zesarux.py runs the
 * real terminal image with a tape for the downstream direction, and runs THIS
 * with no tape at all for the upstream one, decoding the capture with
 * tools/alink/phy.py. It is the automated form of the measurement Task 2 made
 * by hand (98 blocks, 100% CRC).
 *
 * The gap between transmissions is not padding. A decoder that only ever sees
 * back-to-back frames never has to re-acquire, and that is precisely the case
 * that hid a decoder bug until this harness was written: an isolated burst
 * starts from silence, and an estimator that needs to converge has nothing to
 * converge from. Sending one frame per gap keeps the probe honest about the
 * shape of the traffic a real session produces.
 *
 * TARGET ONLY, like src/alink_phy.c itself: never linked into a host test.
 */
#include "types.h"
#include "alink.h"
#include "alink_phy.h"

/* File scope, not locals: this project's rule for anything of size, after a
 * main()-local screen_t once overflowed the stack into the IM2 table. */
static u8 probe_block[ALINK_FRAME_MAX];
static alink_frame_t probe_frame;

/* Roughly a quarter second at 3.5 MHz -- the same order as the pause between
 * transactions in a live session. `volatile` so -SO3 cannot delete the loop. */
static void probe_gap(void)
{
    volatile u16 i;

    for (i = 0; i != 0x4000u; ++i) {
    }
}

int main(void)
{
    u8 n;

    alink_phy_init();

    /* A frame the harness can predict byte for byte: it encodes the same one
     * with alink.frame.encode() and compares. SEQ=1/ACK=0 is a bit pattern
     * that cannot be confused with an all-zero block. */
    probe_frame.type = ALINK_TYPE_LINK;
    probe_frame.seq = 1u;
    probe_frame.ack = 0u;
    probe_frame.len = 8u;
    probe_frame.payload[0] = 'T';
    probe_frame.payload[1] = 'X';
    probe_frame.payload[2] = ' ';
    probe_frame.payload[3] = 'P';
    probe_frame.payload[4] = 'R';
    probe_frame.payload[5] = 'O';
    probe_frame.payload[6] = 'B';
    probe_frame.payload[7] = 'E';
    n = alink_frame_encode(&probe_frame, probe_block);

    for (;;) {
        alink_phy_send(probe_block, n);
        probe_gap();
    }
}
