# 80-Column Display With The TT3000 ROM Font — Design

**Date:** 2026-07-26
**Supersedes:** the 64×24 display decisions in
`2026-06-21-timex-vt100-terminal-core-design.md` (D1, D3, §8). Everything else in
that spec — the VT parser, the cell-grid model, keyboard mapping, the `conn`
interface, the toolchain rules — stands unchanged.
**Status:** Approved design, pending implementation plan.

---

## 1. Purpose

Raise the terminal from **64×24 to 80×24**, and replace the hand-authored 8×8 font
with the 6×8 font extracted from the Timex Terminal TT3000 ROM.

80 columns is the width the rest of the Unix world assumes. The current spec lists
"the display is 64x24, not 80x24" as the first practical limitation in the README,
and "programs with dense 80-column layouts may run but will not look good" as the
last. This slice removes both.

**64-column mode is dropped entirely.** There is no runtime switch, no second
build, no DECCOLM. One mode, one font, one renderer.

---

## 2. Key decisions

Continuing the D-numbering from the core spec.

| # | Decision | Rationale |
|---|----------|-----------|
| D10 | **80×24 at 6 px per cell, 16 px margin each side.** | 512 / 6 = 85 columns available; 80 is the standard. The leftover 32 px split evenly is 2 bytes per side, so column 0 starts on a byte boundary and costs nothing. Left-aligning would have the same bit phase but a lopsided image. |
| D11 | **Single mode. `COLS` stays a compile-time constant, 64 → 80.** | The user scoped this to 80-only. That keeps `COLS` constant, so the ~25 uses in `screen.c` and `vtparse.c` follow automatically, `screen_t` stays a fixed-size struct, and no reflow/clear-on-switch semantics are needed. A runtime-switchable width was designed and rejected as unnecessary complexity. |
| D12 | **Font data extracted from `TT3000.rom` at `0x0A27`, normalised by one left shift.** | The ROM stores its 6-px cell in bits 6..1. Shifting left by 1 puts the cell in bits 7..2, which makes every packing shift a constant. Chosen over repacking the existing `genfont.py` glyphs — see §3 for the comparison that informed the choice. |
| D13 | **The generated font header is committed to the repo.** | CI and third parties must be able to build without the ROM, which is not redistributable as a whole. `tools/romfont.py` stays as the regeneration tool. See §9 for the licensing caveat. |
| D14 | **DEC line-drawing glyphs are authored by us at 6 px, not taken from the ROM.** | The ROM's frame set at `0x0D27` is **double-line**; VT100 special graphics is single-line. The set at `0x113F` is accented/national characters, not box drawing. `genfont.py` keeps the graphics table and loses its ASCII table. |
| D15 | **Attributes are applied to the 6-bit cell before packing.** | Reverse becomes `~g & 0xFC`, underline becomes `0xFC` in row 7. The packing masks discard the low 2 bits anyway, so no separate attribute path is needed in the inner loop. |
| D16 | **Correctness first, speed second.** | The renderer will be measurably slower (§8). The user explicitly deferred optimisation. Ship a correct C implementation, measure, then optimise if warranted. |

---

## 3. Font comparison (the basis for D12)

Both candidate fonts were rendered and diffed glyph by glyph over the 94 printable
non-space codes 0x21–0x7E.

| | |
|---|---|
| Both fonts' glyph bodies | **5 px wide** — neither is narrower than the other |
| Identical bit-for-bit | 11 / 94 |
| Differ by 1–2 px | 14 / 94 |
| Differ by 3–8 px | 25 / 94 |
| Differ by > 8 px | 44 / 94 |
| Most divergent | `%` `$` `@` `=` `7` `Z` `5` `P` `9` `R` |

The dominant difference is **vertical metrics, not letterform**: the ROM draws in
rows 1–6 (6-px capitals, blank row 0 as leading), the existing font in rows 0–6
(7-px capitals). Most of the per-glyph deltas are that offset, not a different
design.

**Conclusion recorded for the future:** the ROM font is not what makes 80 columns
possible — the existing glyphs at 6-px pitch would work equally well. The ROM font
was chosen on appearance. Anyone revisiting this should not assume a technical
constraint that does not exist.

**Defect found in the existing font during this comparison:** `%` is the only glyph
6 px wide (columns 0–5). At 6-px pitch it would touch its right neighbour. This
does not affect the ROM font, but it is why §10 includes a test that no glyph sets
bits 1..0.

---

## 4. Screen layout

```
byte:   0   1 | 2 ............................ 61 | 62  63
        margin          80 cells × 6 px               margin
pixel:  0 ..15 | 16 ......................... 495 | 496..511
```

