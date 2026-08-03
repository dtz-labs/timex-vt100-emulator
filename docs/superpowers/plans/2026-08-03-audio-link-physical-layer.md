# Audio Link Physical Layer (PR B) Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Make the terminal speak protocol v1 over audio — our own EAR decoder and MIC send loop on the Z80, matching pulse encoder/decoder on the PC, wired in as a third `conn` backend.

**Architecture:** The Z80 never uses ROM tape routines (decision D1, option C in the design spec). It emits and decodes standard ROM *bit* timings behind a short 48-pulse preamble, which makes frames variable-length and keeps an idle keep-alive poll cheap. Frame reception runs with interrupts disabled; everything else runs with them on, so the existing IM2 keyboard scanner keeps working for free. Every test runs under ZEsarUX without a virtual audio device: upstream is captured with `--aofile`, downstream is injected with a generated WAV mounted as `--realtape`.

**Tech Stack:** C99 via z88dk (`zcc +zx -SO3 -clib=sdcc_iy`), inline `__asm` for the timing-critical loops, Python 3.9+ standard library, ZEsarUX 13.0, `z88dk-ticks` for cycle measurement.

## Global Constraints

From `docs/superpowers/specs/2026-08-02-audio-link-design.md` and this project's conventions. Every task's requirements implicitly include this section.

- **Physical layer is decision D1 option C:** our own EAR decoder and MIC send loop. **No ROM tape routines** — `CALL 0x0556` and `CALL 0x04C2` are forbidden, along with any entry into ROM internals. This is what keeps the backend working on a TS2068, whose ROM has none of those routines at those addresses.
- **Bit timings are the ROM's:** bit 0 = two half-pulses of 855 T, bit 1 = two half-pulses of 1710 T, MSB first. Turbo timings are explicitly out of scope.
- **Preamble:** 48 pilot pulses of 2,168 T (29.7 ms), then two sync pulses of 667 T and 735 T. The design spec said 15 ms; 48 pulses doubles it for detection margin, costing 15 ms per frame. Recomputed air times are in Task 1.
- **Frames are variable length, 4..68 bytes** — `CTRL, LEN, PAYLOAD, CRC-hi, CRC-lo`. The receiver reads `CTRL` and `LEN` first, then exactly `LEN + 2` more bytes. This is the whole reason for not using `LD-BYTES`, which needs the length before the block arrives.
- **Upstream transmissions carry a 32-byte zero leadout after the CRC.** Capture truncates the tail of a transmission by 8–16 bytes; without the leadout a truncated CRC turns every upstream frame into a retransmission.
- **Ports:** EAR is bit 6 of `IN A,(0xFE)`. MIC is bit 3 of `OUT (0xFE),A`; bits 0–2 are the border and bit 4 the speaker.
- **Interrupts:** enabled while idle, so the existing IM2 handler (`keyboard_im2_isr` → `keyboard_frame_tick` → `keymap_poll`) keeps filling `keybuf` at 50 Hz for free. `DI` only for the duration of one frame's reception or transmission, `EI` immediately after. The ISR does not chain to the ROM, so nothing else depends on it.
- **`src/alink_phy.c` is target-only** — absolute ports, cycle-counted loops. It follows the same rule as `blit_hires.c` / `blit_ula.c`: never linked into a host test, never listed in `test/run.sh`.
- **`src/alink_frame.c` and `src/alink_slave.c` are already written, host-tested and target-compiling.** Do not modify them. Their API is in `include/alink.h`.
- C99, four-space indent, K&R braces, `u8`/`u16` from `include/types.h`, pointer-oriented APIs, never return a struct by value. No float, no `malloc`, no recursion.
- **No virtual audio device in any test.** Upstream: `--aofile`. Downstream: a generated WAV via `--realtape`.

---

## File Structure

