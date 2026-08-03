#!/usr/bin/env python3
"""Host tests for the protocol v1 master, over a simulated channel.

The channel can drop and corrupt blocks on demand, so every ARQ rule is
exercised without any audio. The slave on the far side is a minimal in-process
model -- the REAL C slave is exercised in test_alink_xcheck.py.
"""
import os
import sys

sys.path.insert(0, os.path.join(os.path.dirname(__file__), "..", "tools"))

from alink import frame  # noqa: E402
from alink.master import Master  # noqa: E402

checks = 0


def check(cond, what):
    global checks
    if not cond:
        raise AssertionError(what)
    checks += 1


class ModelSlave:
    """A minimal protocol-conformant slave, used only to drive the master."""

    def __init__(self):
        self.reset()
        self.delivered = bytearray()

    def reset(self):
        self.state = "LISTEN"
        self.last_rx_seq = 1
        self.tx_seq = 0
        self.tx_payload = b""
        self.tx_pending = False

    def queue(self, data):
        if not self.tx_pending and self.state == "LINKED":
            self.tx_payload = data[:frame.MAX_PAYLOAD]
            self.tx_pending = bool(self.tx_payload)

    def handle(self, block):
        f = frame.decode(block)
        if f is None:
            return None
        if f.type == frame.HELLO:
            self.reset()
            self.state = "LINKED"
            return frame.encode(frame.WELCOME, 0, 0,
                                bytes([frame.VERSION, frame.CAPS, 64]))
        if self.state != "LINKED":
            return None
        if f.type == frame.BYE:
            self.reset()
            return None
        if f.type != frame.LINK:
            return None
        if self.tx_pending and f.ack == self.tx_seq:
            self.tx_pending = False
            self.tx_payload = b""
            self.tx_seq ^= 1
        if f.len > 0 and f.seq != self.last_rx_seq:
            self.last_rx_seq = f.seq
            self.delivered.extend(f.payload)
        return frame.encode(frame.LINK, self.tx_seq, self.last_rx_seq,
                            self.tx_payload if self.tx_pending else b"")


class FakeChannel:
    """Carries blocks to a ModelSlave, with scriptable faults."""

    def __init__(self, slave):
        self.slave = slave
        self.drop_next = 0        # drop this many upcoming responses
        self.corrupt_next = 0     # corrupt this many upcoming responses
        self.sent = []
        self._pending = None

    def send(self, block):
        self.sent.append(block)
        resp = self.slave.handle(block)
        if resp is not None and self.drop_next > 0:
            self.drop_next -= 1
            resp = None
        elif resp is not None and self.corrupt_next > 0:
            self.corrupt_next -= 1
            bad = bytearray(resp)
            bad[0] ^= 0xFF
            resp = bytes(bad)
        self._pending = resp

    def recv(self, timeout):
        resp, self._pending = self._pending, None
        return resp


def linked_pair():
    slave = ModelSlave()
    ch = FakeChannel(slave)
    m = Master(ch)
    check(m.transact() is True, "HELLO/WELCOME completes")
    check(m.state == "LINKED", "master is LINKED after WELCOME")
    return m, ch, slave


def test_handshake():
    m, ch, slave = linked_pair()
    check(slave.state == "LINKED", "slave is LINKED")
    check(len(ch.sent) == 1, "one frame sent for the handshake")
    check(frame.decode(ch.sent[0]).type == frame.HELLO, "it was a HELLO")


def test_downstream_delivery_in_order():
    m, ch, slave = linked_pair()
    m.send(b"hello world")
    m.transact()
    check(bytes(slave.delivered) == b"hello world", "payload delivered once")
    m.send(b"!")
    m.transact()
    check(bytes(slave.delivered) == b"hello world!", "second payload appended")


def test_downstream_chunked_at_max_payload():
    m, ch, slave = linked_pair()
    m.send(b"x" * 150)
    for _ in range(5):
        m.transact()
    check(bytes(slave.delivered) == b"x" * 150, "all 150 bytes arrive")
    check(all(frame.decode(b).len <= frame.MAX_PAYLOAD for b in ch.sent),
          "no frame exceeded the cap")


def test_lost_response_retransmits_without_duplicating():
    m, ch, slave = linked_pair()
    m.send(b"abc")
    ch.drop_next = 1
    m.transact()                       # response dropped -> retry inside
    check(bytes(slave.delivered) == b"abc",
          "the slave saw it once despite the retransmit")
    m.send(b"de")
    m.transact()
    check(bytes(slave.delivered) == b"abcde", "stream stays in order")


