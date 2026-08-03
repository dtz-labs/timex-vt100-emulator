#!/usr/bin/env python3
"""Host tests for the PC-side runner's plumbing.

No audio, no emulator, no pty. What is tested here is the part that fails
silently in the field: following a growing dump across reads, surviving the
emulator being restarted under us, and driving a real Master through a real
AudioChannel.
"""
import os
import sys

sys.path.insert(0, os.path.join(os.path.dirname(__file__), "..", "tools"))

from alink import frame, phy  # noqa: E402
from alink.main import AofileReader, AudioChannel  # noqa: E402
from alink.master import Master  # noqa: E402

checks = 0
DUMP = "/tmp/alink_runner_test.raw"


def check(cond, what):
    global checks
    if not cond:
        raise AssertionError(what)
    checks += 1


def raw_for(blocks):
    pulses = []
    for b in blocks:
        pulses.extend(phy.encode_block(b))
        pulses.append(phy.PILOT_T * 8)
    return phy.pulses_to_raw_u8(pulses)


def test_reader_across_split_reads():
    """A frame takes ~0.43 s on the wire and we poll far faster, so a frame
    arriving split across two reads is the normal case, not an edge case."""
    blocks = [frame.encode(frame.LINK, 0, 1, b"first"),
              frame.encode(frame.LINK, 1, 1, b"second")]
    data = raw_for(blocks)
    reader = AofileReader(DUMP)

    got = []
    step = len(data) // 7
    for i in range(0, len(data), step):
        mode = "wb" if i == 0 else "ab"
        with open(DUMP, mode) as f:
            f.write(data[i:i + step])
        got += reader.poll()
    check(got == blocks, f"both blocks recovered across 7 reads, got {got!r}")
    os.unlink(DUMP)


def test_reader_ignores_nothing_new():
    data = raw_for([frame.encode(frame.LINK, 0, 1, b"x")])
    with open(DUMP, "wb") as f:
        f.write(data)
    reader = AofileReader(DUMP)
    check(len(reader.poll()) == 1, "the block is read once")
    check(reader.poll() == [], "polling again with no new bytes yields nothing")
    os.unlink(DUMP)


def test_reader_survives_a_restarted_emulator():
    """ZEsarUX recreates the dump on restart. A reader that kept its old
    offset would either read nothing forever or decode the tail of a file
    that no longer exists."""
    first = frame.encode(frame.LINK, 0, 1, b"before")
    second = frame.encode(frame.LINK, 1, 1, b"after")
    with open(DUMP, "wb") as f:
        f.write(raw_for([first]))
    reader = AofileReader(DUMP)
    check(reader.poll() == [first], "the first dump decodes")

    with open(DUMP, "wb") as f:            # truncate: the emulator restarted
        f.write(raw_for([second]))
    check(reader.poll() == [second],
          "the reader restarts and decodes the new dump")
    os.unlink(DUMP)


def test_reader_missing_file_is_not_an_error():
    reader = AofileReader("/tmp/alink_runner_absent.raw")
    check(reader.poll() == [], "a dump that does not exist yet yields nothing")


class LoopSender:
    """Stands in for ffmpeg: hands the block straight to a model slave and
    appends its response to the dump the reader is following."""

    def __init__(self, path, slave):
        self.path = path
        self.slave = slave
        with open(path, "wb"):
            pass

    def play(self, pcm):
        blocks = phy.decode_raw(pcm, unsigned8=False)
        for block in blocks:
            reply = self.slave.handle(block)
            if reply is not None:
                with open(self.path, "ab") as f:
                    f.write(raw_for([reply]))


class ModelSlave:
    def __init__(self):
        self.reset()
        self.delivered = bytearray()

    def reset(self):
        self.state = "LISTEN"
        self.last_rx_seq = 1
        self.tx_seq = 0

    def handle(self, block):
        f = frame.decode(block)
        if f is None:
            return None
        if f.type == frame.HELLO:
            self.reset()
            self.state = "LINKED"
            return frame.encode(frame.WELCOME, 0, 0,
                                bytes([frame.VERSION, frame.CAPS, 64]))
        if self.state != "LINKED" or f.type != frame.LINK:
            return None
        if f.len > 0 and f.seq != self.last_rx_seq:
            self.last_rx_seq = f.seq
            self.delivered.extend(f.payload)
        return frame.encode(frame.LINK, self.tx_seq, self.last_rx_seq, b"")


def test_master_over_a_real_audio_channel():
    """The Master, the AudioChannel and the AofileReader together, with only
    ffmpeg replaced. Every byte still goes through the pulse codec."""
    slave = ModelSlave()
    reader = AofileReader(DUMP)
    channel = AudioChannel(LoopSender(DUMP, slave), reader)
    master = Master(channel, t_resp=1.0)

    check(master.transact() is True, "the handshake completes over audio")
    check(master.state == "LINKED", "the master is LINKED")

    master.send(b"hello over audio")
    master.transact()
    check(bytes(slave.delivered) == b"hello over audio",
          f"the payload arrived, got {bytes(slave.delivered)!r}")
    os.unlink(DUMP)


def main():
    test_reader_across_split_reads()
    test_reader_ignores_nothing_new()
    test_reader_survives_a_restarted_emulator()
    test_reader_missing_file_is_not_an_error()
    test_master_over_a_real_audio_channel()
    print(f"alink_runner: {checks} checks passed")
    return 0


if __name__ == "__main__":
    sys.exit(main())
