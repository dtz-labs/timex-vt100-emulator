# Bidirectional audio link (ZX ↔ PC) — Design

Date: 2026-08-02
Status: approved design, implementation pending. D1 decided 2026-08-03:
**Option C**, our own EAR decoder at ROM bit timings.
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
spec exists there but has no implementation on either side.

## Prior art and what it actually provides

| Repository | What it contains | What we reuse |
|---|---|---|
| `zx-audio-link` | Protocol v1 spec only — no code | The entire wire protocol |
| `poc-zx-audio-link-from-mac-to-zx` | Python pulse **encoder**, ZEsarUX launcher, Loopback recipe | Encoder, audio plumbing |
| `poc-zx-audio-link-from-zx-to-mac` | Python pulse **decoder** (adaptive, sample-rate independent), `--aofile` capture | Decoder, capture method |

**Neither proof of concept contains reusable Z80 code.** The Mac→ZX receiver
is 37 bytes (`code_len = 0x25`: `LD IX,buffer` / `CALL 0x0556` / print); the
ZX→Mac sender is a keyboard loop plus `CALL 0x04C2`. All the engineering in
both PoCs is on the Python side. The Z80 slave is written from scratch.

Three findings from the PoCs are load-bearing here and are adopted without
re-litigation:

- **ZEsarUX realtime audio output drops samples.** Its CoreAudio FIFO
  overflows and loses milliseconds, destroying tape data. The capture path
  must be `--aofile`, a raw dump written synchronously with emulation.
- **The Mac→ZX direction works** through the PoC pipeline: encoder → ffmpeg →
  Loopback virtual device named `ZX Link` → ZEsarUX External Audio Source.
- **Capture truncates the tail of a transmission** by 8–16 bytes, so the ZX→Mac
  PoC pads every transmission with `LEADOUT_SIZE = 32` zero bytes
  (`zx_audio_link_tx.py:30–33, 263–275`).

  **Measured and not adopted.** This is the one PoC finding that did not
  survive contact with our own capture: 98/98 frames decoded with a leadout
  and 234/234 without it, from real `--aofile` dumps of the Z80 transmitter.
  Nothing truncates on this path — the PoC's observation belongs to its
  earlier Loopback attempts. Our frames also end by **length** rather than by
  silence, so the frame is complete before a leadout would even begin. See D6.

## D1 (DECIDED): the downstream physical layer

**Decision (2026-08-03): Option C — our own EAR decoder at ROM bit timings.**
The reasoning that led here is kept in full below, because the first revision
of this document reached the opposite conclusion from a fabricated number and
that failure is worth keeping visible.

### Why this was reopened

An earlier revision of this design claimed the ROM's `LD-LEADER` locks after
**256 pilot pulses**, and concluded that shortening the generated pilot from
3,223 to 400 pulses was a Python-side constant change requiring no Z80 work.
**That claim was false and the conclusion built on it does not stand.**

The 256 was transplanted from the PoC's *Python decoder*
(`TapeDecoder.PILOT_LOCK = 256`, `zx_audio_link_tx.py:299`) and attributed to
the ROM. Disassembling the actual 48K ROM
(`/Applications/ZEsarUX.app/Contents/Resources/48.rom`) shows otherwise:

```
0571  21 15 04   LD HL,0x0415
0574  10 FE      LD-WAIT: DJNZ LD-WAIT      ; inner loop, 256 x 13 T
0576  2B 7C B5   DEC HL / LD A,H / OR L
0579  20 F9      JR NZ,LD-WAIT              ; outer loop, 1045 times
057B  CD E3 05   CALL LD-EDGE-2
0580  06 9C      LD-LEADER: LD B,0x9C
0582  CD E3 05   CALL LD-EDGE-2             ; consumes TWO edges
058C  24 20 F1   INC H / JR NZ,LD-LEADER    ; 256 iterations
```

Two consequences, both verified against the ROM bytes:

1. `LD-WAIT` is a nested loop that deliberately **ignores the first ~1.00 s
   of pilot** (1045 × 256 × 13 T ≈ 3.5 M T).
2. `LD-LEADER` then needs `H` to wrap: 256 iterations of `LD-EDGE-2`, each
   measuring **two** edges — **512 pilot pulses** (0.317 s), not 256.

Minimum viable pilot for an unmodified `CALL 0x0556` is therefore **≈1.32 s,
about 2,130 pulses** — not 400. The ROM's stock 3,223-pulse pilot is only
~1.5× that minimum, so shortening it saves far less than claimed.