Byte `b` in the 64-byte scanline lives in display file `b & 1` at index `b >> 1`
(the existing `hires_addr()` convention — its first argument always meant *byte*
column, which happened to equal character column at 8 px).

Cells 0–79 occupy bytes 2–61, so file indices 1–30 in each file. Indices 0 and 31
of both files are never written after `video_clear()`.

### Bit phase

For cell `col`, the leftmost pixel is at `p = 16 + 6 * col`. Only the bit phase
depends on `col mod 4`; the byte index rises monotonically with `col`:

| | `sh = p & 7` | mask in `byte` | mask in `byte+1` | spills? |
|---|---|---|---|---|
| col ≡ 0 (mod 4) | 0 | `0xFC` | — | no |
| col ≡ 1 (mod 4) | 6 | `0x03` | `0xF0` | yes |
| col ≡ 2 (mod 4) | 4 | `0x0F` | `0xC0` | yes |
| col ≡ 3 (mod 4) | 2 | `0x3F` | — | no |

Worked examples: col 0 → byte 2, `sh` 0. col 1 → byte 2, `sh` 6. col 2 → byte 3,
`sh` 4. col 3 → byte 4, `sh` 2. col 4 → byte 5, `sh` 0. col 79 → byte 61, `sh` 2.

Closed form, valid for all 80 columns:

```c
p    = 16u + 6u * col;
byte = p >> 3;               /* 2..61 */
sh   = p & 7u;               /* 0, 6, 4, 2 */
m0   = 0xFCu >> sh;
m1   = (sh > 2u) ? (u8)(0xFCu << (8u - sh)) : 0u;
```

The phase repeats every **4 columns** (3 bytes). Cell 79's last pixel is 495.

---

## 5. Font extraction

`tools/romfont.py` reads a TT3000 ROM image and emits `src/font_data.h`:

- source: 96 glyphs of 8 bytes at ROM offset `0x0A27`, covering 0x20–0x7F;
- emitted: 95 entries for 0x20–0x7E, matching the existing `FONT_ASCII` shape;
- transform: each byte `<< 1`, moving the 6-px cell from bits 6..1 to bits 7..2;
- ROM path from `--rom` or `ROMFONT` in the environment; no default that silently
  picks up a stray file.

The script is deterministic, so regeneration is verifiable in CI by re-running it
against a known ROM and diffing — but that check only runs when a ROM is present.

`tools/genfont.py` loses its `ASCII` table and keeps `GRAPH`, redrawn on a 6-px
grid: vertical runs in column 2, horizontal runs spanning all 6 px so lines join
across cells. `font.c` and `font.h` keep their current interface — `font_glyph()`
still returns 8 bytes and the 0xDF–0xFE graphics-page mapping is unchanged.

Font data size is unchanged: 95×8 + 32×8 = 1016 bytes.

---

## 6. Renderer

`render_flush()` no longer writes one byte per cell.

Per dirty row:

1. **Once per row**, collect the 80 glyph pointers and attributes. The naive
   alternative — looking glyphs up inside the scanline loop — would do 640 lookups
   per row instead of 80.
2. **Per scanline (8)**, walk 10 groups of 8 cells. Each group produces 6 bytes at
   `2 + 6*j`, which is 3 consecutive indices in each display file, so both files are
   written sequentially with no parity test in the loop.

Per half-group of 4 cells (glyph bytes `g0..g3`, already attribute-applied):

```c
b0 = (g0 & 0xFC) | (g1 >> 6);
b1 = ((g1 << 2) & 0xF0) | (g2 >> 4);
b2 = ((g2 << 4) & 0xC0) | (g3 >> 2);
```

All shifts are constants. **Verified**: this packer, the mask-based painter of §4,
and a reference painter that plots pixel by pixel produce identical 64-byte
scanlines over 2000 randomised rows.

`render_scroll_region()` is **unchanged** — it moves whole scanlines and is
indifferent to how cells are packed within them.

`render_cursor()` keeps its current role (XOR the cursor cell in place, after
`render_flush()` has painted the row) and uses the §4 span/mask helper, touching
one or two bytes. Folding the cursor into row rendering was considered and rejected:
it would change the `render.h` contract and `main.c`'s dirty-flag dance for no gain.

`render_cell_bytes()` keeps its PURE contract but now yields 6-px cells in bits
7..2.

New pure, host-testable helper (in `render.c`, exported for tests):

```c
void render_cell_span(u8 col, u8 *byte_idx, u8 *sh, u8 *mask0, u8 *mask1);
```

