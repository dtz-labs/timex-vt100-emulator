"""Pulse encoder and decoder for the audio link's physical layer.

Wire format (design decision D1, option C -- our own codec, no ROM routines):

    [48 x pilot 2168 T] [sync 667 T] [sync 735 T] [data...] [leadout...]

Data is MSB first, one bit per pair of equal half-pulses: 855 T for a 0,
1710 T for a 1 -- the ROM's own bit timings, which is what keeps the Z80 side
a simple edge-timing loop rather than a cycle-counted turbo decoder.

The preamble is 48 pulses (29.7 ms) rather than the ROM's 3,223 (1.99 s). We
can afford that because we decode it ourselves: the ROM's LD-BYTES deliberately
ignores its first second of pilot and then needs 512 more pulses to lock, which
is why option A in the design spec was unusable.

Frames are variable length. Once LEN is decoded the receiver knows exactly how
many bytes remain, so a block ends after LEN + 4 bytes rather than after
silence. That differs from the proof-of-concept decoder this is adapted from
(dtz-labs/poc-zx-audio-link-from-zx-to-mac), which assumed a fixed 64-byte
payload and cut on a gap -- with that rule two back-to-back blocks would never
separate, because the second block's preamble follows the first immediately.
It is also the same termination condition src/alink_phy.c uses, so both
implementations end a frame on the same byte.

This module does not validate CRC. That is alink.frame.decode()'s job; this
one recovers blocks and hands them over.
"""
import wave

PILOT_T = 2168
SYNC1_T = 667
SYNC2_T = 735
ZERO_T = 855
ONE_T = 1710

#: Closing edge after the last half-pulse. Nothing ever measures its width --
#: it exists so that the preceding half-pulse HAS a measurable width.
TAIL_T = ZERO_T

PREAMBLE_PULSES = 48
LEADOUT_BYTES = 32

ZX_CLOCK_HZ = 3_500_000
SAMPLE_RATE = 48_000
AMPLITUDE = 24_000

#: Smallest block that can carry a length field, i.e. CTRL + LEN.
_HEADER_BYTES = 2
#: A frame is LEN + this many bytes: CTRL, LEN and the two CRC bytes.
_FRAME_OVERHEAD = 4
#: Absolute cap, mirroring ALINK_MAX_PAYLOAD.
_MAX_PAYLOAD = 64


# --------------------------------------------------------------------------
# Encoder
# --------------------------------------------------------------------------

def _byte_pulses(value):
    """Two equal half-pulses per bit, most significant bit first."""
    out = []
    for shift in range(7, -1, -1):
        width = ONE_T if value & (1 << shift) else ZERO_T
        out.append(width)
        out.append(width)
    return out


def encode_block(block, *, leadout=0):
    """Render one block as a list of pulse widths in T-states.

    `leadout` appends that many zero bytes after the block. Capture truncates
    the tail of a transmission by 8-16 bytes, so upstream transmissions pad
    with LEADOUT_BYTES to make sure what is lost is padding, never the CRC.

    Note the closing pulse. A receiver measures a half-pulse between two
    edges, so the final half-pulse of a transmission has no width until
    something ends it -- without a closing edge the last bit never completes
    and the frame is never delivered. This is a property of the wire format,
    binding on both sides: src/alink_phy.c must emit the same trailing edge
    after its last half-pulse. Its width is irrelevant (nothing measures it);
    only the edge matters.
    """
    pulses = [PILOT_T] * PREAMBLE_PULSES
    pulses.append(SYNC1_T)
    pulses.append(SYNC2_T)
    for value in block:
        pulses.extend(_byte_pulses(value))
    for _ in range(leadout):
        pulses.extend(_byte_pulses(0))
    pulses.append(TAIL_T)
    return pulses


def pulses_to_pcm(pulses, sample_rate=SAMPLE_RATE, amplitude=AMPLITUDE):
    """Render pulse widths to signed 16-bit little-endian mono PCM.

    Sample positions are accumulated as exact fractions and rounded only at
    the boundaries, so rounding error cannot drift over a long transmission.
    """
    out = bytearray()
    level = -amplitude
    position = 0.0
    for width in pulses:
        end = position + width * sample_rate / ZX_CLOCK_HZ
        count = max(1, round(end) - round(position))
        out.extend(level.to_bytes(2, "little", signed=True) * count)
        position = end
        level = -level
    return bytes(out)