A second constraint compounds this. `LD-BYTES` must be told the expected byte
count in `DE` **before** the block arrives, compares a leading flag byte
against `A'`, and verifies a trailing XOR checksum only once `DE` is
exhausted; a shorter block leaves it waiting for edges that never come and it
exits through its error return. A protocol frame is 4–68 bytes, so
variable-length frames cannot be received this way: every downstream block
must be padded to a **fixed 68-byte payload** (plus the ROM's flag byte and
XOR checksum = 70 bytes on the wire). **An empty keep-alive poll therefore
costs exactly as much air time as a full 64-byte payload** — which destroys
the "empty polls are cheap" argument the keyboard-liveness case rested on.

### The three options, with honest numbers

Bit timings are the ROM's throughout (bit 0 = 2 × 855 T, bit 1 = 2 × 1710 T;
2,565 T average, 3,420 T all-ones worst case). Air times below are averages;
the all-ones worst case is ~33% higher and is absorbed by `T_RESP`.

| | **A: stock `CALL 0x0556`** | **B: enter past `LD-WAIT`** | **C: own EAR decoder** |
|---|---|---|---|
| Pilot / preamble | 2,300 pulses = 1.425 s | 700 pulses = 0.434 s | 48 pulses = 29.7 ms |
| Frame on the wire | fixed 70 B = 0.410 s | fixed 70 B = 0.410 s | variable, 4–68 B |
| `t_dn`, full payload | **1.835 s** | **0.844 s** | **0.427 s** |
| `t_dn`, empty poll | 1.835 s | 0.844 s | **0.051 s** |
| Transaction, no render | 2.25 s | 1.26 s | **0.48 s** |
| Throughput, no scrolling | 28 B/s | 51 B/s | **134 B/s** |
| Idle deafness | ~184% | 84% | **10%** |
| Z80 code | ~30 B | ~50 B | ~300 B |
| Machines | standard Sinclair ROM only | standard Sinclair ROM only, **and its internal addresses** | **any** |

**Option A** keeps the ROM entry point untouched. It is the least code and
the least risk, and it is unusable as a terminal: at 28 B/s, with the machine
deaf for longer than the poll interval, keystrokes are dropped rather than
delayed.

**Option B** replicates `LD-BYTES`' own preamble (`DI`, `OUT (0xFE)`, build
`C`, push `SA/LD-RET`) in ~20 bytes and jumps in at `0x057B`, skipping the
one-second wait. It roughly halves transaction time but couples the build to
ROM *internal* addresses (`0x057B`, `0x0580`, `LD-EDGE-2` at `0x05E3`,
`SA/LD-RET` at `0x053F`), and still leaves the machine deaf 84% of an idle
second.

**Option C** decodes EAR ourselves and uses the ROM for nothing. It is the
only option that can read a variable-length frame, so an idle poll costs
0.051 s instead of 0.844 s — which is what makes the keyboard usable at all.
It is also the only one that works on a TS2068, whose Timex ROM has none of
these routines at these addresses.

### Recommendation: Option C, at ROM bit timings

Option C was presented earlier as "maximum speed (~400 B/s)" and declined —
but that framing conflated two independent choices, and the conflation was
mine. Writing our own decoder does **not** require turbo bit timings. At the
ROM's own 855/1710 T half-pulses the decoder must distinguish 244 µs from
489 µs, which a straightforward edge-timing loop does comfortably; the
cycle-counted precision that makes turbo loaders hard is not needed here.
Turbo timings remain available later as a pure physical-layer change with no
protocol impact.

The recommendation is therefore: **own decoder, ROM bit rate.** ~300 bytes of
Z80, 134 B/s, a responsive keyboard and no ROM dependence — against ~50 bytes,
51 B/s, a keyboard deaf most of the time, and a build pinned to one ROM.

This reversed an earlier answer that had been given on false information, so
it was put back to an explicit decision rather than resolved unilaterally.
**Accepted 2026-08-03.**

Everything below is written against Option C.

### The wire format, as implemented

```
[48 x pilot 2168 T] [sync 667 T] [sync 735 T] [CTRL LEN PAYLOAD CRC] [leadout] [closing edge]
```

Data is MSB first, one bit per pair of equal half-pulses: 855 T for a 0, 1710 T
for a 1 — the ROM's own bit timings, which is what keeps the Z80 side a simple
edge-timing loop rather than a cycle-counted turbo decoder.