def test_corrupt_response_is_treated_as_lost():
    m, ch, slave = linked_pair()
    m.send(b"abc")
    ch.corrupt_next = 1
    m.transact()
    check(bytes(slave.delivered) == b"abc", "delivered exactly once")


def test_retry_exhaustion_declares_down():
    m, ch, slave = linked_pair()
    m.send(b"abc")
    ch.drop_next = 99
    check(m.transact() is False, "transaction fails")
    check(m.state == "DOWN", "link declared DOWN after RETRIES")


def test_upstream_keystrokes_arrive_once():
    m, ch, slave = linked_pair()
    slave.queue(b"ls\r")
    m.transact()
    check(bytes(m.received) == b"ls\r", "keystrokes arrive")
    m.transact()
    m.transact()
    check(bytes(m.received) == b"ls\r", "and are not delivered again")


def test_upstream_survives_retransmit_storm_unduplicated():
    """A keyboard chunk pending during a retransmit storm must survive
    un-duplicated -- named explicitly as a regression case by the protocol
    spec's testing section."""
    m, ch, slave = linked_pair()
    slave.queue(b"KEY")
    ch.drop_next = 2
    m.transact()
    check(bytes(m.received) == b"KEY", "arrives despite two lost responses")
    for _ in range(3):
        m.transact()
    check(bytes(m.received) == b"KEY", "exactly once")


def test_idle_polling_never_flaps():
    """An idle link polled forever must never flap -- empty polls complete
    transactions regardless of ACK bits."""
    m, ch, slave = linked_pair()
    for _ in range(50):
        check(m.transact() is True, "idle poll completes")
    check(m.state == "LINKED", "still LINKED after 50 idle polls")
    check(len(m.received) == 0, "no phantom upstream data")


def test_welcome_payload_never_reaches_the_stream():
    """The WELCOME payload must never enter the byte stream."""
    m, ch, slave = linked_pair()
    check(len(m.received) == 0, "handshake produced no stream bytes")
    m.transact()
    check(len(m.received) == 0, "and still none after a poll")


def test_reconnect_after_down_drops_inflight():
    """Rule 6: unacknowledged in-flight payloads are dropped on a session
    reset and must never be resent into a new session.

    This matters precisely because a session reset also resets the slave's
    'last accepted' back to 1. A payload resent with SEQ=0 into the new
    session would look new and be delivered a second time.
    """
    m, ch, slave = linked_pair()
    m.send(b"lost forever")
    ch.drop_next = 99
    m.transact()
    check(m.state == "DOWN", "went DOWN")

    # The channel drops responses, not requests, so the slave did see the
    # frame -- and exactly once, despite three attempts.
    check(bytes(slave.delivered) == b"lost forever",
          "duplicate suppression held across the retransmit storm")

    ch.drop_next = 0
    check(m.transact() is True, "HELLO retry reconnects")
    check(m.state == "LINKED", "LINKED again")

    before = bytes(slave.delivered)
    for _ in range(3):
        m.transact()
    check(bytes(slave.delivered) == before,
          "nothing was resent into the new session")


def test_invalid_welcome_is_no_welcome():
    """The master treats a WELCOME advertising version 0, or a max payload
    outside 1..64, as an invalid response."""
    for bad_payload in (bytes([0, 0, 64]), bytes([1, 0, 0]), bytes([1, 0, 65])):
        class BadSlave(ModelSlave):
            def handle(self, block):
                f = frame.decode(block)
                if f is not None and f.type == frame.HELLO:
                    return frame.encode(frame.WELCOME, 0, 0, bad_payload)
                return None

        m = Master(FakeChannel(BadSlave()))
        check(m.transact() is False, f"WELCOME {bad_payload!r} is rejected")
        check(m.state == "DOWN", f"stays DOWN after {bad_payload!r}")


def main():
    test_handshake()
    test_downstream_delivery_in_order()
    test_downstream_chunked_at_max_payload()
    test_lost_response_retransmits_without_duplicating()
    test_corrupt_response_is_treated_as_lost()
    test_retry_exhaustion_declares_down()
    test_upstream_keystrokes_arrive_once()
    test_upstream_survives_retransmit_storm_unduplicated()
    test_idle_polling_never_flaps()
    test_welcome_payload_never_reaches_the_stream()
    test_reconnect_after_down_drops_inflight()
    test_invalid_welcome_is_no_welcome()
    print(f"alink_master: {checks} checks passed")
    return 0


if __name__ == "__main__":
    sys.exit(main())
