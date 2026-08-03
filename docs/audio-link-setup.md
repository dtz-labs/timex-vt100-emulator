# Audio Link — Setup and Verification

How to build the audio path between a PC and an emulated (or real) ZX
Spectrum / Timex, and how to prove it works before any terminal code is
involved.

**Scope.** This document covers the audio plumbing and its verification —
everything you can set up and test **today**. Running the VT102 terminal over
that path needs `src/alink_phy.c` and `tools/alink/phy.py`, which are PR B of
issue #5 and do not exist yet. What exists today is the protocol engine
(`src/alink_frame.c`, `src/alink_slave.c`, `tools/alink/`), which is tested
over a pipe and never touches audio.

Doing the setup below now is worthwhile regardless: if the audio path does not
pass the verification in section 5, no amount of terminal code will help, and
the one manual, one-off part of the whole feature is out of the way.

---

## 1. Why the two directions are not symmetric

This is the single most surprising thing about the setup, and it is not a
design choice — it is forced by how ZEsarUX handles audio.

```
PC → ZX   (EAR)    encoder → ffmpeg → virtual audio device → ZEsarUX
                                                             External Audio Source

ZX → PC   (MIC)    ZEsarUX --aofile  →  raw sample dump  →  decoder
```

**Downstream needs a virtual audio device.** ZEsarUX's *External Audio Source*
reads from a CoreAudio input, so something has to present the encoded pulses
as a microphone-like input. That is what the virtual device does.

**Upstream must NOT use the audio output.** ZEsarUX pushes samples into a FIFO
that its CoreAudio driver drains; when the FIFO fills, audio is dropped and the
binary logs `audiocoreaudio FIFO full`. A dropped millisecond is four or more
bits of tape data gone, and no decoder can recover that. This was diagnosed in
the `poc-zx-audio-link-from-zx-to-mac` proof of concept by histogramming pulse
widths from a real capture: phantom 2–7-sample spikes, pairs of pulses merged
into one, pilot width smeared across ±13%.

The fix is `--aofile`, which writes every generated sample to a raw file
(unsigned 8-bit mono) synchronously with emulation. Nothing is ever dropped.
A reader follows that file as it grows, like `tail -f`, with about a quarter
second of latency.

**On real hardware this asymmetry disappears** — you record the MIC socket with
any audio input. It is purely an emulator artifact.

---

## 2. Prerequisites

```bash
brew install ffmpeg python
brew install --cask zesarux          # or download ZEsarUX 13.0 manually
```

The launcher scripts expect ZEsarUX at `/Applications/ZEsarUX.app`; set
`ZESARUX_APP` if it lives elsewhere. Python 3.9+ is enough — everything on the
PC side uses the standard library only.

---

## 3. Choosing a virtual audio device

You need a CoreAudio device that can be an **output** for ffmpeg and an
**input** for ZEsarUX at the same time. Options, with an honest account of what
is actually known about each:

| Option | Cost | Status |
|---|---|---|
| **Loopback** (Rogue Amoeba) | paid, trial available | **Recommended.** The only one verified end to end for this project. |
| **BlackHole 2ch** | free | Failed in the proof of concept's environment — see below. Worth retrying. |
| **VB-CABLE** | donationware | Untested for this project. Plausible alternative; report back if you try it. |
| **Audio Hijack** (Rogue Amoeba) | paid | Same vendor and driver family as Loopback. Should work; untested here. |
| **Soundflower** | free | **Do not.** Abandoned, kext-based, does not load on modern macOS or Apple Silicon. |

```bash
brew install --cask loopback          # recommended
brew install --cask blackhole-2ch     # free, see the caveat below
brew install --cask vb-cable          # free, untested here
```

### The BlackHole finding, stated precisely

The `poc-zx-audio-link-from-mac-to-zx` proof of concept reports that **BlackHole
0.6.1 and 0.7.1 did not work** in its test environment: the devices were visible
to CoreAudio, but every recording made through them contained only zero-valued
samples. Loopback passed the identical end-to-end test on the same machine,
which is why this project uses Loopback.

**That is one Mac's observation, not a general claim.** It was macOS 26.5.2 on
Apple Silicon. BlackHole is widely used and works for many people. If you would
rather not pay for Loopback, try BlackHole first and run the verification in
section 5 — it takes two minutes and gives an unambiguous answer. If it reports
zeros, you have reproduced the finding and Loopback is the known-good fallback.

### The Loopback trial

The trial provides all features but overlays noise after 20 minutes of active
device use. Turning the virtual device off and back on resets the timer. For
setup and verification that is more than enough; for actually using the
terminal it is not, and noise will corrupt frames.

---

## 4. Creating the device

With **Loopback**:

1. Open Loopback, click **New Virtual Device**.
2. Rename it to exactly **`ZX Link`**. The scripts look it up by that name.
3. Leave the default **Pass-Thru** source enabled — this is what lets one
   device be an output and an input simultaneously.
4. Keep two output channels, mapped 1→1 and 2→2.
5. Do **not** add a microphone, speakers, or an application as another source.
   Anything else on the device injects noise into the tape signal.
6. If necessary, set `ZX Link` to **48,000 Hz** in *Audio MIDI Setup*.