Three details emerged from implementing the codec (PR B Task 1) rather than
from designing it, and all three are consequences of shortening the preamble
by a factor of 67. They are recorded here because they bind **both** sides:

1. **A frame ends by length, not by silence.** Once `LEN` is decoded the
   receiver knows exactly how many bytes remain. The proof of concept cut on a
   gap, which works for its fixed 64-byte payload but would never separate two
   back-to-back blocks, since the second block's preamble follows the first
   immediately.
2. **The transmitter must emit a closing edge** after its last half-pulse. A
   receiver measures a half-pulse *between two edges*, so the final one has no
   width until something ends it — without the closing edge the last bit never
   completes and the frame is never delivered.
3. **The receiver's DC estimate must settle inside the preamble.** The proof
   of concept tracked DC with a 41 ms time constant, having 1.99 s of pilot to
   converge in. At 29.7 ms that estimate is still far off when data starts, and
   on an 8-bit dump with a large DC offset both halves of the signal then land
   on the same side of the threshold and no edge is ever seen.

## Decisions (settled)

### D2: Carrier detection, and what actually keeps the keyboard alive

Under A or B, `LD-BYTES` blocks indefinitely and executes `DI`, so a ~30-byte
carrier detector (poll port `0xFE` bit 6 for edges at pilot rate) is
mandatory just to keep the keyboard alive between frames.

Under C the detector is a cheap call in the main loop, and the deaf window
shrinks to the frame's own air time.

**Correction (2026-08-03).** An earlier revision said the idle loop
"alternates between watching EAR for a preamble and scanning the keyboard".
It does not, and it does not need to. The keyboard is scanned inside the IM2
interrupt handler at 50 Hz — `keyboard_im2_isr` → `keyboard_frame_tick` →
`keymap_poll` → `keybuf` (`src/main.c`) — entirely independently of the main
loop, which only drains `keybuf` into `conn`. So while idle the design simply
leaves interrupts **enabled** and the keyboard costs nothing.

`DI` is needed only for the duration of one frame's reception or
transmission, because sampling EAR cannot be interleaved with anything
without destroying bit timing. That handler does not chain to the ROM, so
nothing else depends on the interrupts missed during a frame.

### D3: Full protocol v1, no reduced first cut

Framing with CRC-16, `HELLO`/`WELCOME` negotiation, stop-and-wait ARQ with
`SEQ`/`ACK`, keep-alive, dead-link detection and automatic reconnection — all
of it, as specified.

Rejected: a bare block pipe first (a lost frame would corrupt the screen with
no recovery, and bolting ARQ on afterwards touches the same code anyway), and
v1 without `HELLO`/`WELCOME` (saves a few hundred bytes but leaves no clean
session reset, which then has to be replaced by something else).

### D4: Delivered as two pull requests

- **PR A — protocol engine, no audio.** Frame codec and slave state machine
  in C (host-testable), master in Python, tested against each other over a
  pipe. No Z80, no emulator, no sound.
- **PR B — physical layer and integration.** EAR decoder, MIC send loop,
  Python encoder/decoder tuning, `conn` backend, ZEsarUX tooling,
  end-to-end test.

PR A is deterministic and host-testable; PR B needs an emulator and signal
analysis. Combined, a failing end-to-end run would not say whether the
protocol or the signal was at fault.

**D1 only affects PR B**, so PR A stayed plannable while it was open.

### D5: Both targets get the backend

`CONN_BACKEND_AUDIO` is selected the same way `CONN_BACKEND_IF1` already is,
for both the Timex 80-column and ZX 40-column builds (`make audio`,
`make audio-zx`). The ZRCP poke-buffer backend stays as the test build.

### D6: Timing constants

Derived from this project's measured benchmarks, assuming Option C and
assuming **PR #9 (scroll run coalescing) has landed** — it changes `t_render`
by a third.

**These are measured, not estimated** — computed by summing the pulse widths
`tools/alink/phy.py` actually emits, and pinned by a test
(`test_air_time_matches_the_design`). An earlier revision assumed a 15 ms
preamble; the implementation uses 48 pilot pulses (29.7 ms) for detection
margin, plus a two-pulse sync pair and a closing edge.

