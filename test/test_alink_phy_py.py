#!/usr/bin/env python3
"""Host tests for the audio link's pulse encoder and decoder.

Standalone script, no pytest -- matches the other Python suites in this repo.

These tests never touch audio hardware. They encode a block to pulse widths,
render those to PCM, decode the PCM back and compare. That is the whole
physical layer minus the Z80 and the speaker, and it is deterministic, which
matters more than realism when the thing being tuned is a decoder.
"""
import os
import sys
import wave

sys.path.insert(0, os.path.join(os.path.dirname(__file__), "..", "tools"))

from alink import frame, phy  # noqa: E402

checks = 0


def check(cond, what):
    global checks
    if not cond:
        raise AssertionError(what)
    checks += 1


def as_aofile(pcm16):
    """Convert signed-16 PCM to the ZEsarUX --aofile representation.

    That dump is unsigned 8-bit mono, and ZEsarUX emits the MIC bit as a small
    swing around a large constant level -- which is exactly the case the
    decoder's DC tracking exists for.
    """
    return bytes(
        (((int.from_bytes(pcm16[i:i + 2], "little", signed=True) >> 10) + 0xC0)
         & 0xFF)
        for i in range(0, len(pcm16), 2)
    )


def test_pulse_shape():
    pulses = phy.encode_block(bytes([0x00]))
    check(pulses[:phy.PREAMBLE_PULSES] == [phy.PILOT_T] * phy.PREAMBLE_PULSES,
          "preamble is PREAMBLE_PULSES pilot pulses")
    check(pulses[phy.PREAMBLE_PULSES] == phy.SYNC1_T, "first sync pulse")
    check(pulses[phy.PREAMBLE_PULSES + 1] == phy.SYNC2_T, "second sync pulse")
    # The last pulse is the closing edge, not data -- see encode_block().
    check(pulses[-1] == phy.TAIL_T, "a closing pulse ends the transmission")
    body = pulses[phy.PREAMBLE_PULSES + 2:-1]
    check(len(body) == 16, "one byte is 8 bits x 2 half-pulses")
    check(all(w == phy.ZERO_T for w in body), "0x00 is all zero-width pulses")

    body = phy.encode_block(bytes([0xFF]))[phy.PREAMBLE_PULSES + 2:-1]
    check(all(w == phy.ONE_T for w in body), "0xFF is all one-width pulses")


def test_msb_first():
    body = phy.encode_block(bytes([0x80]))[phy.PREAMBLE_PULSES + 2:-1]
    check(body[0] == phy.ONE_T and body[1] == phy.ONE_T,
          "the most significant bit goes first")
    check(body[2] == phy.ZERO_T, "and the next bit is a zero")


def test_leadout_is_appended():
    plain = phy.encode_block(b"\x00")
    padded = phy.encode_block(b"\x00", leadout=phy.LEADOUT_BYTES)
    check(len(padded) - len(plain) == phy.LEADOUT_BYTES * 16,
          "leadout adds LEADOUT_BYTES zero bytes of pulses")


def test_round_trip_every_frame_length():
    for length in (0, 1, 2, 63, 64):
        payload = bytes((i * 5 + 3) & 0xFF for i in range(length))
        block = frame.encode(frame.LINK, 0, 1, payload)
        pcm = phy.pulses_to_pcm(phy.encode_block(block))
        got = phy.decode_raw(pcm, unsigned8=False)
        check(got == [block],
              f"len={length} round-trips, got {len(got)} block(s)")


def test_round_trip_unsigned8():
    block = frame.encode(frame.LINK, 1, 0, b"hello")
    dump = as_aofile(phy.pulses_to_pcm(phy.encode_block(block)))
    check(phy.decode_raw(dump, unsigned8=True) == [block],
          "an unsigned-8 dump with a DC offset decodes")