On first launch Loopback asks to install its **ARK** component and for System
Audio and microphone access. Grant all of them; ffmpeg needs the microphone
permission for the Terminal app when it records from an input.

Then in *System Settings → Sound*:

- **Input:** `ZX Link`
- **Output:** leave on your speakers

Do not select `ZX Link` as the system output during normal use. The sender
addresses the device directly, so the rest of macOS audio keeps playing through
the speakers.

With **BlackHole** or **VB-CABLE** there is no configuration UI — installing the
driver creates the device, and it is already a loopback by nature. Select it as
the system input the same way. Note that the name will be `BlackHole 2ch` or
`VB-Cable` rather than `ZX Link`, which matters for any script that looks the
device up by name.

---

## 5. Verifying the audio path

**Do this before anything else.** It answers "is the cable real?" independently
of any Z80 or protocol code.

The proof-of-concept repositories carry a ready-made test. Clone and run it:

```bash
git clone https://github.com/dtz-labs/poc-zx-audio-link-from-mac-to-zx.git
cd poc-zx-audio-link-from-mac-to-zx
chmod +x ./*.sh ./diagnostics/*.sh
./diagnostics/test-virtual-audio.sh
```

It identifies the device, records eight seconds, sends a 1 kHz tone through it,
and measures the result. A working path looks like:

```
Peak:     2853 (-21.2 dBFS)
RMS:      807 (-32.2 dBFS)
PASS: a clear signal passed through the virtual audio device.
```

**Peak and RMS both zero** is the BlackHole failure mode described in section 3.

Then verify the emulator half — the receiver from the proof of concept, driven
by real audio:

```bash
./start-zesarux.sh                    # terminal 1: builds and loads receiver.tap
./send.sh "HELLO FROM MAC"            # terminal 2
```

Wait for `ZX AUDIO LINK READY` on the emulated screen, press `F1`, then
uppercase `E` in the *External Audio Source* window to enable the input. During
transmission that window should report a pilot tone around 809 Hz and an
advancing record buffer.

For the return direction:

```bash
git clone https://github.com/dtz-labs/poc-zx-audio-link-from-zx-to-mac.git
cd poc-zx-audio-link-from-zx-to-mac
./start-zesarux.sh                    # terminal 1
./receive-aofile.sh                   # terminal 2
```

Type a line in the emulator window and press ENTER. The border shows the SAVE
stripes for about three seconds and the line appears in terminal 2.

If both of those work, your audio path is sound and PR B has somewhere to run.

---

## 6. Troubleshooting

**Several HAL drivers are installed.** Other applications install CoreAudio
plug-ins of their own — check with:

```bash
ls -d /Library/Audio/Plug-Ins/HAL/*.driver
```

Meeting, streaming and headphone utilities all put drivers here. With more than
one virtual device present, automatic device lookup can pick the wrong one; the
proof-of-concept scripts accept `ZX_AUDIO_DEVICE_INDEX` and
`ZX_AUDIO_INPUT_INDEX` to override it. List the indices with
`./diagnostics/list-audio-devices.sh`.

**A Loopback output shows as `(null)`.** Expected with recent ffmpeg. Identify
it by UID instead — it starts with `com.rogueamoeba.Loopback::`. The scripts
already do this.

**The audio test passes but ZEsarUX receives nothing.** Select the device as the
macOS input *before* enabling External Audio Source, then press `E` again in
that window. If ZEsarUX was already running when you changed the input, disable
and re-enable External Audio Source, or restart the emulator.

**Blocks decode intermittently.** If you are capturing the ZX→PC direction
through a virtual device rather than `--aofile`, this is the FIFO drop from
section 1 and it cannot be fixed by tuning. Use `--aofile`.

**Noise appears after about 20 minutes.** Loopback trial limitation. Toggle the
device off and on, or buy a licence.

**Isolating the emulator from the audio routing.** Both proof-of-concept
repositories can produce a WAV file instead of live audio, which ZEsarUX mounts
as *Input Real Tape*. That bypasses the virtual device entirely and separates
"the protocol is wrong" from "the audio routing is wrong":

```bash
./diagnostics/make-test-wav.sh "TEST MESSAGE"
```

---

## 7. What is not here yet

The terminal does not speak over audio yet. Present state:

| Piece | Status |
|---|---|
| Protocol v1 frame codec and slave state machine (C) | **done** — `src/alink_frame.c`, `src/alink_slave.c` |
| Protocol v1 master (Python) | **done** — `tools/alink/master.py` |
| Cross-implementation test | **done** — `test/test_alink_xcheck.py` |
| EAR decoder, MIC send loop (Z80) | PR B |
| Pulse encoder/decoder tuned for this link (Python) | PR B |
| `conn` backend, `make audio` / `make audio-zx` | PR B |
| pty wiring and the end-to-end runner | PR B |

Design and the reasoning behind the physical-layer choice:
[`docs/superpowers/specs/2026-08-02-audio-link-design.md`](superpowers/specs/2026-08-02-audio-link-design.md).

Expect roughly 77 B/s on text that does not scroll, and 32 B/s (Timex 80
columns) or 45 B/s (ZX 40 columns) on scrolling shell output — the terminal's
own rendering, not the audio, is the bottleneck for real traffic.