| Constant | Side | Value | Derivation |
|---|---|---:|---|
| `t_dn` full / poll | — | **0.427 / 0.051 s** | 48-pulse preamble + sync + 68 / 4 B + closing edge |
| `t_up` full / poll | — | **0.427 / 0.051 s** | same encoding; leadout measured unnecessary |
| `t_render` | — | 8.35 s | 64 scrolls × 456,411 T (Timex worst case) |
| `T_RESP` | PC | **10 s** | `t_turn + t_render + t_up + margin` |
| `RETRIES` | PC | 3 | from spec |
| `T_POLL_ACTIVE` | PC | 0 ms | from spec |
| `T_POLL_IDLE` | PC | 1.0 s | from spec |
| `T_HELLO_RETRY` | PC | 2.0 s | from spec |
| `T_DEAD` | ZX | **35 s** | `> RETRIES × (t_dn + T_RESP)` = 31.3 s |

**The leadout was measured and dropped (2026-08-03).** It cost 0.125 s on
every upstream frame — 21% of a transaction, and the single largest avoidable
cost in the link.

Measured against real `--aofile` captures from the Z80 transmitter:
**98/98 frames decoded with the leadout, 234/234 without it**, in the same
amount of audio. Nothing truncates on this path. The proof of concept's
8–16-byte truncation belongs to its earlier Loopback attempts, not to the
`--aofile` dump — and our frames end by **length** rather than by silence, so
a decoder stops needing input the moment `LEN + 4` bytes have arrived. The
leadout could not have helped even if something did truncate.

`ALINK_PHY_LEADOUT` is 0. The constant remains so it can be raised if
real-hardware capture, as opposed to the emulator dump, ever proves to
truncate.

With it gone, `t_up` equals `t_dn`: **0.427 s** for a full frame and
**0.051 s** for an empty poll. Downstream throughput is **134 B/s** and idle
deafness **10%**.

### D7: Throughput is bounded by scrolling, not by audio

The protocol requires the slave to consume a payload before replying — that
is the flow-control mechanism, not an accident — so rendering time enters
transaction time directly.

| Traffic | Timex 80 col. | ZX 40 col. |
|---|---:|---:|
| Text that does not scroll | **134 B/s** | 134 B/s |
| Scrolling output (9 newlines per 64 B chunk) | **39 B/s** | **60 B/s** |

A 64-byte chunk of ordinary shell output carries 8–10 newlines, costing about
1.17 s of rendering on the Timex and 0.59 s on the ZX. The audio layer is not
the bottleneck for real traffic; `blit_scroll_region()` is.

**Corrected 2026-08-03.** An earlier revision said 77 / 32 / 45 B/s. Those
were wrong in both directions at once, which is worth recording:

- **Too pessimistic on throughput.** They assumed both directions carry a full
  64-byte payload. In practice the upstream frame is almost always empty — the
  user is reading, not typing — so the transaction is a full downstream frame
  plus an empty response, not two full frames.
- **Too optimistic on idle responsiveness.** They put idle deafness at ~4%,
  counting only the downstream poll. The slave is equally deaf while
  *transmitting* its own response, and with the leadout that response is the
  expensive half. The real figure is **23%** of an idle second, or 10% without
  the leadout.

Perf-review recommendation 5 has now been implemented (PR #9, `scroll_vram`
−37.2% Timex / −34.1% ZX), which is where the figures above come from. Before
it, the same traffic ran at 25.2 B/s and 38.8 B/s. The improvement at the
traffic mix that matters is **1.27× on the Timex and 1.17× on the ZX** — real,
but not the "roughly 2×" an earlier revision of this document claimed by
applying the pure-newline worst case to mixed traffic.

## Architecture

```
PC (master)                                      ZX / Timex (slave)
┌────────────────────────────┐                  ┌──────────────────────────────┐
│ pty (shell)                │                  │ vtparse → screen → blit      │
│        ↕                   │                  │        ↕                     │
│ master ARQ (stop-and-wait) │                  │ slave ARQ (stop-and-wait)    │
│        ↕                   │                  │        ↕                     │
│ frame codec + CRC-16       │                  │ frame codec + CRC-16         │
│        ↕                   │                  │        ↕                     │
│ pulse encoder ─────────────┼──── EAR ─────────┤ EAR decoder (IRQ: keyboard)  │
│ pulse decoder ←────────────┼──── MIC ─────────┤ MIC send loop (+32 B leadout)│
└────────────────────────────┘                  └──────────────────────────────┘
      encoder → ffmpeg → Loopback "ZX Link" → ZEsarUX External Audio Source
      ZEsarUX --aofile → decode-follow → decoder
```

