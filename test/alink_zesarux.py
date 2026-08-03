#!/usr/bin/env python3
"""End-to-end audio-link checks under ZEsarUX -- both directions, no audio device.

This is the on-target counterpart to the host suites. `test_alink_xcheck.py`
proves the C slave and the Python master agree about the protocol, and
`test_alink_phy_py.py` proves the Python codec round-trips; neither runs a
single Z80 instruction of `src/alink_phy.c`, which is target-only by design
(absolute ports, cycle-counted loops -- the rule `blit_hires.c` follows). This
file is where that code is exercised, against real images, in both directions:

  scenario `link`  the terminal image, fed a generated .rwa tape via --realtape:
                   EAR receive -> slave -> VT parser -> pixels, read back over
                   ZRCP the way test/zesarux_smoke.py does.
  scenario `tx`    build/alink-txprobe.tap, no tape at all: MIC send loop ->
                   --aofile capture -> tools/alink/phy.py decode.

Neither needs a virtual audio device, a loopback cable, or `sox`.

Why two runs and not one
------------------------

**ZEsarUX cannot capture MIC while a tape is inserted.** There is no MIC
recorder; --aofile records the audio OUTPUT, and the MIC bit only reaches that
output through the ULA's EAR feedback -- `IN A,(0xFE)` bit 6 reads back the
last MIC bit written when nothing is connected to EAR, which is issue 2/3
behaviour that ZEsarUX emulates. Insert a --realtape and EAR comes from the
tape instead, so the feedback path is gone.

Measured, not assumed. Running the terminal with a tape and --aofile together
produces a capture in which the ZX is provably transmitting (its PC samples
inside `alink_phy_send`, and the delivered text reaches the screen) while the
dump contains only two levels -- the emulator's rendering of the EAR input, 80
units of swing -- and no trace of the reply. The beeper is in there at 2 units,
so the dump is not simply empty. This also explains, in retrospect, how Task 2
captured the MIC output at all: that probe ran with no tape, so its own MIC
output fed back into EAR and out through the audio path.

So the downstream direction is checked against the real terminal image, and the
upstream direction against a probe that transmits with nothing inserted. Every
check below is on real audio; none of them is a simulation.

The tape is pre-recorded, so the whole master side is written before the
emulator starts
---------------------------------------------------------------------------

--realtape plays a file, and a file cannot react to what the Z80 says. This
works because the slave is deterministic: `session_reset()` in
`src/alink_slave.c` sets `last_rx_seq = 1`, so the master's first LINK carries
`seq=0, ack=1` -- byte for byte what `alink.master.Master._link_transaction()`
puts on the wire after a handshake. The tape is a recording of a real master's
first two transactions.

Frames are repeated because the slave is not always listening: it polls the
line from the main loop and is deaf while it renders or transmits. A repeated
HELLO just resets the session again, and a repeated LINK carries the same SEQ,
so the slave drops it as a duplicate -- repetition costs nothing but tape.

Hard-won facts about the emulator, worth not rediscovering
----------------------------------------------------------

  * The --realtape file must be named `.rwa`. A `.raw` is rejected outright
    ("Unknown input tape type"), and a `.wav` is piped through `sox` -- which,
    when absent, produces one warning and then silence that looks exactly like
    a broken decoder. `phy.pulses_to_raw_u8()` writes the bare 44,100 Hz mono
    8-bit unsigned form ZEsarUX reads with no external tool at all.
  * The tape starts playing when the emulator starts, so it opens with a
    stretch of constant level: no edges, no carrier, and the terminal has time
    to load and paint its banner before the first preamble arrives.
  * The emulator's timeline is not wall-clock. Both scenarios poll for their
    result rather than sleeping a computed time.

Usage (see `make smoke-audio`):

    python3 test/alink_zesarux.py --scenario link --tap build/term-audio.tap \\
        --machine TC2048 --geom hires
    python3 test/alink_zesarux.py --scenario tx --tap build/alink-txprobe.tap
"""

import argparse
import os
import subprocess
import sys
import tempfile
import time

ROOT = os.path.abspath(os.path.join(os.path.dirname(__file__), ".."))
sys.path.insert(0, os.path.join(ROOT, "tools"))
sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))

from alink import frame, phy          # noqa: E402  (path set above)
import zesarux_smoke as smoke         # noqa: E402


