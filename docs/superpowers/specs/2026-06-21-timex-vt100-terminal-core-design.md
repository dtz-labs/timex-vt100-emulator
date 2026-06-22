# Timex VT-100 Terminal Core — Design

**Date:** 2026-06-21
**Target hardware:** Timex TC2048 (primary). Z80A @ 3.5 MHz, 48 KB RAM, SCLD video.
**Toolchain:** Z88DK (`zcc`, Oct 2025 build) at `~/Programowanie/z88dk`; **ZEsarUX 13.0** emulator at `/Applications/ZEsarUX.app/Contents/MacOS/zesarux` (machine `TC2048`). ZEsarUX replaces Fuse: it adds a scriptable remote protocol (ZRCP) and headless video, so emulator checks can be **automated**, not just eyeballed.
**Status:** Approved design — first of several specs for the terminal project. Later specs cover the **Spectranet transport** (telnet/TCP to a host running emacs/vi) and, optionally, a **colour mode**. **Revised 2026-06-21 after an independent technical review** that verified the load-bearing z88dk claims against the installed libraries and corrected the hi-res linkage premise and the default-palette bug (see §2.1, D8, D9). Some hardware/toolchain claims here are carried over *verified* from the sibling twin-stick game project (same toolchain).

---

## 1. Purpose & priorities

Build the **terminal engine core** for using a Timex TC2048 as a VT-100 terminal. This slice interprets an incoming VT-100/ANSI byte stream and renders it as crisp **64×24 hi-res monochrome text**, and maps the Spectrum keyboard back into an outgoing byte stream. The eventual goal (future spec) is connecting to a Unix host over Spectranet so the Timex can drive `emacs`/`vi`.

Mirroring the twin-stick game's philosophy, this slice **de-risks the hardest engine parts first** — the VT-100 parser, the hi-res text renderer, and the keyboard mapping — *behind a communications interface*, before any real networking is layered on.

**Stated priorities (from the user):**
1. Correct VT-100 interpretation (enough to run a full-screen editor).
2. Readability of 64-column hi-res text.

Communications (Spectranet, telnet) is explicitly **out of this slice**, sitting behind a `conn` interface fed by a stub/loopback driver.

Non-goals for this slice: networking, SSH/crypto, colour, scrollback history, double-height/double-width lines, smooth scroll, mouse reporting, 80-column mode.

---

## 2. Key decisions (and why)