| File | Responsibility |
|---|---|
| `include/alink_phy.h` | PHY constants (pulse widths, preamble length) and the send/receive API |
| `src/alink_phy.c` | Target-only: MIC send loop, EAR preamble detector and bit decoder |
| `tools/alink/phy.py` | Pulse encoder and decoder for this link's format; WAV and raw-dump I/O |
| `tools/alink/main.py` | CLI runner: pty, ffmpeg, `--aofile` follower, the master loop |
| `src/conn.c` | Third `#ifdef CONN_BACKEND_AUDIO` branch, delegating to `alink_slave` |
| `Makefile` | `audio` / `audio-zx` targets, `AUDIO_SOURCES` |
| `test/test_alink_phy_py.py` | Python encoder/decoder round trip, including truncation |
| `test/alink_zesarux.py` | ZEsarUX harness for the two on-target directions |

---

### Task 1: Python pulse encoder and decoder

**Files:**
- Create: `tools/alink/phy.py`
- Create: `test/test_alink_phy_py.py`
- Modify: `Makefile` (add to `host-test`)

**Interfaces:**
- Consumes: `alink.frame.encode()` / `decode()` from PR A
- Produces:
  - `PILOT_T = 2168`, `SYNC1_T = 667`, `SYNC2_T = 735`, `ZERO_T = 855`, `ONE_T = 1710`
  - `PREAMBLE_PULSES = 48`, `LEADOUT_BYTES = 32`, `ZX_CLOCK_HZ = 3_500_000`
  - `encode_block(block: bytes, *, leadout: int = 0) -> list[int]` — returns pulse widths in T-states
  - `pulses_to_pcm(pulses, sample_rate=48000, amplitude=24000) -> bytes` — signed 16-bit LE
  - `write_wav(path, block, *, leadout=0, sample_rate=48000) -> None`
  - `class PulseDecoder` with `feed(samples: bytes, *, unsigned8: bool) -> list[bytes]` — returns complete blocks recovered so far
  - `decode_raw(data: bytes, *, unsigned8: bool) -> list[bytes]`

**Why the PoC decoder cannot be reused as is.** `TapeDecoder.PILOT_LOCK = 256` requires 256 stable half-pulses (~159 ms) before it enters the pilot state, and that lock also seeds its adaptive width and threshold tracking. Our preamble is 48 pulses. The adaptive DC-offset removal and hysteresis edge detection are worth keeping; the lock length and the fixed-64-byte frame assumption are not.

**Air times this format produces** (average bit mix, 2,565 T per bit):

| Frame | Bytes on the wire | Air time |
|---|---:|---:|
| Empty poll (`LEN=0`) | 4 | 0.030 + 0.023 = **0.053 s** |
| Full payload (`LEN=64`) | 68 | 0.030 + 0.399 = **0.429 s** |
| Full payload + leadout | 100 | 0.030 + 0.586 = **0.616 s** |

- [ ] **Step 1: Write the failing test**

Create `test/test_alink_phy_py.py`:

