"""Protocol v1 master: stop-and-wait ARQ with piggybacked upstream data.

The master owns every exchange. It is transport-agnostic: give it any object
with send(block) and recv(timeout) and it will run the protocol over it. That
is what lets the same class run against a simulated lossy channel in tests, a
pipe to the compiled C slave in the cross-check, and real audio in PR B.

Master rules, from the protocol v1 spec:

1. Send a frame; wait up to T_RESP for a response, measured from the end of
   our own transmission.
2. ANY valid response completes the transaction and resets the retry counter.
   A transaction fails only on timeout or an invalid block -- the ACK bit is
   never a failure condition.
3. If a downstream payload is outstanding and the response's ACK equals its
   SEQ, it is delivered: advance SEQ. Otherwise it stays outstanding and is
   retransmitted as the next transaction's frame.
4. If the response is a LINK with LEN>0 and a SEQ differing from the last
   accepted upstream SEQ, accept the payload and update the ACK we send.
5. On failure, resend the identical frame; at most RETRIES attempts in total,
   the original counting as the first. Exhausting them declares the link DOWN.
6. Unacknowledged in-flight payloads are dropped on any session reset and must
   never be resent into a new session.
"""
from . import frame


class Channel:
    """The transport interface a Master needs.

    Implementations must be half-duplex: send() returns once the block is fully
    on the wire, and recv() waits up to `timeout` seconds for one whole block.
    """

    def send(self, block):
        raise NotImplementedError

    def recv(self, timeout):
        """Return one complete block, or None on timeout."""
        raise NotImplementedError


class Master:
    def __init__(self, channel, *, t_resp=10.0, retries=3,
                 t_hello_retry=2.0, max_payload=frame.MAX_PAYLOAD):
        self.channel = channel
        self.t_resp = t_resp
        self.retries = retries
        self.t_hello_retry = t_hello_retry
        self.max_payload = max_payload

        self.state = "DOWN"
        self.received = bytearray()      # upstream bytes, for the pty
        self._outbox = bytearray()       # downstream bytes, from the pty
        self._reset_session()

    # -- session state ---------------------------------------------------

    def _reset_session(self):
        """Rule 6: in-flight payloads are dropped on any session reset and
        must never be resent into a new session."""
        self.tx_seq = 0
        self.tx_payload = None           # the outstanding downstream payload
        self.last_rx_seq = 1             # so the first SEQ=0 counts as new
        self.peer_max_payload = frame.MAX_PAYLOAD

    # -- public API ------------------------------------------------------

    def send(self, data):
        """Queue downstream bytes. Chunking happens at transmission time."""
        self._outbox.extend(data)

    def close(self):
        """Best-effort courtesy close. BYE requires no reply."""
        if self.state == "LINKED":
            try:
                self.channel.send(frame.encode(frame.BYE, 0, 0))
            except OSError:
                pass
        self.state = "DOWN"

    def transact(self):
        """Run exactly one exchange. Returns True if it completed."""
        if self.state != "LINKED":
            return self._handshake()
        return self._link_transaction()

    # -- internals -------------------------------------------------------

    def _exchange(self, block):
        """Send a block and wait for a valid response.

        Rule 2: ANY valid response completes the transaction. Rule 5: on
        failure resend the identical block, at most `retries` attempts in
        total including the first.
        """
        for _ in range(self.retries):
            self.channel.send(block)
            reply = self.channel.recv(self.t_resp)
            if reply is None:
                continue
            f = frame.decode(reply, self.max_payload)
            if f is not None:
                return f
        return None

    def _handshake(self):
        offer = bytes([frame.VERSION, frame.CAPS, frame.MAX_PAYLOAD])
        f = self._exchange(frame.encode(frame.HELLO, 0, 0, offer))
        if f is None or f.type != frame.WELCOME:
            return False

        version = f.payload[0]
        peer_max = f.payload[2]
        if version == 0 or not 1 <= peer_max <= frame.MAX_PAYLOAD:
            return False                 # an invalid WELCOME is no WELCOME

        self._reset_session()
        self.peer_max_payload = peer_max
        self.state = "LINKED"
        return True

    def _link_transaction(self):
        # Rule 3: an outstanding payload is retransmitted unchanged until the
        # slave acknowledges it, so only pull new bytes when nothing is in
        # flight.
        if self.tx_payload is None and self._outbox:
            take = min(len(self._outbox), self.peer_max_payload)
            self.tx_payload = bytes(self._outbox[:take])
            del self._outbox[:take]

        payload = self.tx_payload if self.tx_payload is not None else b""
        block = frame.encode(frame.LINK, self.tx_seq, self.last_rx_seq, payload)

        f = self._exchange(block)
        if f is None:
            self.state = "DOWN"          # rule 5: retries exhausted
            self._reset_session()
            return False

        if f.type != frame.LINK:
            return True                  # valid, so the transaction completed

        # Rule 3: delivery is confirmed by the ACK matching our SEQ.
        if self.tx_payload is not None and f.ack == self.tx_seq:
            self.tx_payload = None
            self.tx_seq ^= 1

        # Rule 4: accept upstream only from LEN>0 with a new SEQ. The LEN>0
        # test is also what implements "receivers ignore the SEQ bit of LEN=0
        # frames".
        if f.len > 0 and f.seq != self.last_rx_seq:
            self.last_rx_seq = f.seq
            self.received.extend(f.payload)

        return True
