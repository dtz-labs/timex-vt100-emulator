# Timex VT-100 Terminal Core — Design

**Date:** 2026-06-21
**Target hardware:** Timex TC2048 (primary). Z80A @ 3.5 MHz, 48 KB RAM, SCLD video.
**Toolchain:** Z88DK (`zcc`, Oct 2025 build) at `~/Programowanie/z88dk`; Fuse emulator at `/Applications/Fuse.app` (machine id `2048`).
**Status:** Approved design — first of several specs for the terminal project. Later specs cover the **Spectranet transport** (telnet/TCP to a host running emacs/vi) and, optionally, a **colour mode**. Some hardware/toolchain claims here are carried over *verified* from the sibling twin-stick game project (same toolchain); the hi-res-specific ones are marked **to confirm at M1**.

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
| D2 | **Target TC2048, build `+zx`, Fuse machine `2048`.** | Same target family as the game (proven toolchain). TC2048 has a Spectrum-compatible edge connector, so the *future* Spectranet card plugs in directly (TS2068's connector would need an adapter). |
| D3 | **Cell-grid model + dirty-row incremental render** (Approach A). | Parser mutates an in-RAM 64×24 cell grid `{ch, attr}` with per-row dirty flags; renderer blits only changed cells. Scroll-region, insert/delete line/char, selective erase and reverse-video re-render are all trivial on a grid and impossible-to-get-right writing straight to the interleaved 2-bank hi-res display file. ~3 KB RAM cost is negligible. |
| D4 | **Comms behind a `conn` interface; MVP uses a stub/loopback + baked demo stream.** | The VT parser + renderer + keyboard are the engine risk and are testable without hardware. Networking is a separate concern with its own external/hardware risk → its own spec. |
| D5 | **Keyboard: CAPS SHIFT → Ctrl, SYMBOL SHIFT → Meta (or ESC-prefix), `5/6/7/8` → cursor CSI.** | The Spectrum has no Ctrl/Meta keys; emacs is built on them. Remap the two shift keys. Pure decode (key event → byte sequence) is host-tested; the matrix read is a thin target-only wrapper, exactly like the game's `input.c`. |
| D6 | **Reuse the game's hard-won toolchain rules.** | Same `zcc`/SDCC: pass structs via **out-pointers, never return by value** (SDCC z80 `gen.c` crash); **avoid header-name shadowing** of z88dk system headers (`-iquote`, distinct names); **no float/malloc/recursion**; `-clib=sdcc_iy`; ORG defaults to `0x8000`. |
| D7 | **Poll-driven main loop** (no `HALT` dependency). | The terminal polls `conn` and the keyboard; it does not need the frame interrupt. The crt boots with interrupts disabled (game §2.1) — harmless here. `im 1; ei` is added **only if** we want cursor blink / timed pacing (then a frame counter). |
| D8 | **Own the hi-res address math if the z88dk `tshr_*` lib doesn't link under `+zx`.** | We already proved in the game that we can compute interleaved screen addresses ourselves from a base. Hi-res addressing is a known formula; not being able to link `arch/ts2068` under `+zx` is a non-blocker. |

---

## 2.1 Hardware & toolchain facts

**Verified by reading the local z88dk install (2026-06-21):**
- z88dk ships real Timex hi-res support: `ts_vmod(VMOD_HIRES)` in `<arch/ts2068/ts2068.h>` (`VMOD_SPEC=0`, `VMOD_HICLR=2`, `VMOD_HIRES=6`); the `tshr_*` addressing family (`tshr_cxy2saddr`, `tshr_pxy2saddr` with `x` as `unsigned int` 0..511, `tshr_px2bitmask`, and `tshr_saddrp{right,left,up,down}` / `tshr_saddrc{...}` walkers); `asm_tshr_cls` / `asm_tshr_scroll_up` / `asm_tshr_cls_pix`; and an FZX proportional-font hi-res backend (`struct fzx_tshr_state`, `_fzx_tshr_draw_{or,reset,xor}`).
- Hi-res console output drivers exist (`tshr_01_output_char_64`, `_128`, `_fzx`) but are **`zxn`-target** (Spectrum Next) — structural reference only, not drop-in for `+zx`.
- SP1 hi-res C examples live under `libsrc/sprites/software/sp1/deprecated/ts2068hr/examples/` (deprecated but compilable reference).

**Carried over verified from the game (same toolchain):**
- crt boots with **interrupts disabled**; `-clib=sdcc_iy` works; ORG defaults to `0x8000`; `-create-app` emits the `.tap`; SDCC **struct-return-by-value crashes** → out-pointers; our header names must not shadow z88dk's (`-iquote` + distinct names).

**To confirm at M1 (no invented APIs — finalise empirically):**
- ⛔ Whether `ts_vmod` / `tshr_*` link under `+zx` (vs requiring `+ts2068`). **Fallback (D8):** compute hi-res addresses ourselves.
- The exact hi-res byte→pixel **bank interleaving** on the TC2048 (which display file feeds which screen columns), confirmed in Fuse and against the [WoS Timex reference](https://worldofspectrum.org/faq/reference/tmxreference.htm).
- Hi-res memory **contention** vs full-screen render cost — carry the game's caveat: treat T-state figures as uncontended lower bounds; measure with `z88dk-ticks` before claiming any refresh rate.
- Reverse-video and cursor rendering look correct in the Fuse GUI (no headless screenshot in this Fuse build — visual check is manual).

---

## 3. Scope of this slice

**In scope**
- Build system producing a `.tap` that boots into hi-res mode on Fuse-as-2048.
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
src/render.c   dirty cells -> hi-res display file; 8x8 glyph blit; cursor   [hi-res specific]
src/video.c    SCLD hi-res mode set, clear, bank constants             [Timex-specific core]
src/font.c     8x8 ASCII font data (0x20-0x7E)                         [data]
src/keymap.c   keyboard matrix -> bytes (Ctrl/Meta/cursor)  decode=pure, read=target-only
src/conn.c     conn_read/conn_write interface + stub/loopback + demo stream

include/  vtparse.h  screen.h  render.h  video.h  font.h  keymap.h  conn.h  types.h
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

Data budget (in the 32 KB above `0x8000`): cell grid 64×24×2 = **3072 B**, dirty rows 24 B, font 768 B, parser/keymap/conn state + buffers a few hundred bytes, C runtime/stack ~1–2 KB. Comfortable **~10+ KB free**. The lower 16 KB bank is screen territory by design. A 128 KB machine would later buy scrollback (~1.5 KB/screen) but is not required.

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
    video_hires_on()            ; OUT (0xFF), 6  (or ts_vmod(VMOD_HIRES)); attrs/colour global
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

- `cell_t = { ch, attr }`; `attr` bits: `REVERSE`, `UNDERLINE`. Current SGR lives on `screen_t.attr` and is copied into each cell on `putc`.
- **Dirty tracking per row** (24 flags). `vt_feed` marks a row dirty on any change; `render_flush` blits only dirty rows then clears the flags. Scroll marks the whole region dirty.
- **Glyph blit:** char on the 64×24 grid maps to exactly one byte column → always byte-aligned (the hi-res win). Address from `tshr_cxy2saddr` (or our own math, D8); copy 8 font bytes down 8 scanlines. **Reverse-video** = blit the complemented bytes. **Underline** = OR `0xFF` into the last row.
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

/Applications/Fuse.app/Contents/MacOS/Fuse --machine 2048 --tape build/term.tap
```

(Carried from the game: `--machine 2048` not `tc2048`; no `--auto-load`; `-zorg` not needed.)

---

## 11. Testing strategy

- **Host unit tests (TDD):** `vtparse`, `screen`, `keymap` — pure integer logic compiled with native `cc`. Feed byte sequences, assert the resulting cell grid / cursor / attributes (golden cases drawn from real VT-100 sequences). `test/run.sh` mirrors the game's harness.
- **Emulator integration (Fuse, visual):** `video` + `render` + the full poll loop driven by a **baked demo VT-100 stream** (ANSI art / a recorded editor screen / a scroll test). This Fuse build has no headless screenshot, so flicker/legibility/cursor are a **manual visual check**; `z88dk-ticks` confirms screen RAM was written at the right hi-res addresses and measures render cost.

---

## 12. Open items to resolve during planning / M1

1. Does `ts_vmod` / `tshr_*` link under `+zx`? If not, own the address math (D8).
2. Exact hi-res bank interleaving on TC2048 (confirm in Fuse + WoS ref).
3. Own embedded 8×8 font vs copying the Spectrum ROM font (`0x3D00`).
4. Include DEC special-graphics line-drawing charset now or defer.
5. Cursor blink: implement now (needs `im1;ei` + frame counter) or static block first.
6. Meta via `ESC`-prefix vs raw 8-bit — `ESC`-prefix chosen; revisit if a key clashes.
7. Content of the baked demo stream for M3 (what best exercises scroll-region + SGR + CUP).
8. Render cost / contention at M3 — measure before claiming any refresh target.

---

## 13. Milestones

1. **M1 — hi-res smoke test:** `+zx` build enters hi-res, draws fixed 8×8 text on the 64×24 grid in Fuse. Confirms mode set, addressing, and whether `tshr_*` links (else D8 fallback).
2. **M2 — pure core:** `vtparse` + `screen` host-TDD over the §6 subset (red/green/refactor).
3. **M3 — render + loop:** dirty-row `render` + the poll loop driven by the baked demo stream; visually verified in Fuse.
4. **M4 — keyboard + loopback:** `keymap` (Ctrl/Meta/cursor) + local echo through `conn`, so typing shows on screen.

**Next spec:** Spectranet transport — real `conn` over telnet/TCP to a host running `emacs`/`vi`.