```python
#!/usr/bin/env python3
"""Host tests for the audio link's pulse encoder and decoder.

Standalone script, no pytest -- matches the other Python suites in this repo.

These tests never touch audio hardware. They encode a block to pulse widths,
render those to PCM, decode the PCM back, and compare. That is the whole
physical layer minus the Z80 and the speaker.
"""
import os
import sys

sys.path.insert(0, os.path.join(os.path.dirname(__file__), "..", "tools"))

from alink import frame, phy  # noqa: E402

checks = 0


def check(cond, what):
    global checks
    if not cond:
        raise AssertionError(what)
    checks += 1


def test_pulse_shape():
    pulses = phy.encode_block(bytes([0x00]))
    check(pulses[:phy.PREAMBLE_PULSES] == [phy.PILOT_T] * phy.PREAMBLE_PULSES,
          "preamble is PREAMBLE_PULSES pilot pulses")
    check(pulses[phy.PREAMBLE_PULSES] == phy.SYNC1_T, "first sync pulse")
    check(pulses[phy.PREAMBLE_PULSES + 1] == phy.SYNC2_T, "second sync pulse")
    body = pulses[phy.PREAMBLE_PULSES + 2:]
    check(len(body) == 16, "one byte is 8 bits x 2 half-pulses")
    check(all(w == phy.ZERO_T for w in body), "0x00 is all zero-width pulses")

    pulses = phy.encode_block(bytes([0xFF]))
    body = pulses[phy.PREAMBLE_PULSES + 2:]
    check(all(w == phy.ONE_T for w in body), "0xFF is all one-width pulses")


def test_msb_first():
    pulses = phy.encode_block(bytes([0x80]))
    body = pulses[phy.PREAMBLE_PULSES + 2:]
    check(body[0] == phy.ONE_T and body[1] == phy.ONE_T,
          "the most significant bit goes first")
    check(body[2] == phy.ZERO_T, "and the next bit is a zero")


def test_leadout_is_appended():
    plain = phy.encode_block(b"\x00")
    padded = phy.encode_block(b"\x00", leadout=phy.LEADOUT_BYTES)
    check(len(padded) - len(plain) == phy.LEADOUT_BYTES * 16,
          "leadout adds 32 zero bytes of pulses")


def test_round_trip_every_frame_length():
    for length in (0, 1, 2, 63, 64):
        payload = bytes((i * 5 + 3) & 0xFF for i in range(length))
        block = frame.encode(frame.LINK, 0, 1, payload)
        pcm = phy.pulses_to_pcm(phy.encode_block(block))
        got = phy.decode_raw(pcm, unsigned8=False)
        check(got == [block],
              f"len={length} round-trips, got {len(got)} block(s)")


def test_round_trip_unsigned8():
    """ZEsarUX's --aofile dump is unsigned 8-bit mono around a large DC
    offset, so the decoder must handle that representation too."""
    block = frame.encode(frame.LINK, 1, 0, b"hello")
    pcm16 = phy.pulses_to_pcm(phy.encode_block(block))
    # Convert to the aofile representation: unsigned 8-bit, small swing around
    # a large constant level, which is what the emulator actually writes.
    u8 = bytes((0xC0 + (int.from_bytes(pcm16[i:i + 2], "little", signed=True)
                        >> 12)) & 0xFF
               for i in range(0, len(pcm16), 2))
    check(phy.decode_raw(u8, unsigned8=True) == [block],
          "an unsigned-8 dump with a DC offset decodes")


def test_truncated_tail_is_absorbed_by_leadout():
    """Capture cuts 8-16 bytes off the end of a transmission. With the
    leadout, what is lost is padding; the frame itself survives."""
    block = frame.encode(frame.LINK, 0, 0, b"x" * 40)
    pcm = phy.pulses_to_pcm(phy.encode_block(block,
                                             leadout=phy.LEADOUT_BYTES))
    for cut_bytes in (8, 16):
        cut_samples = cut_bytes * 16 * 2 * 24     # generous over-estimate
        got = phy.decode_raw(pcm[:-cut_samples * 2], unsigned8=False)
        check(got == [block],
              f"frame survives a {cut_bytes}-byte tail truncation")


def test_back_to_back_blocks():
    a = frame.encode(frame.LINK, 0, 1, b"one")
    b = frame.encode(frame.LINK, 1, 1, b"two")
    pcm = phy.pulses_to_pcm(phy.encode_block(a) + phy.encode_block(b))
    check(phy.decode_raw(pcm, unsigned8=False) == [a, b],
          "two blocks in one stream both decode")


def test_noise_alone_yields_nothing():
    quiet = bytes(48000 * 2)                       # two seconds of silence
    check(phy.decode_raw(quiet, unsigned8=False) == [], "silence yields nothing")
    preamble_only = phy.pulses_to_pcm([phy.PILOT_T] * phy.PREAMBLE_PULSES)
    check(phy.decode_raw(preamble_only, unsigned8=False) == [],
          "a preamble with no frame behind it yields nothing")


def test_wav_round_trip(tmp="/tmp/alink_phy_test.wav"):
    import wave
    block = frame.encode(frame.HELLO, 0, 0,
                         bytes([frame.VERSION, frame.CAPS, 64]))
    phy.write_wav(tmp, block)
    with wave.open(tmp, "rb") as w:
        check(w.getnchannels() == 1, "mono")
        check(w.getsampwidth() == 2, "16-bit")
        check(w.getframerate() == 48000, "48 kHz")
        pcm = w.readframes(w.getnframes())
    check(phy.decode_raw(pcm, unsigned8=False) == [block],
          "the WAV decodes back to the same block")
    os.unlink(tmp)


def main():
    test_pulse_shape()
    test_msb_first()
    test_leadout_is_appended()
    test_round_trip_every_frame_length()
    test_round_trip_unsigned8()
    test_truncated_tail_is_absorbed_by_leadout()
    test_back_to_back_blocks()
    test_noise_alone_yields_nothing()
    test_wav_round_trip()
    print(f"alink_phy_py: {checks} checks passed")
    return 0


if __name__ == "__main__":
    sys.exit(main())
```