#: The block test/alink_txprobe.c transmits, encoded here from the same codec
#: the probe uses on target -- so the comparison is byte-for-byte against the
#: protocol, not against a hand-typed hex string.
TXPROBE_BLOCK = frame.encode(frame.LINK, 1, 0, b"TX PROBE")

#: Constant level for the tape's lead-in. Matches the low level
#: pulses_to_raw_u8() emits, so the first pilot pulse is the tape's first edge.
SILENCE_BYTE = b"\x30"


# --------------------------------------------------------------------------
# The tape
# --------------------------------------------------------------------------


def hello_block():
    """The master's HELLO, identical to `Master._handshake()`'s offer."""
    offer = bytes([frame.VERSION, frame.CAPS, frame.MAX_PAYLOAD])
    return frame.encode(frame.HELLO, 0, 0, offer)


def link_block(payload):
    """The master's first LINK of a session: SEQ=0, and ACK=1 because the
    master's `last_rx_seq` starts at 1 (so the slave's first SEQ=0 counts as
    new). Mirrors `Master._link_transaction()` exactly."""
    return frame.encode(frame.LINK, 0, 1, payload)


def build_tape(path, blocks, *, lead_in_s, gap_s):
    """Render `blocks` to a --realtape .rwa file, one gap between each.

    The gap is emitted as a single very wide pulse, i.e. a stretch of constant
    level: no edges, so the terminal sees no carrier and gets a clean preamble
    to lock onto rather than joining a transmission mid-flight.
    """
    pulses = []
    gap_t = int(gap_s * phy.ZX_CLOCK_HZ)
    for block in blocks:
        pulses.extend(phy.encode_block(block))
        pulses.append(gap_t)
    with open(path, "wb") as f:
        f.write(SILENCE_BYTE * int(lead_in_s * phy.REALTAPE_RATE))
        f.write(phy.pulses_to_raw_u8(pulses))
    return path


# --------------------------------------------------------------------------
# ZEsarUX
# --------------------------------------------------------------------------


def launch(zx, tap, machine, port, aofile, realtape=None, zrcp=True):
    """The headless launch `zesarux_smoke.launch()` uses, plus the audio paths.
    --vo/--ao null keep it off the screen and off the speakers; --aofile still
    receives the audio stream with the null driver selected."""
    args = [
        zx,
        "--noconfigfile",
        "--machine", machine,
        "--tape", tap,
        "--vo", "null",
        "--ao", "null",
        "--nosplash",
        "--quickexit",
        "--fastautoload",
        "--aofile", aofile,
    ]
    if zrcp:
        args += ["--enable-remoteprotocol", "--remoteprotocol-port", str(port)]
    if realtape:
        args += ["--realtape", realtape]
    return subprocess.Popen(args, stdout=subprocess.DEVNULL, stderr=subprocess.STDOUT)


def stop(proc):
    proc.terminate()
    try:
        proc.wait(timeout=5)
    except Exception:
        proc.kill()


def screen_rows(s, geom_name, cols, font):
    """The whole screen as 24 decoded rows, byte-exact against the project font
    tables -- the same decode path `zesarux_smoke.py` uses."""
    geom = smoke.GEOMETRIES[geom_name]
    display = smoke.read_display(s, geom)
    return [smoke.decode_row(display, geom, font, r, 0, cols) for r in range(24)]


def wait_for_text(s, geom_name, cols, needle, deadline_s, poll_s=1.5):
    """Poll the display until `needle` appears, or the deadline passes.

    Polling rather than sleeping a fixed time: the tape's timeline is in
    EMULATED seconds while a sleep here is wall-clock, so a fixed wait either
    flakes or wastes the difference. Rows are joined without a separator so a
    line that wraps at 40 columns still matches.
    """
    font = smoke.load_project_font()           # parsed once, not once per poll
    end = time.time() + deadline_s
    while True:
        rows = screen_rows(s, geom_name, cols, font)
        if needle in "".join(rows):
            return True, rows
        if time.time() >= end:
            return False, rows
        time.sleep(poll_s)


# --------------------------------------------------------------------------
# The capture
# --------------------------------------------------------------------------


