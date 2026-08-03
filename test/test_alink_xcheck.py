#!/usr/bin/env python3
"""Cross-implementation check: the Python master against the REAL C slave.

The other four suites each test one implementation against its own reading of
the spec. Two implementations can both be self-consistent and still disagree --
on CRC byte order, on whether an empty poll advances SEQ, on what a duplicate
does. This is the only test that catches that, and it must pass before any
audio work starts: after PR B a failure here would be indistinguishable from a
signal problem.

The C slave echoes every downstream payload back as upstream data, so a single
assertion covers both directions at once.
"""
import os
import subprocess
import sys

sys.path.insert(0, os.path.join(os.path.dirname(__file__), "..", "tools"))

from alink import frame  # noqa: E402
from alink.master import Master  # noqa: E402

ROOT = os.path.abspath(os.path.join(os.path.dirname(__file__), ".."))
BUILD = os.path.join(ROOT, "build", "host")
BINARY = os.path.join(BUILD, "alink_xcheck_slave")

checks = 0


def check(cond, what):
    global checks
    if not cond:
        raise AssertionError(what)
    checks += 1


def build_slave():
    os.makedirs(BUILD, exist_ok=True)
    cc = os.environ.get("CC", "cc")
    subprocess.run(
        [cc, "-std=c99", "-Wall", "-Wextra", "-Werror",
         "-I", os.path.join(ROOT, "include"),
         os.path.join(ROOT, "tools", "alink", "xcheck_slave.c"),
         os.path.join(ROOT, "src", "alink_slave.c"),
         os.path.join(ROOT, "src", "alink_frame.c"),
         "-o", BINARY],
        check=True,
    )


class PipeChannel:
    """Carries blocks to the C slave over its stdin/stdout, length-prefixed."""

    def __init__(self, proc):
        self.proc = proc
        self._pending = None

    def send(self, block):
        self.proc.stdin.write(bytes([len(block)]) + block)
        self.proc.stdin.flush()
        n = self.proc.stdout.read(1)
        if not n:
            raise OSError("slave exited")
        n = n[0]
        self._pending = self.proc.stdout.read(n) if n else None

    def recv(self, timeout):
        resp, self._pending = self._pending, None
        return resp

    def queue_upstream(self, data):
        self.proc.stdin.write(bytes([0xFF, len(data)]) + data)
        self.proc.stdin.flush()


def with_slave(fn):
    """Give `fn` a Master wired to a fresh C slave over one shared channel."""
    proc = subprocess.Popen([BINARY], stdin=subprocess.PIPE,
                            stdout=subprocess.PIPE)
    try:
        ch = PipeChannel(proc)
        fn(Master(ch), ch)
    finally:
        proc.stdin.close()
        proc.wait(timeout=5)


def test_handshake(m, ch):
    check(m.transact() is True, "HELLO/WELCOME completes against the C slave")
    check(m.state == "LINKED", "master reaches LINKED")


def test_echo_round_trip(m, ch):
    m.transact()
    m.send(b"hello")
    m.transact()                     # slave receives and queues the echo
    m.transact()                     # slave sends it back
    check(bytes(m.received) == b"hello",
          f"echo returned intact, got {bytes(m.received)!r}")


def test_empty_polls_do_not_disturb_sequence(m, ch):
    """If the two sides disagreed about whether an empty poll advances SEQ,
    the payload after these ten polls would be discarded as a duplicate."""
    m.transact()
    for _ in range(10):
        check(m.transact() is True, "idle poll completes against the C slave")
    m.send(b"z")
    m.transact()
    m.transact()
    check(bytes(m.received) == b"z",
          f"a payload after ten empty polls still arrives, got "
          f"{bytes(m.received)!r}")


def test_duplicate_is_answered_but_not_redelivered(m, ch):
    """A repeated SEQ must be answered and not delivered again.

    The master alone never exercises this over a lossless pipe -- it only
    retransmits after a loss -- so the frames go onto the wire directly. This
    gap was found by mutation-testing: deleting the SEQ check from the C slave
    left every other cross-check passing.

    The C harness echoes each DELIVERED payload into its upstream slot, so the
    second exchange distinguishes the two behaviours: the frame below
    acknowledges the first echo, freeing the slot, so a slave that redelivered
    would have "bbb" to echo, while a correct one has nothing.
    """
    m.transact()                                   # handshake

    ch.send(frame.encode(frame.LINK, 0, 1, b"aaa"))
    r = frame.decode(ch.recv(0))
    check(r is not None and r.payload == b"aaa",
          f"the first payload is echoed back, got {r!r}")

    ch.send(frame.encode(frame.LINK, 0, 0, b"bbb"))   # same SEQ, acks the echo
    r = frame.decode(ch.recv(0))
    check(r is not None, "the duplicate is still answered")
    check(r.len == 0,
          f"but is not delivered again -- the slave echoed {r.payload!r}")


def test_all_payload_lengths(m, ch):
    m.transact()
    for length in (1, 2, 63, 64):
        m.received.clear()
        payload = bytes((i * 3 + 1) & 0xFF for i in range(length))
        m.send(payload)
        for _ in range(4):
            m.transact()
        check(bytes(m.received) == payload,
              f"length {length} round-trips, got {len(m.received)} bytes")


def test_upstream_from_the_keyboard_slot(m, ch):
    m.transact()
    ch.queue_upstream(b"ls\r")
    m.transact()
    check(bytes(m.received) == b"ls\r",
          f"queued upstream data arrives, got {bytes(m.received)!r}")
    for _ in range(3):
        m.transact()
    check(bytes(m.received) == b"ls\r", "and exactly once")


def test_hello_resets_mid_session(m, ch):
    m.transact()
    m.send(b"abc")
    m.transact()
    m.transact()
    check(bytes(m.received) == b"abc", "first session carried data")

    m.state = "DOWN"                 # force a fresh handshake
    m._reset_session()
    check(m.transact() is True, "the C slave answers a mid-session HELLO")
    check(m.state == "LINKED", "and the session is LINKED again")

    m.received.clear()
    m.send(b"q")
    m.transact()
    m.transact()
    check(bytes(m.received) == b"q", "the reset session carries data again")


def test_bye_returns_the_slave_to_listen(m, ch):
    m.transact()
    ch.send(frame.encode(frame.BYE, 0, 0))
    check(ch.recv(0) is None, "BYE draws no reply")

    ch.send(frame.encode(frame.LINK, 0, 1, b"ignored"))
    check(ch.recv(0) is None, "a LISTEN slave ignores LINK")

    check(m._handshake() is True, "a fresh HELLO relinks it")


def main():
    build_slave()
    for fn in (test_handshake,
               test_echo_round_trip,
               test_empty_polls_do_not_disturb_sequence,
               test_duplicate_is_answered_but_not_redelivered,
               test_all_payload_lengths,
               test_upstream_from_the_keyboard_slot,
               test_hello_resets_mid_session,
               test_bye_returns_the_slave_to_listen):
        with_slave(fn)
    print(f"alink_xcheck: {checks} checks passed")
    return 0


if __name__ == "__main__":
    sys.exit(main())