- [ ] **Step 2: Run the test to verify it fails**

Run: `python3 test/test_alink_phy_py.py`
Expected: FAIL with `ImportError: cannot import name 'phy'`.

- [ ] **Step 3: Implement `tools/alink/phy.py`**

Write the module to satisfy the interface above. Structure it as:

- `encode_block(block, *, leadout=0)` — emit `PREAMBLE_PULSES` pilot widths, then `SYNC1_T`, `SYNC2_T`, then for each byte MSB-first two identical half-pulse widths per bit, then `leadout` zero bytes the same way.
- `pulses_to_pcm(pulses, sample_rate, amplitude)` — walk the pulse list toggling level, accumulating exact fractional sample positions so rounding error does not drift (the PoC's `PulseWriter` does exactly this; copy that approach).
- `PulseDecoder` — port the PoC's `TapeDecoder` (`poc-zx-audio-link-from-zx-to-mac/zx_audio_link_tx.py`) with three changes: pilot lock after 16 stable pulses instead of 256, thresholds derived from the measured pilot width as it already does, and a frame reader that takes `CTRL`/`LEN` and then reads exactly `LEN + 2` more bytes instead of assuming 64.
- The decoder must not validate CRC — that is `alink.frame.decode()`'s job. It returns raw blocks.

- [ ] **Step 4: Run the test to verify it passes**

Run: `python3 test/test_alink_phy_py.py`
Expected: `alink_phy_py: <N> checks passed`, exit 0.

- [ ] **Step 5: Wire into `host-test` and commit**

Add `python3 test/test_alink_phy_py.py` to the `host-test` target in `Makefile`, after `test_alink_xcheck.py`.

```bash
make ci
git add tools/alink/phy.py test/test_alink_phy_py.py Makefile
git commit -m "alink: pulse encoder and decoder for the short-preamble format"
```

---

### Task 2: Z80 MIC send loop

**Files:**
- Create: `include/alink_phy.h`
- Create: `src/alink_phy.c`
- Create: `tools/bench/bench_alink_tx.c`
- Modify: `tools/bench.sh`

**Interfaces:**
- Consumes: `include/types.h`, `include/alink.h`
- Produces:
  - `void alink_phy_init(void)` — capture the current border colour for the port writes
  - `void alink_phy_send(const u8 *block, u8 n)` — emit preamble, sync, `n` bytes and the 32-byte leadout, with interrupts disabled for the duration
  - Constants `ALINK_PHY_PILOT_T`, `ALINK_PHY_SYNC1_T`, `ALINK_PHY_SYNC2_T`, `ALINK_PHY_ZERO_T`, `ALINK_PHY_ONE_T`, `ALINK_PHY_PREAMBLE_PULSES`, `ALINK_PHY_LEADOUT`

**The timing is measured, not assumed.** A pulse of `w` T-states means: set the MIC bit, then burn exactly `w` T-states including the loop overhead, then invert. Write the delay loop, measure one pulse of each width with `z88dk-ticks`, and adjust the loop constants until each is within 2% of its target. **Do not ship a guessed constant** — the decoder derives every later threshold from the measured pilot width, so a systematic error in the pilot rate is survivable but a mismatch between pilot and bit rates is not.

- [ ] **Step 1: Write the header**

Create `include/alink_phy.h` declaring the API and constants above, with a comment recording that this file is target-only and never host-compiled.

- [ ] **Step 2: Write the benchmark harness**

Create `tools/bench/bench_alink_tx.c` following the pattern of the existing harnesses in `tools/bench/`: a freestanding `main()` that calls `bench_mark_a()`, then `alink_phy_send()` with a 4-byte block, then `bench_mark_b()`.

- [ ] **Step 3: Implement the send loop**

Write `src/alink_phy.c` with `alink_phy_send()` as inline `__asm`. Shape:

```
di
; preamble: ALINK_PHY_PREAMBLE_PULSES pulses of PILOT_T
; sync: one pulse of SYNC1_T, one of SYNC2_T
; data: for each byte, MSB first, two pulses of ZERO_T or ONE_T per bit
; leadout: ALINK_PHY_LEADOUT zero bytes, same encoding
ei
```

Each pulse toggles bit 3 of port `0xFE` while preserving the border bits captured by `alink_phy_init()`.

- [ ] **Step 4: Measure and tune**

Add the harness to `tools/bench.sh` alongside the existing rows, then:

Run: `make bench`

Compute the per-pulse cost from the total and compare against the targets. Adjust the delay constants until every pulse width is within 2% of 2,168 / 667 / 735 / 855 / 1,710 T. Record the measured values in `docs/perf/benchmarks.md` under a new "Audio link PHY timings" section, with the same discipline as the other entries there: state the compiler build and show the arithmetic.

- [ ] **Step 5: Verify on target with `--aofile`**

Build a throwaway TAP that calls `alink_phy_send()` with a known block in a loop, run it under ZEsarUX with `--aofile`, and decode the dump with Task 1's decoder:

```bash
/Applications/ZEsarUX.app/Contents/MacOS/zesarux --noconfigfile --machine TC2048 \
    --tape build/txprobe.tap --fastautoload --nowelcomemessage \
    --no-saveconf-on-exit --aofile /tmp/alink-tx.raw
python3 -c "
import sys; sys.path.insert(0, 'tools')
from alink import phy, frame
blocks = phy.decode_raw(open('/tmp/alink-tx.raw','rb').read(), unsigned8=True)
print(len(blocks), 'blocks'); print([frame.decode(b) for b in blocks])
"
```
Expected: the known block decodes, repeatedly and identically.

- [ ] **Step 6: Commit**

```bash
git add include/alink_phy.h src/alink_phy.c tools/bench/bench_alink_tx.c tools/bench.sh docs/perf/benchmarks.md
git commit -m "alink: Z80 MIC send loop with measured pulse timings"
```

---

### Task 3: Z80 EAR receive

**Files:**
- Modify: `include/alink_phy.h` (add the receive API)
- Modify: `src/alink_phy.c`
- Create: `tools/bench/bench_alink_rx.c`
- Modify: `tools/bench.sh`

**Interfaces:**
- Consumes: Task 2's constants and `alink_phy_init()`
- Produces:
  - `u8 alink_phy_carrier(void)` — 1 if EAR is showing pilot-rate edges right now. Cheap, interrupt-safe, returns within a few hundred T-states. Called from the main loop while idle.
  - `u8 alink_phy_receive(u8 *block, u8 max)` — after `alink_phy_carrier()` says yes: lock the preamble, read the sync pair, then `CTRL`, `LEN` and `LEN + 2` more bytes. Returns the byte count, or 0 on any timing failure. Disables interrupts for its duration and re-enables on every exit path.

**The receive loop is the hardest code in the project and the only part that cannot be host-tested.** Budget accordingly. Two properties matter more than speed:

1. **Every exit path must `ei`.** A failure path that returns with interrupts disabled kills the keyboard permanently and looks like a hang.
2. **It must give up.** A carrier that stops mid-frame must not spin forever. Bound every edge wait with a counter sized from the longest legal pulse (one-bit half-pulse, 1,710 T) plus generous margin.

- [ ] **Step 1: Implement `alink_phy_carrier()`**

Sample EAR bit 6 repeatedly for a window of roughly two pilot pulses (~4,400 T) and count transitions. Return 1 if the count is consistent with the pilot rate, 0 otherwise. This runs with interrupts enabled; an IM2 tick landing inside the window can only cause a false negative, which the next poll corrects.

- [ ] **Step 2: Implement `alink_phy_receive()`**

Structure:

```
di
; lock: count pilot edges, measuring their width; require >= 16 consistent
; sync: wait for a pulse markedly shorter than the pilot (SYNC1_T), then SYNC2_T
; bytes: read CTRL, then LEN, then LEN+2 more; each bit is two half-pulses,
;        classified against a threshold derived from the measured pilot width
; on any timeout: ei, return 0
ei
return count
```

Classify a bit on the **sum of its two half-pulses** rather than one, exactly as the Python decoder does — it doubles the timing resolution for free.

- [ ] **Step 3: Verify on target with `--realtape`**

Generate a WAV with Task 1's encoder, build a throwaway TAP that receives one block and prints its bytes as hex, and run:

```bash
python3 -c "
import sys; sys.path.insert(0, 'tools')
from alink import phy, frame
phy.write_wav('/tmp/alink-rx.wav', frame.encode(frame.LINK, 0, 1, b'HELLO PHY'))
"
/Applications/ZEsarUX.app/Contents/MacOS/zesarux --noconfigfile --machine TC2048 \
    --tape build/rxprobe.tap --fastautoload --nowelcomemessage \
    --no-saveconf-on-exit --realtape /tmp/alink-rx.wav
```
Expected: the emulated screen shows the exact bytes of the block.

Repeat for payload lengths 0, 1, 63 and 64, and for a WAV whose tail is cut short — that one must return 0 rather than hang.

- [ ] **Step 4: Measure the idle cost**

Add `bench_alink_rx.c` measuring one `alink_phy_carrier()` call, and record it in `docs/perf/benchmarks.md`. It runs on every main-loop pass, so it must be cheap — under ~2,000 T. If it is not, the main loop's other work will make the terminal feel sluggish while idle.

- [ ] **Step 5: Commit**

```bash
git add include/alink_phy.h src/alink_phy.c tools/bench/bench_alink_rx.c tools/bench.sh docs/perf/benchmarks.md
git commit -m "alink: Z80 EAR carrier detector and frame receiver"
```

---

### Task 4: The `conn` backend

**Files:**
- Modify: `src/conn.c`
- Modify: `include/conn.h`
- Modify: `Makefile`

**Interfaces:**
- Consumes: `alink_slave_*` from PR A, `alink_phy_*` from Tasks 2–3, the existing `conn_ring_t` helpers in `conn.c`
- Produces: `CONN_BACKEND_AUDIO`, `CONN_STATUS_CARRIER_LOST`, `make audio`, `make audio-zx`

**The integration order is fixed by the protocol** and is documented in `include/alink.h`: feed, render, top up, respond. In `conn_poll()`:

```
if (!alink_phy_carrier()) return;
n = alink_phy_receive(block, sizeof block);
if (n == 0) return;
alink_slave_feed(&slave, block, n, &rx);
if (rx.deliver_len) ring_write(&rx_ring, alink_slave_rx_payload(&slave), rx.deliver_len);
if (rx.carrier_lost) conn_flags |= CONN_STATUS_CARRIER_LOST;
if (alink_slave_tx_ready(&slave) && tx_ring has bytes) alink_slave_tx_set(...);
if (rx.respond) { alink_slave_response(&slave, &resp);
                  alink_phy_send(buf, alink_frame_encode(&resp, buf)); }
```

Keyboard-buffer overflow (protocol v1 slave rule 5) belongs here, not in the slave engine: the 128-byte ring is `conn.c`'s. On overflow drop the **newest** bytes and raise a status bit so `main.c` rings BEL — never drop-oldest, which would corrupt the start of the command being typed rather than truncate it. `conn_tx_write()` already returns a short count on a full ring, so the drop-newest behaviour is inherited; the task is to surface it.

- [ ] **Step 1: Add the backend branch to `conn.c`**

Add a third `#ifdef CONN_BACKEND_AUDIO` branch implementing `conn_init()` and `conn_poll()` per the sketch above, delegating everything protocol-shaped to `alink_slave.c`. Keep it under about 60 lines; anything larger belongs in `alink_slave.c` or `alink_phy.c`.

- [ ] **Step 2: Add the status bit**

In `include/conn.h`, add `#define CONN_STATUS_CARRIER_LOST 0x10u` alongside the existing status bits.

- [ ] **Step 3: Add the build targets**

In `Makefile`, mirror the existing `IF1_DEFS` / `$(IF1_TAP)` pattern:

```make
AUDIO_TARGET ?= term-audio
ZX_AUDIO_TARGET ?= term-zx-audio
AUDIO_DEFS ?= -DCONN_BACKEND_AUDIO
AUDIO_SOURCES := src/alink_frame.c src/alink_slave.c src/alink_phy.c
```

`audio` builds `$(TIMEX_SOURCES) $(AUDIO_SOURCES)`; `audio-zx` builds `$(ZX_SOURCES) $(AUDIO_SOURCES)`. Both run `$(CHECK_IMAGE_LIMIT)` on the resulting map, exactly as `if1` does.

- [ ] **Step 4: Build and check the image limit**

Run: `make audio audio-zx`
Expected: both build; the gate passes. The Timex margin is the tight one — it was 1,761 bytes before this work. If the gate fails, compile out the ZRCP backend for these targets, which the audio backend replaces anyway.

- [ ] **Step 5: Commit**

```bash
git add src/conn.c include/conn.h Makefile
git commit -m "conn: audio backend selected by CONN_BACKEND_AUDIO"
```

---

### Task 5: The PC-side runner

**Files:**
- Create: `tools/alink/main.py`
- Modify: `Makefile` (a `run-audio` convenience target)

**Interfaces:**
- Consumes: `alink.frame`, `alink.master.Master`, `alink.phy`
- Produces:
  - `class AudioChannel(master.Channel)` — `send()` renders a block to PCM and pushes it at ffmpeg; `recv()` reads from the `--aofile` follower
  - `class AofileReader` — follows a growing raw dump, feeding `phy.PulseDecoder`
  - CLI: `python3 -m alink.main --aofile PATH [--device NAME] [--command CMD]`

The runner owns the pty: it spawns a shell, feeds `master.send()` from the pty's output, and writes `master.received` back into the pty. `Master.transact()` is called in a loop, at `T_POLL_IDLE` when nothing is moving and immediately when either direction carried a payload.

- [ ] **Step 1: Implement `AofileReader` and `AudioChannel`**

- [ ] **Step 2: Implement the pty loop and CLI**

- [ ] **Step 3: Verify against a WAV, without the emulator**

Prove the runner's own plumbing before involving audio hardware: point `AofileReader` at a file you generate with `phy.write_wav()` converted to a raw dump, and confirm the master reaches LINKED against a Python `ModelSlave`.

- [ ] **Step 4: Commit**

```bash
git add tools/alink/main.py Makefile
git commit -m "alink: PC-side runner with pty, ffmpeg and aofile plumbing"
```

---

### Task 6: End-to-end under ZEsarUX

**Files:**
- Create: `test/alink_zesarux.py`
- Modify: `Makefile` (`smoke-audio` target)
- Modify: `docs/audio-link-setup.md`, `README.md`, `docs/superpowers/specs/2026-08-02-audio-link-design.md`

- [ ] **Step 1: Write the harness**

`test/alink_zesarux.py` starts ZEsarUX with `--aofile` and a generated `--realtape` WAV carrying a `HELLO`, waits for the terminal's `WELCOME` to appear in the dump, and asserts the handshake completed. Then it injects a `LINK` frame carrying printable text and asserts the text appears on screen, read back over ZRCP the way `test/zesarux_smoke.py` already does.

- [ ] **Step 2: Add `smoke-audio` to the Makefile**, running the harness for both `term-audio.tap` and `term-zx-audio.tap`.

- [ ] **Step 3: Run it**

Run: `make smoke-audio`
Expected: handshake and text delivery pass on both builds.

- [ ] **Step 4: Correct the design spec**

Two statements in `docs/superpowers/specs/2026-08-02-audio-link-design.md` are wrong and must be fixed rather than left standing:

- D2 says the idle loop "alternates between watching EAR for a preamble and scanning the keyboard". It does not. The keyboard is scanned by the IM2 handler at 50 Hz (`keyboard_im2_isr` → `keyboard_frame_tick` → `keymap_poll`), independently of the main loop, so the idle loop only watches EAR and interrupts simply stay enabled.
- The timing table's `t_dn` / `t_up` assume a 15 ms preamble. This plan uses 48 pulses (29.7 ms) for detection margin, so a full frame is 0.429 s and an empty poll 0.053 s. Recompute the derived throughput figures and idle-deafness percentage from the measured values, and say they are measured.

- [ ] **Step 5: Document how to run it**

Fill in the "What is not here yet" section of `docs/audio-link-setup.md` with the actual run instructions, and update the README's Audio Link section to drop "(in progress)".

- [ ] **Step 6: Commit**

```bash
git add test/alink_zesarux.py Makefile docs README.md
git commit -m "alink: end-to-end verification under ZEsarUX, and doc corrections"
```

---

## Self-Review

**1. Spec coverage.** D1 option C → Tasks 2–3. D2 (carrier detection) → Task 3 Step 1, with the spec's own error corrected in Task 6. D5 (both targets) → Task 4 Step 3. D6/D7 (timing, throughput) → measured in Tasks 2–3, spec corrected in Task 6. The 32-byte leadout → Task 1 and Task 2. `phy.py` rework → Task 1. Keyboard overflow with BEL → Task 4. pty wiring → Task 5.

**2. Placeholder scan.** Tasks 1 and 4 carry complete test and structure code. Tasks 2, 3 and 5 specify structure, interfaces and acceptance criteria but not line-by-line assembly — deliberately, and stated as such: the pulse loop's delay constants are **measured outputs**, not inputs, and writing invented constants into a plan is precisely how the ROM-pilot error happened once already in this feature. Each of those tasks names the measurement that decides correctness.

**3. Type consistency.** `alink_phy_send(const u8 *, u8)`, `alink_phy_receive(u8 *, u8)`, `alink_phy_carrier(void)` are used identically in Tasks 2, 3 and 4. Python `encode_block` / `decode_raw` / `write_wav` / `PulseDecoder` signatures match between Tasks 1, 2, 3 and 5. Constant names are `ALINK_PHY_*` in C and bare in `phy.py`, consistently.

**Known risk, stated rather than hidden.** Task 3 is the only part of this project with no host-testable form. Its correctness rests on ZEsarUX plus, eventually, real hardware. If the receive loop resists tuning, the fallback is not "try harder": it is to revisit D1 with the measured evidence, since options A and B remain documented and costed in the design spec.
