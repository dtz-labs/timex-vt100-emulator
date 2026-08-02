# Bidirectional audio link (ZX ↔ PC) — Design

Date: 2026-08-02
Status: approved design, implementation pending
Issue: #5

## Purpose

Give the terminal a third `conn` backend that carries its two byte streams
over audio instead of over Interface 1 RS-232 or the ZRCP poke buffer. The
PC runs a shell in a pty and drives the link; the ZX or Timex renders what
arrives and sends keystrokes back. Under ZEsarUX the audio path is virtual;
on real hardware the same code drives the EAR and MIC sockets.

The wire protocol is not invented here. It is the approved
[Half-Duplex Terminal Protocol v1](https://github.com/dtz-labs/zx-audio-link)
from `dtz-labs/zx-audio-link`, implemented in full for the first time — the
spec exists but has no implementation on either side.

## Prior art and what it actually provides

Three repositories were read before this design:

| Repository | What it contains | What we reuse |
|---|---|---|
| `zx-audio-link` | Protocol v1 spec only — no code | The entire wire protocol |
| `poc-zx-audio-link-from-mac-to-zx` | Python pulse **encoder**, ZEsarUX launcher, Loopback recipe | Encoder, audio plumbing |
| `poc-zx-audio-link-from-zx-to-mac` | Python pulse **decoder** (adaptive, sample-rate independent), `--aofile` capture | Decoder, capture method |

**Neither proof of concept contains reusable Z80 code.** The Mac→ZX receiver
is 37 bytes (`LD IX,buffer` / `CALL 0x0556` / print); the ZX→Mac sender is a
keyboard loop plus `CALL 0x04C2`. All the engineering in both PoCs is on the
Python side. The Z80 slave in this project is written from scratch.

Two findings from the PoCs are load-bearing here and are adopted without
re-litigation:

- **ZEsarUX realtime audio output drops samples.** Its CoreAudio FIFO
  overflows and loses milliseconds, which destroys tape data. The capture
  path must be `--aofile`, a raw dump written synchronously with emulation.
- **The Mac→ZX direction works through a Loopback virtual device** named
  `ZX Link` feeding ZEsarUX's External Audio Source.

## Decisions

### D1: Short pilot downstream, custom send loop upstream

The ROM tape format costs 3,223 pilot pulses × 2,168 T = 1.99 s per block
before a single data bit moves. Round trip with ROM timings on both sides is
~5 s, i.e. ~13 B/s.

The receive side does not need that pilot: the ROM's `LD-LEADER` locks after
256 pilot pulses. Shortening the generated pilot to 400 pulses (0.23 s) is a
constant change in the Python encoder and requires no Z80 work beyond carrier
detection (D2).

The transmit side cannot be shortened the same way — `SA-BYTES` has its pilot
length fixed in ROM. But transmitting needs no timing recovery, so a custom
`OUT`-with-delays loop (~50 bytes of Z80) emits a 15 ms preamble instead.

| Direction | Method | Air time (68-byte frame) |
|---|---|---:|
| PC → ZX | ROM `LD-BYTES`, 400-pulse pilot | 0.23 s + 0.40 s = **0.65 s** |
| ZX → PC | custom MIC loop, 15 ms preamble | 0.02 s + 0.40 s = **0.42 s** |

Rejected: full turbo encoding (~4,000 baud) on both sides. It would reach
~400 B/s but requires a cycle-counted EAR decoder on the Z80 — the hardest
code in the project — and on real hardware would likely need per-deck tuning.
Deferred to a possible v2; the protocol does not change if the physical layer
is later replaced.

### D2: Carrier detection is mandatory, not an optimisation

`LD-BYTES` blocks indefinitely waiting for a pilot and executes `DI`. Calling
it from the main loop would leave the machine permanently deaf to the
keyboard, because IM2 keyboard scanning stops for the duration.

A ~30-byte carrier detector polls port `0xFE` bit 6 for edges at pilot rate
and only then hands control to `LD-BYTES`. While idle, the keyboard is
scanned normally.

This is why the 400-pulse pilot is 400 and not 256: the detector needs a few
milliseconds to recognise the carrier before the ROM's own 256-pulse lock
begins.

Consequence worth stating plainly: an empty poll frame (`LEN=0`) is 4 bytes,
so it costs 0.23 s of pilot plus 0.02 s of data. On an idle link the machine
is deaf for ~0.25 s per second and responsive for the rest. Only frames
carrying a real 64-byte payload cost the full 0.65 s, and during those the
user is reading arriving text rather than typing.

### D3: Full protocol v1, no reduced first cut

Framing with CRC-16, `HELLO`/`WELCOME` negotiation, stop-and-wait ARQ with
`SEQ`/`ACK`, keep-alive, dead-link detection and automatic reconnection — all
of it, as specified.

Rejected: a bare block pipe first. A lost frame would corrupt the screen with
no recovery, and bolting ARQ on afterwards touches the same code anyway.
Rejected: v1 without `HELLO`/`WELCOME`. It saves a few hundred bytes but
leaves no clean session reset when either side restarts, which then has to be
replaced by something else.

### D4: Delivered as two pull requests

- **PR A — protocol engine, no audio.** Frame codec and slave state machine
  in C (host-testable), master in Python, tested against each other over a
  pipe. No Z80, no emulator, no sound.
- **PR B — physical layer and integration.** Carrier detector, `LD-BYTES`
  wrapper, MIC send loop, Python encoder/decoder tuning, `conn` backend,
  ZEsarUX tooling, end-to-end test.

Reason: PR A is deterministic and host-testable; PR B needs an emulator and
signal analysis. Combined, a failing end-to-end run would not say whether the
protocol or the signal was at fault.

### D5: Both targets get the backend

`CONN_BACKEND_AUDIO` is selected the same way `CONN_BACKEND_IF1` already is,
for both the Timex 80-column and ZX 40-column builds (`make audio`,
`make audio-zx`). The ZRCP poke-buffer backend stays as the test build.

### D6: Timing constants derived from this project's measurements

The protocol spec gives formulas and states that concrete values must come
from the real physical layer. Instantiated from `docs/perf/benchmarks.md`:

| Constant | Side | Value | Derivation |
|---|---|---:|---|
| `t_dn` | — | 0.65 s | 400 × 2,168 T pilot + 68 B data |
| `t_up` | — | 0.42 s | 15 ms preamble + 68 B data |
| `t_render` | — | 14.3 s | 64 scrolls × 781,607 T (Timex worst case) |
| `T_RESP` | PC | **16 s** | `t_turn + t_render + t_up + margin` |
| `RETRIES` | PC | 3 | from spec |
| `T_POLL_ACTIVE` | PC | 0 ms | from spec |
| `T_POLL_IDLE` | PC | 1.0 s | from spec |
| `T_HELLO_RETRY` | PC | 2.0 s | from spec |
| `T_DEAD` | ZX | **55 s** | `> RETRIES × (t_dn + T_RESP)` = 50 s |

`T_RESP` of 16 s costs nothing on a healthy link — it is a timeout, not a
delay, and the master proceeds the moment a response arrives. It is paid only
for a genuinely lost frame, which is rare in the emulator. The real cost is
`T_DEAD`: after the link physically breaks, "NO CARRIER" appears only after
about a minute.

`t_render` is measured, not estimated: one scrolled row costs 781,607 T on
the Timex hi-res build (`scroll_model` 101,508 T + `scroll_vram` 680,099 T)
and 381,051 T on the ZX ULA build (53,808 T + 327,243 T). A 64-byte payload
of newlines forces 64 scrolls.

### D7: Throughput is bounded by scrolling, not by audio

The protocol requires the slave to consume a payload before replying — that
is the flow-control mechanism, not an accident — so rendering time enters
transaction time directly.

| Traffic | Effective downstream rate |
|---|---:|
| Text that does not scroll | ~100 B/s |
| Scrolling output, Timex 80 col. | ~20 B/s |
| Scrolling output, ZX 40 col. | ~35 B/s |

A 64-byte chunk of ordinary shell output carries 8–10 newlines, costing about
2.2 s of rendering on the Timex. The audio layer is not the bottleneck for
real traffic; `blit_scroll_region()` is.

Recommendation 5 of `docs/superpowers/reviews/2026-07-26-perf-review.md`
("hoist video text-row addressing, handle both files together") was never
implemented — `scroll_vram` still measures 680,099 T, unchanged. Landing it
is worth roughly 2× the link's throughput on scrolling traffic and is cheaper
than any work in the physical layer. **It is a prerequisite for this
feature's usefulness and is tracked separately, not inside PR A or PR B.**

## Architecture

```
PC (master)                                      ZX / Timex (slave)
┌────────────────────────────┐                  ┌─────────────────────────────┐
│ pty (shell)                │                  │ vtparse → screen → blit     │
│        ↕                   │                  │        ↕                    │
│ master ARQ (stop-and-wait) │                  │ slave ARQ (stop-and-wait)   │
│        ↕                   │                  │        ↕                    │
│ frame codec + CRC-16       │                  │ frame codec + CRC-16        │
│        ↕                   │                  │        ↕                    │
│ pulse encoder ─────────────┼──── EAR ─────────┤ carrier detect → LD-BYTES   │
│ pulse decoder ←────────────┼──── MIC ─────────┤ MIC send loop               │
└────────────────────────────┘                  └─────────────────────────────┘
        ↑ WAV → Loopback "ZX Link" → ffmpeg → ZEsarUX External Audio Source
        ↑ ZEsarUX --aofile → decode-follow
```

The ROM performs the hardest operation in the system — timing recovery on
receive. Our Z80 code never decodes pulses; it only detects that something is
arriving and yields.

## Components

### Z80 side

| File | Responsibility | Host-testable |
|---|---|---|
| `include/alink.h` | frame layout, timing constants, API | — |
| `src/alink_frame.c` | frame encode/decode, CRC-16/CCITT-FALSE | **yes** |
| `src/alink_slave.c` | LISTEN/LINKED state machine, SEQ/ACK, dedupe | **yes** |
| `src/alink_phy.c` | carrier detect, `LD-BYTES` call, MIC send loop | no |
| `test/test_alink_frame.c` | codec and CRC tests | — |
| `test/test_alink_slave.c` | state-machine tests | — |

`alink_phy.c` is target-only and follows the same rule as `blit_hires.c` /
`blit_ula.c`: absolute hardware access, never linked into a host test.

`conn.c` gains a third, thin `#ifdef CONN_BACKEND_AUDIO` branch that only
delegates to `alink_slave.c` — about 20 lines. The bulk stays in the `alink_*`
files rather than growing a file that already carries two backends.

### PC side

| File | Responsibility |
|---|---|
| `tools/alink/frame.py` | frame codec + CRC-16, mirror of `alink_frame.c` |
| `tools/alink/master.py` | master ARQ state machine, pty integration |
| `tools/alink/phy.py` | pulse encoder (short pilot) and decoder, from the PoCs |
| `tools/alink/main.py` | CLI: run the link, follow `--aofile`, drive ffmpeg |

### Memory budget

Estimated ~1.2 KB of code plus ~140 B of data. Available margin is 5,349 B
on the Timex build and 8,082 B on the ZX build, and the audio backend
*replaces* the ZRCP backend rather than adding to it. `tools/check_image_limit.py`
enforces the limit at build time, as it already does for the other backends.

## Data flow

1. The master reads pty output and forms a `LINK` frame with up to 64 bytes.
2. The encoder renders the frame as pulses with a 400-pulse pilot; ffmpeg
   streams it to the `ZX Link` Loopback device; ZEsarUX feeds it to EAR.
3. The slave's carrier detector sees pilot edges and calls `LD-BYTES`, which
   decodes the block into a frame buffer with interrupts disabled.
4. The slave validates the frame, delivers the payload to `vtparse`, renders
   it, then builds its response carrying any queued keystrokes.
5. The MIC send loop emits the response with a 15 ms preamble. ZEsarUX writes
   every sample to `--aofile`.
6. The decoder follows that file, recovers the frame, and the master delivers
   the keystrokes to the pty.

## Error handling

All of it comes from protocol v1 and nothing is invented here. A block is a
valid frame only if its length equals `LEN`+4, its CRC matches and `LEN` ≤ 64;
anything else is treated as if it never arrived. Duplicates are recognised by
the `SEQ` bit, answered, but not delivered twice. A valid `HELLO` in any state
resets the session. Keyboard-buffer overflow drops the **newest** bytes and
rings BEL, never the oldest, so the start of a command being typed survives.

One hazard is specific to this project: `LD-BYTES` executes `DI`, so IM2
keyboard scanning stops while a frame is being received. The carrier detector
(D2) exists to confine that window to the moments when a signal is actually
present.

## Testing strategy

- **Host, C:** CRC-16 vectors, frame round trips, truncated blocks, flipped
  bits, `LEN` disagreeing with block length. The slave state machine is driven
  by injected frames that are dropped, duplicated and corrupted; the assertion
  is exactly-once, in-order delivery.
- **Python:** the master over a simulated lossy channel, including the
  regression cases the protocol spec names — an idle link polled forever must
  never flap, a `WELCOME` payload must never reach the pty, and a keyboard
  chunk pending during a retransmit storm must survive un-duplicated.
- **Cross-implementation:** the Python master against the C slave compiled
  natively, over a pipe. This test is not in the protocol spec and is the most
  valuable one here: it catches divergent readings of the spec between two
  independent implementations before audio is involved.
- **End-to-end:** ZEsarUX with the PoC audio path, both TAPs.

## Out of scope

- Turbo physical layer (see D1).
- Real hardware validation. The code uses standard tape timings and ROM
  routines, so it should drive real EAR/MIC sockets, but v1 is validated only
  under ZEsarUX.
- File transfer, compression, encryption. The link presents a transparent
  byte pipe, so XMODEM and friends could later run over it unchanged.
- Replacing the ZRCP test backend (D5).
