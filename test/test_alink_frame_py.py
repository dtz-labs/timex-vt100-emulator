#!/usr/bin/env python3
"""Host tests for the Python side of the protocol v1 frame codec.

Standalone script, no pytest -- matches test/test_check_image_limit.py, which
is how this repo runs its Python tests.

The point of these tests is not that the codec is self-consistent. It is that
it agrees with src/alink_frame.c. test_alink_xcheck.py proves that end to end
against the compiled C slave; this file pins the wire bytes so a divergence
shows up here first, with a readable diff.
"""
import os
import sys

sys.path.insert(0, os.path.join(os.path.dirname(__file__), "..", "tools"))

from alink import frame  # noqa: E402

checks = 0


def check(cond, what):
    global checks
    if not cond:
        raise AssertionError(what)
    checks += 1


def test_crc_published_vector():
    check(frame.crc16(b"123456789") == 0x29B1, "CCITT-FALSE check value")
    check(frame.crc16(b"") == 0xFFFF, "empty input returns the init value")


def test_encode_layout():
    block = frame.encode(frame.LINK, seq=1, ack=1, payload=b"hi")
    check(len(block) == 6, "4 + payload")
    check(block[0] == (frame.LINK << 5) | 0x02 | 0x01, "CTRL bit layout")
    check(block[1] == 2, "LEN")
    check(block[2:4] == b"hi", "payload")
    crc = frame.crc16(block[:4])
    check(block[4] == (crc >> 8) & 0xFF, "CRC high byte first")
    check(block[5] == crc & 0xFF, "CRC low byte second")


def test_encode_rejects_oversize():
    try:
        frame.encode(frame.LINK, 0, 0, b"x" * (frame.MAX_PAYLOAD + 1))
    except ValueError:
        check(True, "oversize payload raises")
        return
    raise AssertionError("oversize payload must raise")


def test_round_trip_all_lengths():
    for length in range(0, frame.MAX_PAYLOAD + 1):
        payload = bytes((i * 7 + 1) & 0xFF for i in range(length))
        block = frame.encode(frame.LINK, seq=length & 1,
                             ack=(length >> 1) & 1, payload=payload)
        check(len(block) == length + 4, f"block length at len={length}")
        f = frame.decode(block)
        check(f is not None, f"decodes at len={length}")
        check(f.type == frame.LINK, f"type at len={length}")
        check(f.seq == (length & 1), f"seq at len={length}")
        check(f.ack == ((length >> 1) & 1), f"ack at len={length}")
        check(f.payload == payload, f"payload at len={length}")
        check(f.len == length, f"len property at len={length}")


def test_reject_length_mismatch():
    block = frame.encode(frame.LINK, 0, 0, b"abcd")
    check(frame.decode(block) is not None, "the intact block decodes")
    check(frame.decode(block[:-1]) is None, "one byte short")
    check(frame.decode(block + b"\x00") is None, "one byte long")
    check(frame.decode(b"\x00\x00\x00") is None, "shorter than a header")


def test_reject_bit_flips():
    good = frame.encode(frame.LINK, 0, 0, b"abc")
    for i in range(len(good)):
        for bit in range(8):
            bad = bytearray(good)
            bad[i] ^= 1 << bit
            check(frame.decode(bytes(bad)) is None,
                  f"flip byte {i} bit {bit} must be rejected")


def test_reject_oversize_and_over_advertised():
    block = frame.encode(frame.LINK, 0, 0, b"x" * 40)
    check(frame.decode(block, max_payload=frame.MAX_PAYLOAD) is not None,
          "40 bytes is under the absolute cap")
    check(frame.decode(block, max_payload=32) is None, "over advertised")
    check(frame.decode(block, max_payload=40) is not None, "exactly at it")

    hello = frame.encode(frame.HELLO, 0, 0,
                         bytes([frame.VERSION, frame.CAPS, 64]) + b"x" * 37)
    check(frame.decode(hello, max_payload=8) is not None,
          "the advertised cap applies to LINK frames only")


def test_hello_minimum_length():
    for length in range(0, frame.HELLO_PAYLOAD):
        for type_ in (frame.HELLO, frame.WELCOME):
            block = frame.encode(type_, 0, 0, b"\x00" * length)
            check(frame.decode(block) is None,
                  f"type {type_} with LEN={length} must be rejected")

    block = frame.encode(frame.HELLO, 0, 0,
                         bytes([frame.VERSION, frame.CAPS, 64, 0xDE, 0xAD]))
    f = frame.decode(block)
    check(f is not None, "LEN>3 HELLO decodes")
    check(f.payload[0] == frame.VERSION, "version at offset 0")
    check(f.payload[2] == 64, "max payload at offset 2")
    check(f.len == 5, "extra bytes are kept, just ignored by negotiation")


def test_reject_reserved_types():
    for type_ in range(4, 8):
        block = frame.encode(type_, 0, 0, b"")
        check(frame.decode(block) is None, f"type {type_} is reserved")


def test_reserved_ctrl_bits_ignored():
    body = bytes([(frame.LINK << 5) | 0x1C | 0x02, 0])
    block = body + frame.crc16(body).to_bytes(2, "big")
    f = frame.decode(block)
    check(f is not None, "reserved CTRL bits do not invalidate a frame")
    check(f.type == frame.LINK, "type survives")
    check(f.seq == 1 and f.ack == 0, "seq/ack survive")


def main():
    test_crc_published_vector()
    test_encode_layout()
    test_encode_rejects_oversize()
    test_round_trip_all_lengths()
    test_reject_length_mismatch()
    test_reject_bit_flips()
    test_reject_oversize_and_over_advertised()
    test_hello_minimum_length()
    test_reject_reserved_types()
    test_reserved_ctrl_bits_ignored()
    print(f"alink_frame_py: {checks} checks passed")
    return 0


if __name__ == "__main__":
    sys.exit(main())