#: ZEsarUX's --realtape reads a bare .raw file at exactly this rate, mono and
#: 8-bit unsigned, with no external tooling. Any other format (including WAV)
#: goes through the sox utility, which is a dependency worth not having: the
#: emulator prints "Unable to find sox program" and silently plays nothing.
REALTAPE_RATE = 44_100


def pulses_to_raw_u8(pulses, sample_rate=REALTAPE_RATE):
    """Render pulse widths to unsigned 8-bit mono, the --realtape raw format."""
    out = bytearray()
    high = True
    position = 0.0
    for width in pulses:
        end = position + width * sample_rate / ZX_CLOCK_HZ
        count = max(1, round(end) - round(position))
        out.extend(b"\xd0" if high else b"\x30")
        out.extend((b"\xd0" if high else b"\x30") * (count - 1))
        position = end
        high = not high
    return bytes(out)


def write_realtape(path, blocks, *, leadout=0, gap_pulses=8):
    """Write one or more blocks as a --realtape raw file.

    `gap_pulses` inserts a long idle stretch between blocks so a receiver that
    joins mid-stream has a clean preamble to lock onto rather than landing in
    the middle of one.
    """
    pulses = []
    for block in blocks:
        pulses.extend(encode_block(block, leadout=leadout))
        pulses.append(PILOT_T * gap_pulses)
    with open(path, "wb") as f:
        f.write(pulses_to_raw_u8(pulses))


def write_wav(path, block, *, leadout=0, sample_rate=SAMPLE_RATE):
    """Write one block as a mono 16-bit WAV.

    ZEsarUX mounts such a file with --realtape, which feeds it to the emulated
    EAR input without any virtual audio device -- the deterministic way to test
    the downstream direction.
    """
    pcm = pulses_to_pcm(encode_block(block, leadout=leadout), sample_rate)
    with wave.open(path, "wb") as w:
        w.setnchannels(1)
        w.setsampwidth(2)
        w.setframerate(sample_rate)
        w.writeframes(pcm)


# --------------------------------------------------------------------------
# Decoder
# --------------------------------------------------------------------------