def test_truncated_tail_is_absorbed_by_leadout():
    """Capture cuts 8-16 bytes off the end of a transmission.

    With length-driven termination the block is complete long before the
    leadout even starts, so losing the whole leadout changes nothing. The
    leadout exists so that what capture eats is padding rather than CRC.
    """
    block = frame.encode(frame.LINK, 0, 0, b"x" * 40)
    pcm = phy.pulses_to_pcm(phy.encode_block(block,
                                             leadout=phy.LEADOUT_BYTES))
    # The leadout is LEADOUT_BYTES x 8 bits x 2 half-pulses of ZERO_T.
    leadout_t = phy.LEADOUT_BYTES * 8 * 2 * phy.ZERO_T
    leadout_samples = round(leadout_t * phy.SAMPLE_RATE / phy.ZX_CLOCK_HZ)
    check(leadout_samples > 5000, "the leadout is a meaningful amount of audio")

    for cut in (leadout_samples // 2, leadout_samples):
        got = phy.decode_raw(pcm[:-cut * 2], unsigned8=False)
        check(got == [block],
              f"frame survives losing {cut} samples of tail, got {got!r}")


def test_back_to_back_blocks():
    """Two blocks with no gap between them.

    The proof-of-concept decoder ended a block on silence, so this case would
    never separate. Length-driven termination handles it, and it is the same
    rule the Z80 receiver uses.
    """
    a = frame.encode(frame.LINK, 0, 1, b"one")
    b = frame.encode(frame.LINK, 1, 1, b"two")
    pcm = phy.pulses_to_pcm(phy.encode_block(a) + phy.encode_block(b))
    check(phy.decode_raw(pcm, unsigned8=False) == [a, b],
          "two blocks in one stream both decode")


def test_noise_alone_yields_nothing():
    quiet = bytes(phy.SAMPLE_RATE * 2 * 2)          # two seconds of silence
    check(phy.decode_raw(quiet, unsigned8=False) == [],
          "silence yields nothing")

    preamble_only = phy.pulses_to_pcm([phy.PILOT_T] * phy.PREAMBLE_PULSES)
    check(phy.decode_raw(preamble_only, unsigned8=False) == [],
          "a preamble with no frame behind it yields nothing")

    sync_only = phy.pulses_to_pcm([phy.PILOT_T] * phy.PREAMBLE_PULSES
                                  + [phy.SYNC1_T, phy.SYNC2_T])
    check(phy.decode_raw(sync_only, unsigned8=False) == [],
          "a preamble and sync with no data yields nothing")


def test_impossible_length_is_rejected():
    """LEN above the absolute cap cannot be a legal frame, so the decoder
    abandons the block rather than waiting for bytes that will never come."""
    pcm = phy.pulses_to_pcm(phy.encode_block(bytes([0x40, 200] + [0] * 8)))
    check(phy.decode_raw(pcm, unsigned8=False) == [],
          "LEN=200 is abandoned, not accumulated")


def test_wav_round_trip():
    path = "/tmp/alink_phy_test.wav"
    block = frame.encode(frame.HELLO, 0, 0,
                         bytes([frame.VERSION, frame.CAPS, 64]))
    phy.write_wav(path, block)
    with wave.open(path, "rb") as w:
        check(w.getnchannels() == 1, "mono")
        check(w.getsampwidth() == 2, "16-bit")
        check(w.getframerate() == phy.SAMPLE_RATE, "48 kHz")
        pcm = w.readframes(w.getnframes())
    check(phy.decode_raw(pcm, unsigned8=False) == [block],
          "the WAV decodes back to the same block")
    os.unlink(path)


def test_air_time_matches_the_design():
    """The design spec's timing table is derived from these numbers, so a
    change here must be a deliberate one."""
    def seconds(pulses):
        return sum(pulses) / phy.ZX_CLOCK_HZ

    poll = seconds(phy.encode_block(frame.encode(frame.LINK, 0, 1)))
    full = seconds(phy.encode_block(frame.encode(frame.LINK, 0, 1,
                                                 b"x" * 64)))
    check(0.050 < poll < 0.056, f"an empty poll is ~0.053 s, got {poll:.4f}")
    check(0.42 < full < 0.44, f"a full frame is ~0.429 s, got {full:.4f}")


def main():
    test_pulse_shape()
    test_msb_first()
    test_leadout_is_appended()
    test_round_trip_every_frame_length()
    test_round_trip_unsigned8()
    test_truncated_tail_is_absorbed_by_leadout()
    test_back_to_back_blocks()
    test_noise_alone_yields_nothing()
    test_impossible_length_is_rejected()
    test_wav_round_trip()
    test_air_time_matches_the_design()
    print(f"alink_phy_py: {checks} checks passed")
    return 0


if __name__ == "__main__":
    sys.exit(main())
