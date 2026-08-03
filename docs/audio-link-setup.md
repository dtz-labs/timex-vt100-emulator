# Audio link setup

The audio link lets the terminal talk to a PC over the tape ports — EAR in,
MIC out — with no Interface 1, no serial adapter and no extra hardware beyond
two audio cables. It speaks Half-Duplex Terminal Protocol v1 over a physical
layer of our own (design decision D1, option C): our own EAR decoder and MIC
send loop, at the ROM's bit timings, calling no ROM routine at all. That last
part is what keeps it working on a TS2068, whose ROM has none of the standard
tape entry points at the usual addresses.

- Protocol and decisions: [`docs/superpowers/specs/2026-08-02-audio-link-design.md`](superpowers/specs/2026-08-02-audio-link-design.md)
- Wire format: `include/alink_phy.h`, `tools/alink/phy.py`
- PC-side runner: `tools/alink/main.py`

## 1. Build the audio images

```sh
export PATH="$HOME/Programowanie/z88dk/bin:$PATH"
export ZCCCFG="$HOME/Programowanie/z88dk/lib/config"
make audio        # build/term-audio.tap     -- Timex, 80x24 hi-res
make audio-zx     # build/term-zx-audio.tap  -- ZX Spectrum, 40x24 ULA
```

These are the ZRCP builds with the audio backend compiled in instead
(`-DCONN_BACKEND_AUDIO`). The banner says `Audio link. Waiting for HELLO.`
rather than the bridge help, and the border flashes while a frame is on the
wire — the quickest way to see the link is alive without instrumentation.

## 2. Check it works, with no audio hardware at all

```sh
make smoke-audio
```

This is the honest end-to-end verification, and it needs no virtual audio
device, no loopback cable and no `sox`. It runs three ZEsarUX sessions:

| Run | Image | What it proves |
|---|---|---|
| downstream | `term-audio.tap` on TC2048 | a generated tape reaches the VT parser and the text lands on screen, read back byte-exact over ZRCP |
| downstream | `term-zx-audio.tap` on 48k | the same, on the 40-column ULA build |
| upstream | `alink-txprobe.tap` | `alink_phy_send()`'s output is captured and decodes byte-for-byte |

It takes about two minutes. The harness is `test/alink_zesarux.py`; run a
single scenario directly when you are debugging one direction:

```sh
python3 test/alink_zesarux.py --scenario link --tap build/term-audio.tap \
    --machine TC2048 --geom hires --keep     # --keep leaves the tape and dump
python3 test/alink_zesarux.py --scenario tx  --tap build/alink-txprobe.tap
```

### Why the upstream direction needs its own run

ZEsarUX has no MIC recorder. `--aofile` records the audio *output*, and the
MIC bit only reaches that output through the ULA's EAR feedback: with nothing
connected to EAR, `IN A,(0xFE)` bit 6 reads back the last MIC bit written
(issue 2/3 behaviour, which ZEsarUX emulates). Attach any EAR source —
a `--realtape` file, or an external audio input — and the feedback is gone.

Measured, not assumed: running the terminal with a tape and `--aofile`
together produces a capture in which the ZX is provably transmitting (its PC
samples inside `alink_phy_send`, and the text reaches the screen) while the
dump holds only the emulator's rendering of the EAR input, with no trace of
the reply. The beeper is in that same dump at 2 units of swing against the
tape's 80, so the capture is not simply empty.

So the two directions are probed separately, and both against real audio.

## 3. Run it against real hardware

This is the setup the link was designed for, and the one where nothing is
special: MIC and EAR are two independent cables.

1. ZX EAR ← PC headphone/line out. ZX MIC → PC line in.
2. Load `term-audio.tap` (or `term-zx-audio.tap`) as usual.
3. Point the runner at the PC's audio devices and give it a shell:

```sh
python3 -m alink.main --aofile /path/to/capture.raw --device "ZX Link" \
    --command /bin/zsh --cols 80
```