def decode_capture(path):
    """Every block the --aofile dump yields, paired with its parsed frame
    (None when the block does not survive validation)."""
    with open(path, "rb") as f:
        data = f.read()
    blocks = phy.decode_raw(data, unsigned8=True)
    return [(b, frame.decode(b)) for b in blocks], len(data)


_TYPE_NAMES = {frame.HELLO: "HELLO", frame.WELCOME: "WELCOME",
               frame.LINK: "LINK", frame.BYE: "BYE"}


def describe(frames):
    counts = {}
    for _, f in frames:
        key = "invalid" if f is None else _TYPE_NAMES[f.type]
        counts[key] = counts.get(key, 0) + 1
    return ", ".join("%s=%d" % kv for kv in sorted(counts.items())) or "none"


# --------------------------------------------------------------------------
# Scenarios
# --------------------------------------------------------------------------


def scenario_link(args, checks, tape, dump):
    """Downstream: tape -> EAR -> alink_phy_receive() -> slave -> screen."""
    cols = args.cols or smoke.GEOMETRIES[args.geom]["cols"]
    payload = args.text.encode("ascii") + b"\r\n"
    blocks = [hello_block()] * args.hellos + [link_block(payload)] * args.links
    build_tape(tape, blocks, lead_in_s=args.lead_in, gap_s=args.gap)
    print("tape: %s (%d bytes, %d HELLO + %d LINK, %.1fs lead-in, %.2fs gaps)"
          % (tape, os.path.getsize(tape), args.hellos, args.links,
             args.lead_in, args.gap))

    proc = launch(args.zx, args.tap, args.machine, args.port, dump, realtape=tape)
    rows = []
    found = False
    try:
        s = smoke.connect(args.port, args.connect_wait)
        try:
            found, rows = wait_for_text(s, args.geom, cols, args.text, args.deadline)
        finally:
            s.close()
    finally:
        stop(proc)

    screen = "\n".join(rows)
    smoke.check(checks, "delivered text on screen (%r)" % args.text, found,
                "" if found else "screen=%r" % screen)

    # Protocol v1 rule 4: a HELLO's payload is consumed by negotiation and must
    # never enter the byte stream. Its third byte is MAX_PAYLOAD = 64 = '@',
    # which the terminal would print in plain sight if the slave leaked it.
    leaked = "@" in "".join(rows)
    smoke.check(checks, "HELLO payload never reached the VT parser",
                not leaked, "screen=%r" % screen if leaked else "")

    # The capture is the emulator's own rendering of the EAR input, not the
    # ZX's reply (see the module docstring). Decoding it is still worth doing:
    # it is the only check that the signal ZEsarUX actually presented to the
    # EAR pin carries our frames intact, which is what separates "the terminal
    # ignored a good signal" from "the tape we generated was malformed".
    frames, dump_bytes = decode_capture(dump)
    print("capture: %s (%d bytes, %d blocks: %s)"
          % (dump, dump_bytes, len(frames), describe(frames)))
    kinds = set(_TYPE_NAMES[f.type] for _, f in frames if f is not None)
    smoke.check(checks, "downstream frames intact at the emulator's EAR input",
                {"HELLO", "LINK"} <= kinds and all(f is not None for _, f in frames),
                "%d blocks: %s" % (len(frames), describe(frames)))


def scenario_tx(args, checks, dump):
    """Upstream: alink_phy_send() -> MIC -> EAR feedback -> --aofile -> decode.

    No tape is inserted, which is the whole point: with one inserted, EAR comes
    from the tape and the MIC output never reaches the capture.
    """
    proc = launch(args.zx, args.tap, args.machine, args.port, dump, zrcp=False)
    try:
        # Poll the growing dump rather than sleeping a fixed time, for the same
        # reason wait_for_text() does: the emulator's clock is not the wall's.
        end = time.time() + args.deadline
        frames = []
        while time.time() < end:
            time.sleep(2.0)
            if not os.path.exists(dump):
                continue
            frames, dump_bytes = decode_capture(dump)
            if len(frames) >= args.tx_frames:
                break
    finally:
        stop(proc)

    frames, dump_bytes = decode_capture(dump)
    print("capture: %s (%d bytes, %d blocks: %s)"
          % (dump, dump_bytes, len(frames), describe(frames)))

    smoke.check(checks, "MIC transmissions captured (>= %d blocks)" % args.tx_frames,
                len(frames) >= args.tx_frames, "%d blocks" % len(frames))

    # Byte-for-byte against the block the probe was told to send. The measured
    # rate on this path is 234/234 blocks (src/alink_phy.c's leadout note) and
    # 6/6 in the run this floor was set from; the floor is deliberately well
    # below both, so it fails on a broken timing loop rather than on one bad
    # frame at the moment the emulator was killed.
    exact = sum(1 for b, _ in frames if b == TXPROBE_BLOCK)
    ratio = exact / len(frames) if frames else 0.0
    smoke.check(checks,
                "captured blocks are byte-identical to the sent frame (>= %.0f%%)"
                % (args.min_valid * 100),
                bool(frames) and ratio >= args.min_valid,
                "%d/%d = %.1f%% (want %s)"
                % (exact, len(frames), ratio * 100, TXPROBE_BLOCK.hex()))


