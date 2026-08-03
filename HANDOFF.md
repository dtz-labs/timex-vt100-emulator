# Handoff — audio link, PR B Task 6

Repo: `/Volumes/SSD/Programowanie/timex-vt100-emulator` (`dtz-labs/zx-vt102-terminal`).
Branch: `feat/alink-phy`, pushed, open as **PR #13**. Master is green.

## What you are finishing

Issue #5: the terminal speaks protocol v1 over audio. **Tasks 1–5 of 6 are
done and verified.** Only Task 6 remains.

Plan: `docs/superpowers/plans/2026-08-03-audio-link-physical-layer.md`
Design: `docs/superpowers/specs/2026-08-02-audio-link-design.md`

### Task 6, exactly

1. **`test/alink_zesarux.py`** — start ZEsarUX with `--aofile` and a generated
   `--realtape` `.rwa` carrying a `HELLO`; assert the terminal's `WELCOME`
   appears in the dump; then inject a `LINK` frame with printable text and
   assert the text appears on screen, read over ZRCP the way
   `test/zesarux_smoke.py` already does.
2. **`make smoke-audio`** in the Makefile, running it for both
   `build/term-audio.tap` and `build/term-zx-audio.tap`.
3. **Fill in `docs/audio-link-setup.md` section 7** ("What is not here yet")
   with real run instructions, and drop "(in progress)" from the README's
   Audio Link section.

## State that matters

Build and test:

```sh
export PATH="$HOME/Programowanie/z88dk/bin:$PATH"
export ZCCCFG="$HOME/Programowanie/z88dk/lib/config"
make ci                       # host suites, ~1,350 checks, all green
make audio audio-zx           # the two audio TAPs
```

All six images pass the image-limit gate. Margins: `term` 4,321 B,
`term-audio` 2,290 B, `term-zx-audio` 2,525 B.

Files you will touch or read:

| File | What it is |
|---|---|
| `src/alink_phy.c` | Z80 MIC send loop and EAR receiver. Target only, cycle-counted. |
| `src/alink_frame.c`, `src/alink_slave.c` | Protocol v1, host-tested. Do not modify. |
| `tools/alink/phy.py` | Pulse codec, `.rwa`/WAV writers, decoder |
| `tools/alink/main.py` | pty runner; `--selftest` checks plumbing with no audio |
| `src/conn.c` | `#ifdef CONN_BACKEND_AUDIO` branch |
| `test/zesarux_smoke.py` | The pattern to copy for the new harness |

## Hard-won facts — do not rediscover these

**Testing needs no virtual audio device.** Upstream: `--aofile` (raw u8 dump).
Downstream: a `.rwa` file via `--realtape`. Loopback is only for live use.

**`sox` is a hidden `--realtape` dependency.** A WAV goes through it; with sox
absent ZEsarUX prints one warning and then *silently plays nothing*, which
looks exactly like a broken decoder. Use `phy.write_realtape()` — bare raw,
44,100 Hz, mono, 8-bit unsigned, no tooling. **The extension must be `.rwa`**;
`.raw` is rejected as "Unknown input tape type".

**Working ZEsarUX invocation** (both directions at once):

```sh
cd /Applications/ZEsarUX.app/Contents/Resources
/Applications/ZEsarUX.app/Contents/MacOS/zesarux --noconfigfile --machine TC2048 \
  --vo null --ao null --nowelcomemessage --no-saveconf-on-exit \
  --tape build/term-audio.tap --fastautoload \
  --realtape /tmp/in.rwa --aofile /tmp/out.raw
```

`--realtape-fastload` does not exist. `--vo null --ao null` runs it headless.

**Decoding a capture:**

```python
import sys; sys.path.insert(0, "tools")
from alink import phy, frame
blocks = phy.decode_raw(open("/tmp/out.raw", "rb").read(), unsigned8=True)
print([frame.decode(b) for b in blocks])
```

**z88dk/SDCC traps** (all cost real time already):
- `-clib=sdcc_iy` **reserves IY** — never use `IYL`/`IYH`.
- sdasz80 rejects `;` comments inside `__asm`, and `u`-suffixed or
  parenthesised constants that reach it through the preprocessor. Bare
  numbers only; keep consistency checks in C with `#if`/`#error`.
- `jr` to a far label overflows ±127 bytes — use `jp`.
- Anything over a few hundred bytes goes in BSS, never on the stack. A
  `main()`-local `screen_t` once overflowed into the IM2 table.

**Git/shell:** commit messages containing quotes break `-m` in this shell.
Write to a file and use `git commit -F`.

## Style the reviewers here expect

- Every number in a commit message, PR body or doc is **measured**, and the
  measurement is shown. A fabricated constant (the "400 pilot pulses" that
  came from the PoC's Python decoder and was attributed to the ROM) already
  destroyed one design decision in this project.
- Mutation-test anything claimed to be a gate. Three of this project's tests
  passed while the thing they guarded was broken.
- When a test fails, first ask whether the **test** is wrong. It was, three
  times: an impossible `ACK=0` poll, a dropped-response-vs-dropped-request
  mix-up, and a pulse-count that forgot the closing edge.
- Corrections go in the document, not in a changelog. The design spec keeps
  its own refuted reasoning visible on purpose.

## Do not

- Call ROM tape routines (`0x0556`, `0x04C2`) or enter ROM internals. Decision
  D1 rules them out; that is what keeps this working on a TS2068.
- Modify `src/alink_frame.c` or `src/alink_slave.c` — they are host-tested and
  cross-checked against the Python master.
- Link `src/alink_phy.c` into any host test. It is target-only, like
  `blit_*.c`.