`hires.c` / `hires.h` are **unchanged**.

---

## 7. Changes outside the renderer

| File | Change |
|---|---|
| `include/screen.h` | `COLS` 64 → 80. `screen_t` grows 3072 → 3840 bytes. |
| `include/vtparse.h` | `VT_TAB_BYTES` follows `COLS`, 8 → 10 bytes. No code change. |
| `src/screen.c`, `src/vtparse.c` | None — all ~25 sites use `COLS`. |
| `terminfo/timex-vt102.terminfo` | `cols#64` → `cols#80`; description text. |
| `tools/zesarux_stdio_bridge.py` | `--cols` default 64 → 80. |
| `tools/if1_pty_bridge.py` | `--cols` default 64 → 80; docstring. |
| `src/main.c` | Demo stream box is 37 chars wide; widen to suit 80 columns. |
| `README.md` | Remove the two 64-column limitations; update the width claims. |
| `docs/*.png` | Screenshots become stale; retake. |

`screen_t` is a local in `main()`, so the extra 768 bytes land on the stack. Not
expected to be a problem, but **to confirm against `build/term.map`** during
implementation rather than assumed.

---

## 8. Risk: render throughput

Today a cell is one unconditional byte store. After this change each byte costs
roughly a shift, a mask and an OR, and there are 25% more cells. A row repaint is
expected to be **2–3× slower**.

Mitigations already in the design: glyph lookup hoisted out of the scanline loop
(§6), the 8-cell group unrolled, constant shifts throughout.

Per D16 this is accepted for now. If measurement shows it matters, the next step is
a Z80 assembly path for the `attr == 0` case, which is the overwhelming majority of
cells. Measure with `z88dk-ticks` before writing any of it.

Secondary risk: **legibility at 6 px**. The ROM squeezes capitals into 6 rows and
480 px now carries 80 characters instead of 64. Only verifiable by eye in ZEsarUX.

---

## 9. Licensing note

`TT3000.rom` is proprietary to Timex Portugal. This repository is MIT-licensed and
public, and D13 commits 760 bytes of glyph bitmaps extracted from that ROM.

The concern was raised during design and the decision to proceed was taken
deliberately by the project owner. Recording it here so it is not rediscovered as a
surprise: bitmap typefaces are generally held to be uncopyrightable in the United
States, while the position in the EU is less settled. Only the glyph bitmaps are
taken — no ROM code, and not the ROM image.

Should this ever need undoing, §3 records that repacking the existing `genfont.py`
glyphs at 6-px pitch is a complete substitute, needing only a redraw of `%`.

---

## 10. Testing

**Host tests** (`test/run.sh`, `cc -std=c99 -Wall -Wextra -Werror`):

- `test_render.c`
  - the 4-cell → 3-byte packer against a bit-by-bit reference painter, over all
    four phases;
  - `render_cell_span()` for `sh ∈ {0, 2, 4, 6}`, including both spilling cases;
  - cell 0 starts at byte 2, cell 79 ends within byte 61, margins untouched;
  - reverse and underline on a cell that spans two bytes.
- `test_font.c`
  - spot-check several ROM glyphs against known bitmaps;
  - **no glyph sets bits 1..0** — the check that would have caught the `%` defect
    of §3;
  - the DEC graphics page still maps 0xDF–0xFE correctly.
- `test_screen.c`, `test_vtparse.c` — existing cases re-checked at `COLS == 80`;
  wrap, tab stops and DECSTBM are the ones that actually depend on the width.
- `test_hires.c` — unchanged.

**Target verification** (ZEsarUX, TC2048):

- a DEC line-drawing box spanning the full 80 columns, joints included;
- `ls -l`, `man`, and an 80-column-assuming program through `make shell-zrcp`;
- reverse and underline where a cell straddles a byte boundary;
- the 16-px margins are visibly even.

---

## 11. Out of scope

- Optimising the renderer (D16).
- 64-column mode in any form.
- Colour, double-width/double-height, smooth scroll.
- The browser/JSSpeccy transport — tracked separately as issue #1.

---

## 12. Milestones

1. `tools/romfont.py` + generated `src/font_data.h` + font tests.
2. 6-px DEC graphics in `genfont.py`.
3. `render_cell_span()` and the packer as pure functions, with host tests, before
   `COLS` is touched — this proves the bit maths in isolation, so a later failure
   is known to be in the blit path rather than the arithmetic.
4. Flip `COLS` to 80, rewrite `render_flush()` and `render_cursor()`, fix the tests.
5. terminfo, bridges, demo stream, README.
6. ZEsarUX verification and new screenshots.