## Components

### Z80 side

| File | Responsibility | Host-testable |
|---|---|---|
| `include/alink.h` | frame layout, timing constants, API | — |
| `src/alink_frame.c` | frame encode/decode, CRC-16/CCITT-FALSE | **yes** |
| `src/alink_slave.c` | LISTEN/LINKED state machine, SEQ/ACK, dedupe | **yes** |
| `src/alink_phy.c` | EAR decoder, MIC send loop, idle keyboard poll | no |
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
| `tools/alink/phy.py` | pulse encoder and decoder, adapted from the PoCs |
| `tools/alink/main.py` | CLI: run the link, follow `--aofile`, drive ffmpeg |

`phy.py` cannot reuse the PoC decoder unchanged. `TapeDecoder.PILOT_LOCK`
requires 256 stable half-pulses before it enters the pilot state — ≈159 ms at
2,168 T per pulse, more than five times our 29.7 ms preamble. Pilot lock also
seeds the decoder's adaptive width and threshold tracking, so shortening it
means reworking that seeding, not just lowering a constant.

### Memory budget

The honest figure is the margin **below the IM2 table**, since that is where
the image grows. After PR #9:

| Build | Margin | After a ~1.5 KB backend | Notes |
|---|---:|---:|---|
| `term.tap` | 1,761 B | ~260 B | plus whatever removing ZRCP returns |
| `term-zx.tap` | 4,254 B | ~2,750 B | comfortable |

An earlier revision quoted 5,349 B and 8,082 B. Those were the sum of two
**non-contiguous** gaps (below the table plus above the trampoline); only the
first is usable for image growth, so the figures overstated headroom by
3,430 B each. On the Timex the audio backend is a tight fit and will likely
require the ZRCP backend to be compiled out — which it replaces anyway.
`tools/check_image_limit.py` enforces this at build time.

## Data flow

1. The master reads pty output and forms a `LINK` frame of 4–68 bytes.
2. The encoder renders it as pulses with a 48-pulse preamble; ffmpeg streams it
   to the `ZX Link` Loopback device; ZEsarUX feeds it to EAR.
3. The slave's idle loop -- which only watches EAR, since the IM2 handler
   scans the keyboard at 50 Hz by itself -- sees the preamble, disables
   interrupts and decodes the frame.
4. The slave validates the frame, delivers the payload to `vtparse`, renders
   it, then builds its response carrying any queued keystrokes.
5. The MIC send loop emits the response with a 48-pulse preamble, a 32-byte
   zero leadout and a closing edge. ZEsarUX writes every sample to `--aofile`.
6. The decoder follows that file, recovers the frame, and the master delivers
   the keystrokes to the pty.

## Error handling

From protocol v1; nothing is invented here. A block is a valid frame only if
its length equals `LEN` + 4, its CRC matches, `LEN` ≤ 64, and — for `LINK`
frames — `LEN` does not exceed the receiver's advertised maximum payload;
`HELLO` and `WELCOME` additionally require `LEN` ≥ 3, with bytes beyond
offset 2 reserved and ignored. Anything failing these is treated as if it
never arrived.

Duplicates are recognised by the `SEQ` bit, answered, but not delivered
twice. A valid `HELLO` in any state resets the session. Keyboard-buffer
overflow drops the **newest** bytes and rings BEL, never the oldest, so the
start of a command being typed survives.

Two hazards are specific to this project:

- **Keyboard scanning stops during reception** (D2). Unavoidable; the design
  minimises the window rather than eliminating it.
- **The tail of every upstream transmission may be truncated** by 8–16 bytes
  in capture. The 32-byte leadout after the CRC absorbs it.

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
- **Physical layer:** encoder → decoder round trip in Python without the
  emulator, including a truncated-tail case that must be absorbed by the
  leadout, and a preamble-only capture that must not yield a frame.
- **End-to-end:** ZEsarUX with the PoC audio path, both TAPs.

## Out of scope

- Turbo bit timings. Available later as a pure physical-layer change with no
  protocol impact (D1).
- Real hardware validation. The code drives EAR and MIC with standard tape
  timings, so it should work on real sockets, but v1 is validated only under
  ZEsarUX.
- File transfer, compression, encryption. The link presents a transparent
  byte pipe, so XMODEM and friends could later run over it unchanged.
- Replacing the ZRCP test backend (D5), except where the Timex image runs out
  of room and it must be compiled out.