class PulseDecoder:
    """Adaptive streaming decoder; sample-rate independent.

    Half-pulse widths are measured between hysteresis zero crossings and every
    later threshold is a ratio of the measured pilot width, so a 48 kHz WAV and
    a low-rate ZEsarUX --aofile dump decode identically. Bits are classified on
    the sum of each half-pulse pair, which doubles the timing resolution at
    coarse sample rates.

    Adapted from the proof of concept's TapeDecoder, with two changes: the
    pilot locks after PILOT_LOCK pulses instead of 256 (our preamble is 48),
    and the data phase is length-driven rather than gap-driven.
    """

    #: Pulses of consistent width needed before we believe it is a preamble.
    #: Must be comfortably below PREAMBLE_PULSES so the sync pair is not eaten
    #: by the lock, and high enough that noise does not trigger it.
    PILOT_LOCK = 16
    #: Below this many samples per half-pulse the widths are too coarse to
    #: classify, whatever the sample rate nominally is.
    MIN_PILOT_WIDTH = 3.0

    #: The DC estimate has to settle inside the preamble, and our preamble is
    #: 29.7 ms rather than the ROM's 1.99 s. The proof of concept tracked DC
    #: with a single 0.0005 coefficient -- a 2,000-sample (41 ms) time
    #: constant, which had two seconds of pilot to converge in. At our
    #: preamble length that estimate is still miles off when the data starts,
    #: and on an 8-bit dump with a large DC offset both halves of the signal
    #: then land on the same side of the threshold and no edge is ever seen.
    #: So: converge fast for a warmup window, then switch to the slow
    #: coefficient that rejects drift without chasing the signal.
    DC_WARMUP_SAMPLES = 512
    DC_ALPHA_WARMUP = 0.02
    DC_ALPHA_TRACK = 0.0005

    def __init__(self):
        self.dc = 0.0
        self.peak = 0.0
        self.level = 0
        self.run = 0
        self.seen = 0
        self._reset()

    def _reset(self):
        self.state = "idle"
        self.pilot = 0.0
        self.pilot_count = 0
        self.sync_buffer = []
        self.halves = []
        self.bits = []
        self.data = bytearray()
        self.want = None            # total block length, known once LEN is in

    def feed(self, samples):
        """Consume samples, yielding each complete block as it finishes."""
        for value in samples:
            # ZEsarUX emits the MIC bit as a small swing around a large
            # constant level, so the DC offset is tracked and removed rather
            # than assumed to be zero.
            if self.seen == 0:
                self.dc = float(value)      # start inside the signal, not at 0
            self.seen += 1
            alpha = (self.DC_ALPHA_WARMUP if self.seen <= self.DC_WARMUP_SAMPLES
                     else self.DC_ALPHA_TRACK)
            self.dc += (value - self.dc) * alpha
            ac = value - self.dc
            self.peak = max(abs(ac), self.peak * 0.99995)
            threshold = max(1.0, self.peak * 0.25)
            if ac > threshold:
                new_level = 1
            elif ac < -threshold:
                new_level = -1
            else:
                new_level = self.level
            if new_level != self.level:
                width, self.run = self.run, 0
                if self.level != 0:
                    yield from self._half_pulse(width)
                self.level = new_level
            self.run += 1
            # A long gap abandons a partial block: whatever we had is junk.
            if self.state == "data" and self.pilot and self.run > 6 * self.pilot:
                self._reset()

    def _half_pulse(self, width):
        if self.state == "idle":
            if self.pilot and abs(width - self.pilot) <= 0.3 * self.pilot:
                self.pilot_count += 1
                self.pilot += (width - self.pilot) * 0.05
                if (self.pilot_count >= self.PILOT_LOCK
                        and self.pilot >= self.MIN_PILOT_WIDTH):
                    self.state = "pilot"
            else:
                self.pilot = float(width)
                self.pilot_count = 1
            return

        if self.state == "pilot":
            # Markedly LONGER than the locked pilot means the lock was wrong:
            # start over with this width as the new candidate.
            #
            # This is not hypothetical. A 32-byte leadout is 512 identical
            # half-pulses, which is a textbook false preamble -- the lock
            # takes it, and then nothing can dislodge it, because the sync
            # test below only ever looks for something SHORTER. The real
            # preamble that follows is longer, so it never matches; the data
            # after it is longer too. The decoder would sit locked on the
            # leadout forever, silent, with a perfectly good signal arriving.
            if width > 1.3 * self.pilot:
                self.state = "idle"
                self.pilot = float(width)
                self.pilot_count = 1
                return
            # Markedly shorter than the pilot: either SYNC1, or edge ringing
            # splitting a pilot pulse. The next widths decide.
            if width < 0.7 * self.pilot:
                self.state = "sync_wait"
                self.sync_buffer = []
            return

        if self.state == "sync_wait":
            self.sync_buffer.append(width)
            if len(self.sync_buffer) < 3:
                return
            sync2_like = self.sync_buffer[0] < 0.5 * self.pilot
            pilot_sized = sum(1 for w in self.sync_buffer
                              if w >= 0.87 * self.pilot)
            if not sync2_like or pilot_sized >= 2:
                self.state = "pilot"          # ringing, not a real sync
            else:
                self.state = "data"
                self.halves = []
                self.bits = []
                self.data = bytearray()
                self.want = None
                for buffered in self.sync_buffer[1:]:
                    yield from self._data_half(buffered)
            self.sync_buffer = []
            return

        if self.state == "data":
            yield from self._data_half(width)

    def _data_half(self, width):
        if width > 3 * self.pilot:
            self._reset()                     # nonsense width: give up
            return

        self.halves.append(width)
        if len(self.halves) < 2:
            return

        first, second = self.halves
        self.halves = []
        # Pair sums: a 0 is ~2*855/2168 of the pilot, a 1 is ~2*1710/2168.
        threshold = self.pilot * (ZERO_T + ONE_T) / PILOT_T
        self.bits.append(1 if first + second > threshold else 0)
        if len(self.bits) < 8:
            return

        value = 0
        for bit in self.bits:
            value = (value << 1) | bit
        self.bits = []
        self.data.append(value)

        if self.want is None and len(self.data) >= _HEADER_BYTES:
            length = self.data[1]
            if length > _MAX_PAYLOAD:
                self._reset()                 # cannot be a legal frame
                return
            self.want = length + _FRAME_OVERHEAD

        if self.want is not None and len(self.data) >= self.want:
            block = bytes(self.data[:self.want])
            self._reset()
            yield block


def decode_raw(data, *, unsigned8):
    """Decode a whole buffer at once. Returns every block recovered.

    `unsigned8` selects the ZEsarUX --aofile representation (unsigned 8-bit
    mono); otherwise the buffer is signed 16-bit little-endian, which is what
    write_wav() and pulses_to_pcm() produce.
    """
    if unsigned8:
        samples = [b - 128 for b in data]
    else:
        samples = [int.from_bytes(data[i:i + 2], "little", signed=True)
                   for i in range(0, len(data) - len(data) % 2, 2)]
    return list(PulseDecoder().feed(samples))