`--device` is the CoreAudio output device `ffmpeg` plays the downstream pulses
into; `--aofile` is the growing raw capture the upstream decoder follows. Set
`--cols 40` for the ZX build (it also picks the `zx-vt102` terminfo).

Before wiring anything, check the runner's own plumbing:

```sh
python3 -m alink.main --selftest
```

Levels matter more than anything else here. The decoder tracks the signal
envelope, so it does not care about absolute volume, but it does need both
polarities to be present: start around 50% output volume and raise it until
frames decode, rather than starting at maximum, where clipping and the input
stage's DC blocking do more harm than the extra amplitude does good.

## 4. Run it against ZEsarUX with a virtual audio device

Possible in principle — route the runner's output into a virtual device
(BlackHole, Loopback) and select it as ZEsarUX's External Audio Source — but
**the upstream direction will not work this way**, for the reason in section
2: giving ZEsarUX an external EAR source removes the MIC feedback that
`--aofile` depends on. This path is therefore not verified here, and
`make smoke-audio` deliberately does not use it.

If you want an interactive emulator session, use the terminal image with the
ZRCP backend (`make tap` + `make bridge-zrcp`) instead: it is the same
terminal core over a transport that the emulator supports properly. The audio
backend's own correctness is covered by `make smoke-audio`.

## 5. When something does not work

**"Nothing arrives at all", downstream.**
Check the tape file's *extension*. ZEsarUX identifies a real-tape file by
name: `.rwa` is read directly, `.raw` is rejected as "Unknown input tape
type", and `.wav` is piped through `sox` — which, when `sox` is absent, prints
one warning and then silently plays nothing, a failure indistinguishable from
a broken decoder. `phy.write_realtape()` writes the bare form ZEsarUX reads
with no external tool: 44,100 Hz, mono, 8-bit unsigned.

**"Nothing arrives at all", upstream, under the emulator.**
Expected if anything is attached to EAR. See section 2.

**Frames decode, but only sometimes.**
The terminal polls the line from its main loop and is deaf while it renders or
transmits — measured at roughly 10% of an idle second, more while scrolling.
The master's retry rule covers this (any valid response completes a
transaction; a repeated frame is dropped as a duplicate by SEQ), which is why
the test harness simply repeats each frame on the tape.

**The border flashes but nothing appears on screen.**
The frame is being received and rejected. Every frame carries a CRC-16 and the
slave silently ignores a block that fails validation, exactly as if it had
never arrived — so this is a signal-integrity problem, not a protocol one.
Lower the output level and check for clipping.

**A capture full of good-looking pulses decodes to zero blocks.**
This was a real bug, fixed in `PulseDecoder`: the mid-point between the two
signal levels is now tracked as an envelope (instant attack, slow release)
rather than as a running mean. A mean only sits between the levels when the
signal spends about half its time at each, and MIC output does not — it rests
at one level and pulses to the other. Against an isolated burst the mean was
still parked at the resting level when the preamble arrived, so every sample
landed on the same side of the threshold. `test_isolated_unipolar_burst_decodes()`
in `test/test_alink_phy_py.py` is the regression test.

## 6. What it costs

Measured on this implementation, not derived from the design sketch:

| | |
|---|---|
| Preamble | 48 pilot pulses = 29.7 ms |
| Empty poll, on the wire | 4 bytes = 0.051 s |
| Full payload (64 B) | 68 bytes = 0.427 s |
| Throughput | 134 B/s |
| Idle deafness | ~10% |
| Frame integrity, on target | 234/234 blocks in the leadout measurement; 6/6 byte-identical in the `smoke-audio` run that set its 90% floor |

The 32-byte leadout the design originally called for is **not** used: frames
end by length rather than by silence, so the padding protected nothing, and it
was a textbook false preamble that could deadlock a decoder onto it. Removing
it took throughput from 106 B/s to 134 B/s and idle deafness from 23% to 10%.
See `src/alink_phy.c`'s `N_LEADOUT` for the measurement.