# --------------------------------------------------------------------------
# main
# --------------------------------------------------------------------------


def main():
    ap = argparse.ArgumentParser(description=__doc__)
    ap.add_argument("--scenario", choices=["link", "tx"], default="link")
    ap.add_argument("--tap", default=None,
                    help="default: build/term-audio.tap, or the probe for --scenario tx")
    ap.add_argument("--machine", default="TC2048")
    ap.add_argument("--geom", choices=list(smoke.GEOMETRIES), default="hires")
    ap.add_argument("--cols", type=int, default=None)
    ap.add_argument("--port", type=int, default=int(os.environ.get("ZRCP_PORT", "10001")))
    ap.add_argument("--text", default="AUDIO LINK OK",
                    help="payload carried by the LINK frames on the tape")
    ap.add_argument("--hellos", type=int, default=6,
                    help="HELLO repetitions (the slave is deaf part of the time)")
    ap.add_argument("--links", type=int, default=8, help="LINK repetitions")
    ap.add_argument("--lead-in", type=float, default=10.0,
                    help="seconds of silence before the first frame, for boot")
    ap.add_argument("--gap", type=float, default=0.5,
                    help="seconds of silence between frames")
    ap.add_argument("--deadline", type=float, default=90.0,
                    help="seconds to wait for the scenario's result")
    ap.add_argument("--connect-wait", type=float, default=6.0,
                    help="seconds before the first ZRCP connection attempt")
    ap.add_argument("--tx-frames", type=int, default=5,
                    help="blocks the tx scenario must capture")
    ap.add_argument("--min-valid", type=float, default=0.9,
                    help="fraction of captured blocks that must be exact")
    ap.add_argument("--keep", action="store_true", help="keep the tape and dump")
    ap.add_argument("--zx", default=os.environ.get("ZX", smoke.DEFAULT_ZX))
    args = ap.parse_args()

    default_tap = ("build/alink-txprobe.tap" if args.scenario == "tx"
                   else "build/term-audio.tap")
    # ZEsarUX silently ignores a relative --tape (it logs to its own stdout,
    # which this script does not capture, and then sits at the BASIC prompt).
    args.tap = os.path.abspath(args.tap or os.path.join(ROOT, default_tap))
    if not os.path.exists(args.tap):
        raise SystemExit("no such TAP: %s (build it first)" % args.tap)

    tmp = tempfile.gettempdir()
    tape = os.path.join(tmp, "alink_%s_%d.rwa" % (args.scenario, args.port))
    dump = os.path.join(tmp, "alink_%s_%d.raw" % (args.scenario, args.port))
    for stale in (tape, dump):
        if os.path.exists(stale):
            os.remove(stale)

    print("=== alink %s: tap=%s machine=%s ==="
          % (args.scenario, os.path.basename(args.tap), args.machine))

    checks = []
    if args.scenario == "link":
        scenario_link(args, checks, tape, dump)
    else:
        scenario_tx(args, checks, dump)

    if not args.keep:
        for path in (tape, dump):
            if os.path.exists(path):
                os.remove(path)

    ok = all(c[1] for c in checks)
    print()
    print("VERDICT: %s -- alink %s on %s (%d/%d checks passed)"
          % ("PASS" if ok else "FAIL", args.scenario, os.path.basename(args.tap),
             sum(1 for c in checks if c[1]), len(checks)))
    return 0 if ok else 1


if __name__ == "__main__":
    raise SystemExit(main())