| # | Decision | Rationale |
|---|----------|-----------|
| D1 | **Hi-res mono 512×192, 64×24 chars, 8×8 font.** | Width matters for an editor; SCLD hi-res is the only mode giving ≥64 columns. Hi-res is monochrome-only, accepted because emacs needs *width*, not colour. "Reverse video" (emacs mode line, region) = invert the 8 glyph bytes — cheap. Tell the host `stty cols 64 rows 24`. |
| D2 | **Target TC2048, build `+zx`, ZEsarUX machine `TC2048`.** | Same target family as the game (proven toolchain). TC2048 has a Spectrum-compatible edge connector, so the *future* Spectranet card plugs in directly (TS2068's connector would need an adapter). |
| D3 | **Cell-grid model + dirty-row incremental render** (Approach A). | Parser mutates an in-RAM 64×24 cell grid `{ch, attr}` with per-row dirty flags; renderer blits only changed cells. Scroll-region, insert/delete line/char, selective erase and reverse-video re-render are all trivial on a grid and impossible-to-get-right writing straight to the interleaved 2-bank hi-res display file. ~3 KB RAM cost is negligible. |
| D4 | **Comms behind a `conn` interface; MVP uses a stub/loopback + baked demo stream.** | The VT parser + renderer + keyboard are the engine risk and are testable without hardware. Networking is a separate concern with its own external/hardware risk → its own spec. |
| D5 | **Keyboard: CAPS SHIFT → Ctrl, SYMBOL SHIFT → Meta (or ESC-prefix), `5/6/7/8` → cursor CSI.** | The Spectrum has no Ctrl/Meta keys; emacs is built on them. Remap the two shift keys. Pure decode (key event → byte sequence) is host-tested; the matrix read is a thin target-only wrapper, exactly like the game's `input.c`. |
| D6 | **Reuse the game's hard-won toolchain rules.** | Same `zcc`/SDCC: pass structs via **out-pointers, never return by value** (SDCC z80 `gen.c` crash); **avoid header-name shadowing** of z88dk system headers (`-iquote`, distinct names); **no float/malloc/recursion**; `-clib=sdcc_iy`; ORG defaults to `0x8000`. |
| D7 | **Poll-driven main loop** (no `HALT` dependency). | The terminal polls `conn` and the keyboard; it does not need the frame interrupt. The crt boots with interrupts disabled (game §2.1) — harmless here. `im 1; ei` is added **only if** we want cursor blink / timed pacing (then a frame counter). |
| D8 | **Own the hi-res address math — this is the baseline, not a fallback.** | The review verified the z88dk `tshr_*` / `ts_vmod` family is **not reachable under `+zx`** (absent from `zx_clib.lib`, header off the include path; lives only in `ts2068_clib.lib` / `zxn_clib.lib`). So we compute hi-res addresses ourselves. The verified formula: `byte_col = char_col >> 1; file_base = (char_col & 1) ? 0x6000 : 0x4000; addr = file_base + zx_thirds(byte_col, pixel_row)` where `zx_thirds` is the standard ZX scanline byte math (carried verbatim from the game's host-tested `scld_scanline`). The per-column even/odd bank split is **new** vs the game and must be host-tested (module `hires.c`, §4). |
| D9 | **Base palette white-on-black, set via port `0xFF` bits 3–5; REVERSE defined relative to it.** | Hi-res colour is global (no per-cell attrs), chosen by port `0xFF` bits 3–5. `OUT (0xFF),6` alone leaves bits 3–5 = `000` = **black-on-white** (a white screen) — not what we want. We set the bits-3–5 code for **white-on-black** (exact 3-bit code confirmed at M1 on ZEsarUX; `000` is *not* it). `ATTR_REVERSE` then = draw the complemented glyph (white cell, black ink) against the black field; against a white base it would be invisible, hence the base must be fixed first. |

---

## 2.1 Hardware & toolchain facts

**Verified by the independent review against the local z88dk install (2026-06-21):**
- ⛔ **The z88dk Timex hi-res API (`ts_vmod`, `tshr_*`) is NOT reachable under `+zx`.** Verified three ways: `asm_ts_vmod` / `asm_tshr_*` are absent from `lib/clibs/zx_clib.lib` (present only in `ts2068_clib.lib` and `zxn_clib.lib`); `#include <arch/ts2068.h>` under `+zx` fails ("file not found" — the `+zx` config never adds `include/arch/ts2068` to the search path); and the newlib prototypes live in a Spectrum-Next header that dies on Next-only tokens. **Consequence:** we own the hi-res address math (D8) — this is the path, not a contingency. The standard `asm_zx_cxy2saddr` (0x4000 file) *is* in `zx_clib.lib` and the even-column path matches it.
- ✅ **Hi-res mode byte = 6** (port `0xFF` bits 0–2 = `110`); confirmed in `<arch/ts2068/ts2068.h>` (`VMOD_HIRES 6`) and the WoS Timex reference.
- ✅ **Both display files form one image; columns alternate banks** — even char-column → `0x4000` file, odd → `0x6000` file (high byte OR `$60`). Confirmed in z88dk `asm_tshr_cxy2saddr.asm` and WoS ("columns taken alternately from screen 0 and screen 1"). No page-flip (fine — a terminal doesn't need it).
- ✅ **Hi-res vertical/thirds math == standard ZX layout** (`asm_tshr_cy2saddr` is literally aliased to `asm_zx_cy2saddr`), so the game's host-tested scanline byte math is reusable per file.
- FZX hi-res font backend and the `tshr_01_output_char_64` console driver exist but are `ts2068`/`zxn`-target — structural reference only.

**Carried over verified from the game (same toolchain):**
- crt boots with **interrupts disabled**; `-clib=sdcc_iy` works; ORG defaults to `0x8000`; `-create-app` emits the `.tap`; SDCC **struct-return-by-value crashes** → out-pointers; header names must not shadow z88dk's (`-iquote` + distinct names); port `0xFF` **bit 6 = hardware DI** (EI can't override) so every OUT keeps bits 6–7 = 0.
- **I/O under `sdcc_iy`:** `outp()` from `<stdlib.h>` is *not* declared — use `z80_outp()` from `<z80.h>`; interrupts/HALT via `<intrinsic.h>` (`intrinsic_im_1` / `intrinsic_ei`). (Game §15.5.)

**To confirm at M1 (no invented APIs — finalise empirically):**
- The exact **bits-3–5 hi-res colour code for white-on-black** (checked on ZEsarUX via screen dump; `000` = black-on-white is *not* it — see D9).
- ✅ **Confirmed (M1):** our own `hires.c` address math produces the right screen bytes — host-tested (9 checks) and verified byte-exact on ZEsarUX TC2048 via ZRCP (`test/zesarux_smoke.py`). Hi-res display renders crisp 64-col text in the GUI.
- Hi-res memory **contention** and **full-screen render cost** — measure with `z88dk-ticks` before claiming any refresh rate (see §11); the game measured ~9,000 T per 8×8 C blit, so full-repaint paths are a real cost.
- Reverse-video and cursor rendering look correct on ZEsarUX (ZRCP screenshot / RAM dump — automatable, plus a human glance).

---

## 3. Scope of this slice

**In scope**
- Build system producing a `.tap` that boots into hi-res mode on ZEsarUX-as-TC2048.
- `video`: enter SCLD hi-res (mode 6), clear, bank-layout constants.
- `screen`: 64×24 cell grid `{ch, attr}`, cursor, scroll region, current SGR, per-row dirty flags; all grid operations (put, erase, scroll, insert/delete, save/restore cursor).
- `vtparse`: VT-100/ANSI state machine driving `screen` (subset in §6).
- `render`: blit dirty cells to the hi-res display file; 8×8 glyph blit with reverse-video inversion; cursor draw.
- `keymap`: Spectrum keyboard → outgoing bytes (Ctrl/Meta/cursor mapping).
- `conn`: byte source/sink interface + a **stub/loopback** driver and a **baked demo VT-100 stream** to drive the loop without networking.
- `main`: the poll loop wiring it together.

**Out of scope (future specs)**
Spectranet/telnet/TCP transport, SSH/crypto, colour mode, scrollback, double-height/width, smooth scroll, mouse, 80-column.

---

## 4. Module architecture

Boundary rule: **only `video.c` / `render.c` know hi-res addresses and port `0xFF`.** `vtparse` / `screen` / `keymap` are pure and host-testable. `conn` is the seam where networking will later slot in.

```
src/main.c     poll loop: conn_read -> vtparse -> screen; render dirty; keymap -> conn_write
src/vtparse.c  VT-100/ANSI escape state machine, drives screen        [pure, host-tested]
src/screen.c   64x24 cell grid, cursor, scroll region, SGR, dirty rows [pure, host-tested]
src/hires.c    (char_col,pixel_row) -> (bank base, byte offset)        [pure, host-tested]
src/render.c   dirty cells -> hi-res display file via hires.c; 8x8 glyph blit; cursor [hi-res]
src/video.c    SCLD hi-res mode set (raw OUT 0xFF), palette, clear     [Timex-specific core]
src/font.c     8x8 ASCII font data (0x20-0x7E) + DEC line-drawing glyphs [data]
src/keymap.c   keyboard matrix -> bytes (Ctrl/Meta/cursor)  decode=pure, read=target-only
src/conn.c     conn_read/conn_write interface + stub/loopback + demo stream

include/  vtparse.h  screen.h  hires.h  render.h  video.h  font.h  keymap.h  conn.h  types.h
```

**Interface sketches** (structs via out-pointers per D6):

```c
/* screen.h */
typedef struct { uint8_t ch; uint8_t attr; } cell_t;       /* attr bits: REVERSE, UNDERLINE */
typedef struct { cell_t cells[ROWS][COLS]; uint8_t cx, cy;
                 uint8_t top, bot;     /* scroll region */
                 uint8_t attr;         /* current SGR */
                 uint8_t dirty[ROWS];  /* per-row dirty flag */
                 /* saved cursor, modes (autowrap, cursor-visible) ... */ } screen_t;
void screen_init(screen_t *s);
void screen_putc(screen_t *s, uint8_t ch);
void screen_cup(screen_t *s, uint8_t row, uint8_t col);
void screen_erase_display(screen_t *s, uint8_t mode);
void screen_erase_line(screen_t *s, uint8_t mode);
void screen_scroll(screen_t *s, int8_t n);     /* within [top,bot] */
/* ... move, insert/delete line/char, set_attr, save/restore cursor ... */

/* vtparse.h */
typedef struct { uint8_t state; uint8_t params[MAX_PARAMS]; uint8_t nparams; /*...*/ } vtparse_t;
void vt_init(vtparse_t *vt);
void vt_feed(vtparse_t *vt, screen_t *s, uint8_t byte);    /* one byte -> screen mutation */

/* conn.h  (MVP: stub) */
int  conn_read(uint8_t *buf, int max);   /* returns bytes available, 0 if none */
void conn_write(const uint8_t *buf, int n);

/* keymap.h  (decode is pure; read is target-only) */
int  keymap_poll(uint8_t *out, int max);  /* fills out[] with bytes for pressed keys */

/* render.h */
void render_flush(const screen_t *s);     /* blit dirty rows, clear dirty flags */
void render_cursor(const screen_t *s);
```

---

## 5. Memory map (TC2048, hi-res)

```
0x4000-0x57FF  hi-res display file 1 (6144 b)  ─┐ both files together form the
0x6000-0x77FF  hi-res display file 2 (6144 b)  ─┘ single 512x192 image (no page-flip)
0x5800-0x5AFF / 0x7800-0x7AFF  attr regions — unused in hi-res (colour is global)
0x8000-0xFFFF  program code + data + stack (~32 KB)   ORG 0x8000 (default), stack ~0xFF58
```

Data budget (in the 32 KB above `0x8000`): cell grid 64×24×2 = **3072 B**, dirty rows 24 B, font ~768 B (+ line-drawing glyphs), parser/keymap/conn state + buffers a few hundred bytes, C runtime/stack ~2 KB. Total data ≈ **6–7 KB → ~25 KB free** (the review confirmed the budget is comfortable, not tight). The lower 16 KB bank is screen territory by design. A 128 KB machine would later buy scrollback (~1.5 KB/screen) but is not required.

---

## 6. VT-100 subset (enough for emacs/vi)

**Must:**
- **C0:** BS, HT (tab stops every 8), LF, CR, BEL (ignore or brief flash), FF.
- **ESC:** `c` (RIS reset), `D` (IND), `M` (RI), `E` (NEL), `7`/`8` (DECSC/DECRC save/restore cursor).
- **CSI `ESC[`:** CUU/CUD/CUF/CUB (`A`/`B`/`C`/`D`), CUP/HVP (`H`/`f`), ED (`J` 0/1/2), EL (`K` 0/1/2), IL/DL (`L`/`M`), ICH/DCH (`@`/`P`), **SGR** (`m`: 0 reset, 7 reverse, 27 not-reverse, 4 underline, 24 not-underline; bold/colours accepted-and-ignored), **DECSTBM** (`r` scroll region), SM/RM (`h`/`l`) incl. **DECTCEM `?25`** (cursor on/off), **DECAWM `?7`** (autowrap), **DECCKM `?1`** (cursor-key mode), **DSR** (`6n` → reply `ESC[row;colR`), **DA** (`c` → reply `ESC[?1;0c`).

**Should (nice for mc/emacs boxes):** G0/G1 charset select `ESC(B` / `ESC(0` with DEC special-graphics line-drawing.

**Won't:** double-height/width (`ESC#`), smooth scroll, AVO colour attrs, printer controls.

---

## 7. Main loop

```
init:
    video_hires_on()            ; z80_outp(0xFF, 6 | WHITE_ON_BLACK_BITS) -- bits0-2=110 hi-res,
                                ; bits3-5 = white-on-black palette (D9), bits6-7 = 0 (keep interrupt)
    screen_init(&scr); vt_init(&vt); conn_open_stub()
    render full clear; render_flush(&scr)

loop (poll-driven):
    n = conn_read(buf, sizeof buf)         ; bytes from host (stub/demo stream)
    for i in 0..n:  vt_feed(&vt, &scr, buf[i])   ; mutate cell grid + mark dirty rows
    render_flush(&scr)                     ; blit only dirty rows; redraw cursor
    m = keymap_poll(key, sizeof key)       ; Spectrum keys -> bytes
    if m: conn_write(key, m)               ; (loopback echoes back into conn_read in MVP)
```

No `HALT` required. If cursor blink is wanted, add `im 1; ei` once at init and a frame counter to toggle the cursor cell's dirty flag (§D7).

---

## 8. Cell model & rendering

- **Base palette = white-on-black** (D9), set once via port `0xFF` bits 3–5. All glyph/reverse logic is defined relative to this: normal cell = white ink on black; `ATTR_REVERSE` cell = black ink on white (the complemented glyph). Get the base wrong (e.g. `OUT 0xFF,6` alone → black-on-white) and reverse-video becomes invisible.
- `cell_t = { ch, attr }`; `attr` bits: `REVERSE`, `UNDERLINE`. Current SGR lives on `screen_t.attr` and is copied into each cell on `putc`.
- **Dirty tracking per row** (24 flags). `vt_feed` marks a row dirty on any change; `render_flush` blits only dirty rows then clears the flags. Scroll marks the whole region dirty.
- **Glyph blit:** char on the 64×24 grid maps to exactly one byte column → always byte-aligned (the hi-res win). Address from **our own `hires.c`** (D8 — the z88dk helper isn't linkable under `+zx`): `byte_col = col>>1; base = (col&1)?0x6000:0x4000; addr = base + zx_thirds(byte_col, row*8)`; copy 8 font bytes down 8 scanlines (step within a char row = +256, the thirds math handles char-row boundaries). **Reverse-video** = blit the complemented bytes. **Underline** = set the last row to `0xFF`.
- **DEC line-drawing glyphs** (`ESC(0`) must be drawn to **touch cell edges** — horizontal at the row's vertical centre spanning full width, verticals at a fixed column abutting neighbours — or box borders won't connect. This is a font-data requirement, not code.
- **Cursor:** invert the cell under the cursor (or draw a block/underline); hidden when DECTCEM off. Blink optional (§D7).

---

## 9. Keyboard mapping

- Read the 8 half-rows of the Spectrum matrix; produce ASCII.
- **CAPS SHIFT held → Ctrl** (mask `& 0x1F`). **SYMBOL SHIFT held → Meta**: emit `ESC` prefix then the key (the robust, mode-independent way to send Meta).
- **Cursor keys** `CAPS+5/6/7/8` → `ESC[D / ESC[B / ESC[A / ESC[C` (or SS3 form under DECCKM — track the mode flag from §6).
- ENTER → CR, DELETE (CAPS+0) → DEL/BS, BREAK/EDIT → ESC.
- Decode (key state → byte sequence) is **pure and host-tested**; the matrix scan is the only target-only part.

---

## 10. Build & run

Flags **finalised empirically at M1** (no invented z88dk APIs/flags):

```bash
export PATH="$HOME/Programowanie/z88dk/bin:$PATH"
export ZCCCFG="$HOME/Programowanie/z88dk/lib/config"

zcc +zx -SO3 -clib=sdcc_iy -iquote"$PWD/include" src/*.c -o build/term -create-app

ZX=/Applications/ZEsarUX.app/Contents/MacOS/zesarux

# Interactive run as a Timex TC2048 (the +zx tap reaches the SCLD via OUT 0xFF):
"$ZX" --machine TC2048 --tape build/term.tap

# Headless / scripted check: no video/audio, ZRCP on :10000 for memory + screen dumps.
"$ZX" --machine TC2048 --tape build/term.tap --vo null --ao null \
      --enable-remoteprotocol --remoteprotocol-port 10000 --quickexit
```

Notes: still build `+zx` (TC2048 is Spectrum-compatible; hi-res is a runtime SCLD feature of the machine model). ZEsarUX machine id is **`TC2048`** (not Fuse's bare `2048`). `-zorg` not needed (ORG defaults to 0x8000).

---

## 11. Testing strategy

- **Host unit tests (TDD):** `vtparse`, `screen`, `keymap` — pure integer logic compiled with native `cc`. Feed byte sequences, assert the resulting cell grid / cursor / attributes (golden cases drawn from real VT-100 sequences). `test/run.sh` mirrors the game's harness.
- **Emulator integration (ZEsarUX, scriptable):** `video` + `render` + the full poll loop driven by a **baked demo VT-100 stream** (ANSI art / a recorded editor screen / a scroll test). Unlike Fuse, ZEsarUX can be driven headlessly (`--vo null`) and scripted over **ZRCP** (`--enable-remoteprotocol`), which exposes memory reads and screen capture — so M1/M3 can run **automated** checks: boot the tap, read the hi-res display files (`0x4000`/`0x6000`) over ZRCP and assert our `hires.c` wrote the expected bytes, plus capture a screenshot for a diffable artifact. (Exact ZRCP screen/memory command names confirmed at M1 — no invented APIs.) A human glance still helps for legibility/flicker, but is no longer the only option. `z88dk-ticks` measures render cost.
- **Host tests must not bake in loopback behaviour:** the MVP `conn` loopback echoes *raw* keystrokes (Enter = bare CR, control chars literal) — that is **not** how a real host behaves (a host echoes and translates). Keep `vtparse`/`keymap` test expectations defined against real VT-100 semantics, not against what the loopback happens to produce.

**Performance reality (do not hand-wave):** the dirty-row model makes the *common* editor case cheap — typing dirties one row, scrolling dirties the region. But two paths are genuinely expensive in C: a **full-screen repaint** (`^L`, clear, switching emacs buffers — 64×24 = 1536 cells) and a **fast-scrolling region**. The game measured **~9,000 T per 8×8 glyph blit in C**; a full repaint at even ~1,000 T/cell is ~1.5M T ≈ 0.4 s. This is acceptable for a *correct* core, but **hand-written asm for the glyph inner loop is the expected lever**, and no refresh-rate claim is made until measured with `z88dk-ticks`. Incremental render reduces the frequency of the slow path, it does not make rendering free.

---

## 12. Open items to resolve during planning / M1

1. ~~Does `ts_vmod` / `tshr_*` link under `+zx`?~~ **Resolved (review):** no — own the address math (D8, `hires.c`). Remaining: host-test our formula and confirm screen RAM on target.
2. **bits-3–5 palette:** M1 ran black-on-white (mode `0x06`) and it reads great in the GUI. White-on-black (D9) is now a **preference**, not a blocker — flip bits 3–5 if wanted. Note: headless `save-screen` captures only the standard ULA layer (352×304), so palette/visual checks need the GUI, not the headless dump.
3. Own embedded 8×8 font vs copying the Spectrum ROM font (`0x3D00`).
4. ~~Include DEC line-drawing charset?~~ Yes (should-have); font glyphs must touch cell edges.
5. Cursor blink: implement now (needs `im1;ei` + frame counter) or static block first.
6. Meta via `ESC`-prefix vs raw 8-bit — `ESC`-prefix chosen; revisit if a key clashes.
7. Content of the baked demo stream for M3 (what best exercises scroll-region + SGR + CUP).
8. Render cost / contention at M3 — measure with `z88dk-ticks` before claiming any refresh target; expect asm for the glyph loop on the full-repaint path.

---

## 13. Milestones

1. **M1 — hi-res smoke test:** ✅ **DONE (2026-06-22).** `+zx` build autoloads on ZEsarUX `--machine TC2048` and enters hi-res; `main.c` draws ASCII on the 64×24 grid from the ROM 8×8 font via our own `hires.c` math. Verified **two ways**: a ZRCP byte-exact RAM check (`test/zesarux_smoke.py` — on-screen glyph bytes for 'T'/file0 and 'C'/file1 match the ROM font) and a **visual GUI check (crisp 64-column text)**. Palette is currently **black-on-white** (mode `0x06`, bits 3–5 = 0); white-on-black (D9) is an optional flip.
2. **M2 — pure core:** `vtparse` + `screen` host-TDD over the §6 subset (red/green/refactor). *(In progress: `screen.c` `init`/`putc`/`cup`/`scroll` green.)*
3. **M3 — render + loop:** dirty-row `render` + the poll loop driven by the baked demo stream; verified on ZEsarUX (screenshot + ZRCP RAM assertions).
4. **M4 — keyboard + loopback:** `keymap` (Ctrl/Meta/cursor) + local echo through `conn`, so typing shows on screen.

**Next spec:** Spectranet transport — real `conn` over telnet/TCP to a host running `emacs`/`vi`.
