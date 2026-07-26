# 80-Column Display With The TT3000 ROM Font — Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Move the terminal from 64×24 to 80×24 using a 6-px cell and the 6×8 font extracted from the Timex Terminal TT3000 ROM, and publish a browser live demo.

**Architecture:** At 6 px per cell a character no longer occupies exactly one byte, so the renderer packs 4 cells into 3 bytes with compile-time-constant shifts. The 80 cells are centred in the 512-px line with a 16-px (2-byte) margin each side, which keeps column 0 byte-aligned. `COLS` stays a compile-time constant, so `screen.c` and `vtparse.c` need no changes. Font data comes from a ROM extractor; DEC line-drawing glyphs are authored separately at 6 px.

**Tech Stack:** C99 (host `cc` for tests, z88dk/SDCC `+zx -clib=sdcc_iy` for the TAP), Python 3 for the font generators, ZEsarUX for target verification, GitHub Pages + a Timex-capable JSSpeccy 3 fork for the browser demo.

**Spec:** `docs/superpowers/specs/2026-07-26-80-column-rom-font-design.md`

## Global Constraints

- C99. Four-space indent, K&R braces, matching the existing sources.
- Fixed-width aliases from `include/types.h` only: `u8`, `s8`, `u16`, `s16`.
- Pass structs by pointer or out-pointer. **Never return a struct by value** — it crashes SDCC's z80 backend.
- No float, no `malloc`, no recursion, no headers that shadow z88dk system headers.
- Only hardware-facing code may include `<z80.h>`. Pure modules must compile on the host.
- Run the host tests with `make test` (Makefile:126 runs `sh test/run.sh`; the script is not executable, so invoking it directly fails). It compiles with `-std=c99 -Wall -Wextra -Werror`. Every task must leave it passing.
- When a task changes anything under `tools/` or `test/*.py`, run `make ci` as well — it adds `python3 -m py_compile` over those files plus a terminfo check.
- Commit subjects use `module: concise change`, imperative mood.
- Screen layout constants, fixed for the whole plan: **80 columns × 6 px**, **16 px left margin**, cells occupy scanline bytes **2–61**, file indices **1–30** in each display file.
- The ROM used for extraction is `~/TT3000/TT3000.rom`. It is **not** committed; the generated header is.

---

### Task 1: Extract the ROM font

**Files:**
- Create: `tools/romfont.py`
- Create: `src/font_ascii_data.h` (generated, committed)
- Modify: `src/font.c:9` (include split)
- Delete: `src/font_data.h`
- Test: `test/test_font.c`

**Interfaces:**
- Consumes: nothing from earlier tasks.
- Produces: `static const u8 FONT_ASCII[95][8]` in `src/font_ascii_data.h`, indexed by `ch - 0x20` for 0x20–0x7E. Each glyph's 6-px cell occupies bits 7..2; bits 1..0 are always zero. `font_glyph(u8 ch)` keeps its existing signature and returns 8 bytes.

- [ ] **Step 1: Write the failing test**

Replace `test_glyph_A` and `test_glyph_tilde` in `test/test_font.c`, and add the pitch-safety test. Keep every other test in that file untouched.

```c
/* 'A' from the TT3000 ROM: body in rows 1-6, cell in bits 7..2. */
static void test_glyph_A(void)
{
    const u8 *g = font_glyph(0x41);  /* 'A' */
    CHECK(g[0] == 0x00);  /* ...... */
    CHECK(g[1] == 0x70);  /* .###.. */
    CHECK(g[2] == 0x88);  /* #...#. */
    CHECK(g[3] == 0x88);  /* #...#. */
    CHECK(g[4] == 0xF8);  /* #####. */
    CHECK(g[5] == 0x88);  /* #...#. */
    CHECK(g[6] == 0x88);  /* #...#. */
    CHECK(g[7] == 0x00);  /* ...... */
}

/* '~' (0x7E) is the last ASCII entry. */
static void test_glyph_tilde(void)
{
    const u8 *g = font_glyph(0x7E);  /* '~' */
    CHECK(g[0] == 0x00);
    CHECK(g[1] == 0x50);  /* .#.#.. */
    CHECK(g[2] == 0xA0);  /* #.#... */
    CHECK(g[3] == 0x00);
}

/*
 * At 6-px pitch a cell owns bits 7..2 only. A glyph that lights bit 1 or bit 0
 * would bleed into its right-hand neighbour, which reads as a renderer bug
 * rather than a font bug. Check the whole ASCII page, not a sample.
 */
static void test_no_glyph_exceeds_six_pixels(void)
{
    unsigned code;

    for (code = 0x20u; code <= 0x7Eu; ++code) {
        const u8 *g = font_glyph((u8)code);
        u8 i;

        for (i = 0; i < 8u; ++i) {
            CHECK((g[i] & 0x03u) == 0);
        }
    }
}
```

Register the new test in `main()`:

```c
    test_no_glyph_exceeds_six_pixels();
```

`test/test_render.c` asserts the same glyph, so it changes here too. Update
`test_render_cell_normal` to the ROM bytes, and rewrite the two attribute tests to
assert *relative to the plain glyph* rather than against literals — that decouples
them from whichever font is loaded, so Task 4 will need to change one expression
each instead of eight:

```c
/* Normal cell: glyph bytes copied as-is. */
static void test_render_cell_normal(void)
{
    u8 out[8];
    render_cell_bytes('A', 0, out);

    /* 'A' from the TT3000 ROM: body in rows 1-6. */
    CHECK(out[0] == 0x00);
    CHECK(out[1] == 0x70);
    CHECK(out[2] == 0x88);
    CHECK(out[3] == 0x88);
    CHECK(out[4] == 0xF8);
    CHECK(out[5] == 0x88);
    CHECK(out[6] == 0x88);
    CHECK(out[7] == 0x00);
}

/* REVERSE inverts the glyph. */
static void test_render_cell_reverse(void)
{
    u8 plain[8];
    u8 rev[8];
    u8 i;

    render_cell_bytes('A', 0, plain);
    render_cell_bytes('A', ATTR_REVERSE, rev);

    for (i = 0; i < 8u; ++i) {
        CHECK(rev[i] == (u8)~plain[i]);
    }
}

/* UNDERLINE fills the bottom row and leaves the rest alone. */
static void test_render_cell_underline(void)
{
    u8 plain[8];
    u8 ul[8];
    u8 i;

    render_cell_bytes('A', 0, plain);
    render_cell_bytes('A', ATTR_UNDERLINE, ul);

    for (i = 0; i < 7u; ++i) {
        CHECK(ul[i] == plain[i]);
    }
    CHECK(ul[7] == 0xFF);
}

/* REVERSE + UNDERLINE: underline wins on the bottom row. */
static void test_render_cell_reverse_underline(void)
{
    u8 plain[8];
    u8 both[8];
    u8 i;

    render_cell_bytes('A', 0, plain);
    render_cell_bytes('A', (u8)(ATTR_REVERSE | ATTR_UNDERLINE), both);

    for (i = 0; i < 7u; ++i) {
        CHECK(both[i] == (u8)~plain[i]);
    }
    CHECK(both[7] == 0xFF);
}
```

- [ ] **Step 2: Run the test to verify it fails**

Run: `make test`
Expected: FAIL — `test_font` aborts on the `assert` inside `test_glyph_A`, because `FONT_ASCII` still holds the hand-authored 8-px glyphs (`g[0]` is `0x70`, not `0x00`).

- [ ] **Step 3: Write the extractor**

Create `tools/romfont.py`:

```python
#!/usr/bin/env python3
"""
romfont.py -- extract the 6x8 terminal font from a Timex Terminal TT3000 ROM.

Run:  python3 tools/romfont.py --rom ~/TT3000/TT3000.rom > src/font_ascii_data.h

The ROM holds 96 glyphs of 8 bytes at offset 0x0A27, covering 0x20-0x7F. Each
glyph's 6-pixel cell sits in bits 6..1, so we shift left by one: the cell then
occupies bits 7..2, bit 7 is the leftmost pixel (matching the rest of the
renderer), and every packing shift in render.c becomes a constant.

Only 0x20-0x7E is emitted, matching the shape FONT_ASCII already had.

The generated header is committed so CI and third parties can build without the
ROM, which is not redistributable.
"""
import argparse
import os
import sys

FONT_OFFSET = 0x0A27
FIRST_CODE = 0x20
LAST_CODE = 0x7E
GLYPH_BYTES = 8
COUNT = LAST_CODE - FIRST_CODE + 1


def read_rom(path):
    with open(path, "rb") as f:
        data = f.read()
    need = FONT_OFFSET + COUNT * GLYPH_BYTES
    if len(data) < need:
        sys.exit("ROM too short: %s is %d bytes, need at least %d" % (path, len(data), need))
    return data


def glyph(data, code):
    off = FONT_OFFSET + (code - FIRST_CODE) * GLYPH_BYTES
    return [(data[off + i] << 1) & 0xFF for i in range(GLYPH_BYTES)]


def check_pitch(data):
    """A glyph lighting bits 1..0 would collide with its right neighbour."""
    for code in range(FIRST_CODE, LAST_CODE + 1):
        if any(b & 0x03 for b in glyph(data, code)):
            sys.exit("glyph 0x%02X exceeds the 6-pixel cell (bits 1..0 set)" % code)


def emit(data):
    print("/* font_ascii_data.h -- GENERATED by tools/romfont.py; do not edit by hand. */")
    print("/* 6x8 glyphs from the Timex Terminal TT3000 ROM; cell in bits 7..2. */")
    print()
    print("static const u8 FONT_ASCII[%d][8] = {" % COUNT)
    for code in range(FIRST_CODE, LAST_CODE + 1):
        row = ", ".join("0x%02X" % b for b in glyph(data, code))
        label = "SP" if code == 0x20 else chr(code)
        print("    { %s },  /* 0x%02X %s */" % (row, code, label))
    print("};")


def main():
    ap = argparse.ArgumentParser(description="Extract the TT3000 6x8 font as C data.")
    ap.add_argument("--rom", default=os.environ.get("ROMFONT"),
                    help="path to the TT3000 ROM image (or set ROMFONT)")
    args = ap.parse_args()
    if not args.rom:
        sys.exit("no ROM given: pass --rom PATH or set ROMFONT")

    data = read_rom(args.rom)
    check_pitch(data)
    emit(data)


if __name__ == "__main__":
    main()
```

- [ ] **Step 4: Generate the header and split the includes**

Run:

```bash
python3 tools/romfont.py --rom ~/TT3000/TT3000.rom > src/font_ascii_data.h
```

Split the old combined header. `src/font_data.h` currently holds both tables; move its `FONT_GRAPH` table verbatim into a new `src/font_graph_data.h`, keeping its generated-file banner, then delete `src/font_data.h`.

In `src/font.c`, replace the single include at line 9:

```c
/* Generated glyph data. */
#include "font_ascii_data.h"
#include "font_graph_data.h"
```

- [ ] **Step 5: Run the tests to verify they pass**

Run: `make test`
Expected: PASS, ending in `ALL HOST TESTS PASSED`. Both `test_font` and
`test_render` now assert the ROM glyph.

- [ ] **Step 6: Commit**

```bash
git add tools/romfont.py src/font_ascii_data.h src/font_graph_data.h src/font.c test/test_font.c test/test_render.c
git rm src/font_data.h
git commit -m "font: extract the 6x8 ASCII font from the TT3000 ROM

Replaces the hand-authored 8x8 glyphs. tools/romfont.py reads the ROM,
shifts each byte left by one so the 6-px cell lands in bits 7..2, and
refuses to emit a glyph that lights bits 1..0 -- at 6-px pitch that would
bleed into the next cell.

The generated header is committed so CI builds without the ROM. Font data
is now split: ASCII from the ROM, DEC graphics from genfont.py.

host TDD"
```

---

### Task 2: Redraw the DEC graphics at 6 px

**Files:**
- Modify: `tools/genfont.py` (drop `ASCII`, redraw `GRAPH` on a 6-px grid)
- Modify: `src/font_graph_data.h` (regenerated)
- Test: `test/test_font.c`

**Interfaces:**
- Consumes: `font_glyph()` and the `FONT_GRAPH` layout from Task 1.
- Produces: `static const u8 FONT_GRAPH[32][8]` in `src/font_graph_data.h`, indexed by `(ch & 0x7F) - 0x5F` for cell codes 0xDF–0xFE. Line constants: vertical run in column 2 (`0x20`), horizontal run across all six columns (`0xFC`), left half `0xE0`, right half `0x3C`. `0xE0 | 0x3C == 0xFC`, so halves join cleanly.

- [ ] **Step 1: Write the failing test**

Replace `test_glyph_graph_lines` and `test_glyph_graph_0x7E` in `test/test_font.c`:

```c
/* DEC line drawing at 6-px pitch: vertical in column 2, horizontal across all six. */
static void test_glyph_graph_lines(void)
{
    static const u8 horiz[8] = { 0, 0, 0, 0xFC, 0, 0, 0, 0 };
    static const u8 vert[8]  = { 0x20, 0x20, 0x20, 0x20, 0x20, 0x20, 0x20, 0x20 };
    static const u8 ul[8]    = { 0, 0, 0, 0x3C, 0x20, 0x20, 0x20, 0x20 };
    static const u8 ur[8]    = { 0, 0, 0, 0xE0, 0x20, 0x20, 0x20, 0x20 };
    static const u8 ll[8]    = { 0x20, 0x20, 0x20, 0x3C, 0, 0, 0, 0 };
    static const u8 lr[8]    = { 0x20, 0x20, 0x20, 0xE0, 0, 0, 0, 0 };

    check_glyph(font_glyph(0xF1), horiz);  /* q: horizontal */
    check_glyph(font_glyph(0xF8), vert);   /* x: vertical */
    check_glyph(font_glyph(0xEC), ul);     /* l: upper-left */
    check_glyph(font_glyph(0xEB), ur);     /* k: upper-right */
    check_glyph(font_glyph(0xED), ll);     /* m: lower-left */
    check_glyph(font_glyph(0xEA), lr);     /* j: lower-right */
}

/* The half-lines must combine into a full run, or boxes show gaps at joints. */
static void test_graph_half_lines_join(void)
{
    const u8 *left  = font_glyph(0xEA);  /* j: lower-right corner, left half */
    const u8 *right = font_glyph(0xED);  /* m: lower-left corner, right half */

    CHECK((u8)(left[3] | right[3]) == 0xFC);
}

/* Cross and tees carry the vertical through every row. */
static void test_glyph_graph_cross(void)
{
    static const u8 cross[8] = { 0x20, 0x20, 0x20, 0xFC, 0x20, 0x20, 0x20, 0x20 };
    check_glyph(font_glyph(0xEE), cross);  /* n: crossing lines */
}

/* DEC graphics 0x7E: centred dot. */
static void test_glyph_graph_0x7E(void)
{
    const u8 *g = font_glyph(0xFE);
    CHECK(g[0] == 0x00);
    CHECK(g[1] == 0x00);
    CHECK(g[2] == 0x00);
    CHECK(g[3] == 0x30);  /* ..##.. */
    CHECK(g[4] == 0x30);  /* ..##.. */
    CHECK(g[5] == 0x00);
}
```

Register the two new tests in `main()`:

```c
    test_graph_half_lines_join();
    test_glyph_graph_cross();
```

`test/test_render.c` asserts the same horizontal line through
`render_cell_bytes`. In `test_render_cell_graphics`, change `CHECK(out[3] == 0xFF);`
to:

```c
    CHECK(out[3] == 0xFC);  /* horizontal run spans the 6-px cell */
```

- [ ] **Step 2: Run the test to verify it fails**

Run: `make test`
Expected: FAIL in `test_font` — `FONT_GRAPH` still holds 8-px art, so `horiz[3]` is `0xFF` rather than `0xFC`.

- [ ] **Step 3: Rewrite the generator**

In `tools/genfont.py`: delete the entire `ASCII` dictionary, update the module docstring, and replace the graphics section and `main()`. The `row_byte` and `emit_table` helpers stay as they are — `row_byte` already walks only the characters present, so 6-character rows produce bits 7..2.

```python
# DEC Special Graphics on a 6-px cell (bits 7..2). The line-drawing glyphs use
# the VT-100 letters: j/k/l/m corners, q horizontal, x vertical, n/t/u/v/w joins.
# The vertical run sits in column 2 and the horizontal run spans all six columns,
# so runs meet across cell boundaries and the half-lines OR back to a full run.
BLANK6 = ["......"] * 8
VLINE = "..#..."    # 0x20
HLINE = "######"    # 0xFC
HLEFT = "###..."    # 0xE0 -- up to and including the vertical column
HRIGHT = "..####"   # 0x3C -- from the vertical column rightwards

GRAPH = {
    0x60: [                       # diamond
        "......",
        "......",
        "..#...",
        ".###..",
        "..#...",
        "......",
        "......",
        "......",
    ],
    0x61: [                       # checkerboard
        "#.#.#.",
        ".#.#.#",
        "#.#.#.",
        ".#.#.#",
        "#.#.#.",
        ".#.#.#",
        "#.#.#.",
        ".#.#.#",
    ],
    0x66: [                       # degree sign
        "..##..",
        ".#..#.",
        ".#..#.",
        "..##..",
        "......",
        "......",
        "......",
        "......",
    ],
    0x67: [                       # plus/minus
        "..#...",
        ".####.",
        "..#...",
        "......",
        ".####.",
        "......",
        "......",
        "......",
    ],
    0x6A: [VLINE, VLINE, VLINE, HLEFT, *BLANK6[4:]],                     # lower-right
    0x6B: [*BLANK6[:3], HLEFT, VLINE, VLINE, VLINE, VLINE],              # upper-right
    0x6C: [*BLANK6[:3], HRIGHT, VLINE, VLINE, VLINE, VLINE],             # upper-left
    0x6D: [VLINE, VLINE, VLINE, HRIGHT, *BLANK6[4:]],                    # lower-left
    0x6E: [VLINE, VLINE, VLINE, HLINE, VLINE, VLINE, VLINE, VLINE],      # crossing
    0x6F: [HLINE, *BLANK6[1:]],                                          # scan 1
    0x70: [*BLANK6[:2], HLINE, *BLANK6[3:]],                             # scan 3
    0x71: [*BLANK6[:3], HLINE, *BLANK6[4:]],                             # scan 5
    0x72: [*BLANK6[:5], HLINE, *BLANK6[6:]],                             # scan 7
    0x73: [*BLANK6[:7], HLINE],                                          # scan 9
    0x74: [VLINE, VLINE, VLINE, HRIGHT, VLINE, VLINE, VLINE, VLINE],     # left tee
    0x75: [VLINE, VLINE, VLINE, HLEFT, VLINE, VLINE, VLINE, VLINE],      # right tee
    0x76: [VLINE, VLINE, VLINE, HLINE, *BLANK6[4:]],                     # bottom tee
    0x77: [*BLANK6[:3], HLINE, VLINE, VLINE, VLINE, VLINE],              # top tee
    0x78: [VLINE] * 8,                                                   # vertical
    0x7E: [                       # centred dot
        "......",
        "......",
        "......",
        "..##..",
        "..##..",
        "......",
        "......",
        "......",
    ],
}


def main():
    print("/* font_graph_data.h -- GENERATED by tools/genfont.py; do not edit by hand. */")
    print("/* DEC special graphics on a 6-px cell (bits 7..2), bit 7 leftmost. */")
    print()
    emit_table("FONT_GRAPH", 0x5F, 0x7E, lambda c: GRAPH.get(c), BLANK6)
```

Update the docstring's run line to `python3 tools/genfont.py > src/font_graph_data.h`.

- [ ] **Step 4: Regenerate and run the tests**

Run:

```bash
python3 tools/genfont.py > src/font_graph_data.h
make test
```

Expected: PASS, ending in `ALL HOST TESTS PASSED`.

- [ ] **Step 5: Commit**

```bash
git add tools/genfont.py src/font_graph_data.h test/test_font.c
git commit -m "font: redraw DEC special graphics on a 6-px cell

The TT3000 ROM's frame set is double-line and VT-100 special graphics is
single-line, so the box-drawing glyphs are authored here rather than
extracted. Vertical runs sit in column 2, horizontal runs span all six
columns, and the half-lines OR back to a full run so joints close.

genfont.py loses its ASCII table -- that now comes from the ROM.

host TDD"
```

---

### Task 3: Cell geometry helper

**Files:**
- Modify: `include/render.h` (add layout constants and the declaration)
- Modify: `src/render.c` (add the function)
- Test: `test/test_render.c`

**Interfaces:**
- Consumes: nothing from earlier tasks.
- Produces:
  ```c
  #define RENDER_LEFT_MARGIN_PX 16u
  #define RENDER_CELL_PX 6u
  void render_cell_span(u8 col, u8 *byte_idx, u8 *sh, u8 *mask0, u8 *mask1);
  ```
  For column `col`, `*byte_idx` is the scanline byte holding the cell's leftmost pixel (2–61), `*sh` the left shift applied to a glyph byte (0, 2, 4 or 6), `*mask0` the bits owned inside `*byte_idx`, and `*mask1` the bits owned inside `*byte_idx + 1` (zero when the cell does not spill). Task 5 uses this for cursor inversion.

This task deliberately lands **before** `COLS` changes, so a later failure is known to be in the blit path rather than the arithmetic.

- [ ] **Step 1: Write the failing test**

Append to `test/test_render.c`:

```c
/*
 * Cell geometry. 80 cells of 6 px are centred in the 512-px line, so cell 0
 * starts at pixel 16 (byte 2) and cell 79 ends at pixel 495 (byte 61). The bit
 * phase repeats every four columns; two of the four phases spill into the next
 * byte.
 */
static void test_cell_span_phases(void)
{
    u8 b, sh, m0, m1;

    render_cell_span(0, &b, &sh, &m0, &m1);
    CHECK(b == 2 && sh == 0 && m0 == 0xFC && m1 == 0x00);

    render_cell_span(1, &b, &sh, &m0, &m1);
    CHECK(b == 2 && sh == 6 && m0 == 0x03 && m1 == 0xF0);

    render_cell_span(2, &b, &sh, &m0, &m1);
    CHECK(b == 3 && sh == 4 && m0 == 0x0F && m1 == 0xC0);

    render_cell_span(3, &b, &sh, &m0, &m1);
    CHECK(b == 4 && sh == 2 && m0 == 0x3F && m1 == 0x00);

    /* The phase, not the byte, is what repeats: cell 4 is byte 5, phase 0. */
    render_cell_span(4, &b, &sh, &m0, &m1);
    CHECK(b == 5 && sh == 0 && m0 == 0xFC && m1 == 0x00);
}

/* The last cell must stay inside the line, leaving bytes 62 and 63 as margin. */
static void test_cell_span_last_column(void)
{
    u8 b, sh, m0, m1;

    render_cell_span(79, &b, &sh, &m0, &m1);
    CHECK(b == 61 && sh == 2 && m0 == 0x3F && m1 == 0x00);
}

/* Every cell owns exactly six pixels, and never reaches past byte 61. */
static void test_cell_span_covers_six_pixels(void)
{
    u8 col;

    for (col = 0; col < 80u; ++col) {
        u8 b, sh, m0, m1;
        u8 bits = 0;
        u8 i;

        render_cell_span(col, &b, &sh, &m0, &m1);
        for (i = 0; i < 8u; ++i) {
            if (m0 & (u8)(1u << i)) { ++bits; }
            if (m1 & (u8)(1u << i)) { ++bits; }
        }
        CHECK(bits == 6);
        CHECK(b >= 2u);
        CHECK((m1 != 0) ? (b + 1u <= 61u) : (b <= 61u));
    }
}
```

Register them in `main()`:

```c
    test_cell_span_phases();
    test_cell_span_last_column();
    test_cell_span_covers_six_pixels();
```

- [ ] **Step 2: Run the test to verify it fails**

Run: `make test`
Expected: FAIL at compile time — `error: implicit declaration of function 'render_cell_span'`, which `-Werror` turns fatal.

- [ ] **Step 3: Write the implementation**

In `include/render.h`, after the `ATTR_*` defines:

```c
/*
 * Screen geometry. 80 cells of 6 px are centred in the 512-px hi-res line:
 * a 16-px (2-byte) margin each side, cells occupying scanline bytes 2..61.
 * The margin is chosen byte-aligned so column 0 starts on a byte boundary.
 */
#define RENDER_LEFT_MARGIN_PX 16u
#define RENDER_CELL_PX        6u

/*
 * Where cell `col` lives inside a scanline.
 * byte_idx: scanline byte (0..63) holding the cell's leftmost pixel.
 * sh:       right shift taking a glyph byte (cell in bits 7..2) into place.
 * mask0:    bits this cell owns inside byte_idx.
 * mask1:    bits it owns inside byte_idx + 1; zero when the cell does not spill.
 *
 * PURE logic: host-tested, must not include z80.h.
 */
void render_cell_span(u8 col, u8 *byte_idx, u8 *sh, u8 *mask0, u8 *mask1);
```

In `src/render.c`, after `render_cell_bytes`:

```c
/* PURE: locate one cell inside a scanline. */
void render_cell_span(u8 col, u8 *byte_idx, u8 *sh, u8 *mask0, u8 *mask1)
{
    u16 px = (u16)(RENDER_LEFT_MARGIN_PX + RENDER_CELL_PX * (u16)col);
    u8 s = (u8)(px & 7u);

    *byte_idx = (u8)(px >> 3);
    *sh = s;
    *mask0 = (u8)(0xFCu >> s);
    *mask1 = (s > 2u) ? (u8)((0xFCu << (8u - s)) & 0xFFu) : 0u;
}
```

- [ ] **Step 4: Run the tests to verify they pass**

Run: `make test`
Expected: PASS, ending in `ALL HOST TESTS PASSED`.

- [ ] **Step 5: Commit**

```bash
git add include/render.h src/render.c test/test_render.c
git commit -m "render: add 6-px cell geometry helper

render_cell_span maps a character column to its scanline byte, shift and
bit masks. 80 cells of 6 px centred in 512 px put cell 0 at byte 2 and
cell 79 at byte 61, with bytes 0-1 and 62-63 as margin.

Lands before COLS changes so the bit maths is proven in isolation.

host TDD"
```

---

### Task 4: The 4-cells-into-3-bytes packer

**Files:**
- Modify: `include/render.h` (declaration)
- Modify: `src/render.c` (implementation, and mask `render_cell_bytes` to the cell)
- Test: `test/test_render.c`

**Interfaces:**
- Consumes: `RENDER_LEFT_MARGIN_PX`, `RENDER_CELL_PX`, `render_cell_span()` from Task 3.
- Produces: `void render_pack4(const u8 *g, u8 *out3);` — takes four glyph bytes (cell in bits 7..2, attributes already applied) and writes the three scanline bytes they occupy. Task 5 calls this twice per group of eight cells.

- [ ] **Step 1: Write the failing test**

Append to `test/test_render.c`. The reference painter is deliberately naive — plotting one pixel at a time is obviously correct, which is what makes it worth testing the fast path against.

```c
/*
 * Reference painter: plot each cell's six pixels one at a time into a 64-byte
 * scanline. Slow and obviously correct; the packer must agree with it.
 */
static void paint_reference(const u8 *glyph_row, u8 ncols, u8 line[64])
{
    u8 col, k, i;

    for (i = 0; i < 64u; ++i) {
        line[i] = 0;
    }
    for (col = 0; col < ncols; ++col) {
        u16 px = (u16)(RENDER_LEFT_MARGIN_PX + RENDER_CELL_PX * (u16)col);

        for (k = 0; k < 6u; ++k) {
            if (glyph_row[col] & (u8)(0x80u >> k)) {
                u16 q = (u16)(px + k);
                line[q >> 3] |= (u8)(0x80u >> (q & 7u));
            }
        }
    }
}

/* The packer must reproduce the reference painter for every bit phase. */
static void test_pack4_matches_reference(void)
{
    /* Four glyph bytes with distinct bit patterns inside the 6-px cell. */
    static const u8 g[4] = { 0xFC, 0xA8, 0x54, 0x84 };
    u8 want[64];
    u8 got[3];

    paint_reference(g, 4, want);
    render_pack4(g, got);

    CHECK(got[0] == want[2]);
    CHECK(got[1] == want[3]);
    CHECK(got[2] == want[4]);
}

/* A single lit cell must not disturb its neighbours' bytes. */
static void test_pack4_isolation(void)
{
    static const u8 only_second[4] = { 0x00, 0xFC, 0x00, 0x00 };
    u8 got[3];

    render_pack4(only_second, got);

    /* Cell 1 is phase 6: two bits low in byte 2, four bits high in byte 3. */
    CHECK(got[0] == 0x03);
    CHECK(got[1] == 0xF0);
    CHECK(got[2] == 0x00);
}

/* All cells fully lit must fill bytes 2..4 completely -- no gaps between cells. */
static void test_pack4_no_gaps(void)
{
    static const u8 all_on[4] = { 0xFC, 0xFC, 0xFC, 0xFC };
    u8 got[3];

    render_pack4(all_on, got);

    CHECK(got[0] == 0xFF);
    CHECK(got[1] == 0xFF);
    CHECK(got[2] == 0xFF);
}

/*
 * No attribute may light bits 1..0 -- those belong to the cell on the right.
 * Reverse is the dangerous one: inverting a whole byte sets them every time.
 */
static void test_attributes_stay_inside_the_cell(void)
{
    static const u8 attrs[3] = { ATTR_REVERSE, ATTR_UNDERLINE,
                                 (u8)(ATTR_REVERSE | ATTR_UNDERLINE) };
    u8 a, i;

    for (a = 0; a < 3u; ++a) {
        u8 out[8];

        render_cell_bytes('A', attrs[a], out);
        for (i = 0; i < 8u; ++i) {
            CHECK((out[i] & 0x03u) == 0);
        }
    }
}
```

Register them in `main()`:

```c
    test_pack4_matches_reference();
    test_pack4_isolation();
    test_pack4_no_gaps();
    test_attributes_stay_inside_the_cell();
```

Three existing tests need their expectations narrowed to the 6-px cell. Because
Task 1 rewrote the attribute tests to compare against the plain glyph, each needs a
single expression changed:

- `test_render_cell_reverse`: `rev[i] == (u8)~plain[i]` becomes
  `rev[i] == (u8)(~plain[i] & 0xFCu)`.
- `test_render_cell_underline`: `ul[7] == 0xFF` becomes `ul[7] == 0xFC`.
- `test_render_cell_reverse_underline`: both changes — the loop body becomes
  `both[i] == (u8)(~plain[i] & 0xFCu)` and `both[7] == 0xFF` becomes
  `both[7] == 0xFC`.

- [ ] **Step 2: Run the test to verify it fails**

Run: `make test`
Expected: FAIL at compile time — `error: implicit declaration of function 'render_pack4'`.

- [ ] **Step 3: Write the implementation**

In `include/render.h`, after the `render_cell_span` declaration:

```c
/*
 * Pack four cells into the three scanline bytes they occupy.
 * g:    four glyph bytes for one scanline, cell in bits 7..2, attributes applied.
 * out3: the three bytes, in left-to-right scanline order.
 *
 * 4 cells x 6 px = 24 px = exactly 3 bytes, so every shift here is a constant.
 * PURE logic: host-tested, must not include z80.h.
 */
void render_pack4(const u8 *g, u8 *out3);
```

In `src/render.c`, replace `render_cell_bytes` and add the packer:

```c
/* PURE: one glyph byte for one scanline, with attributes applied to the cell. */
static u8 glyph_row_byte(const u8 *glyph, u8 attr, u8 scanline)
{
    u8 g = glyph[scanline];

    if (attr & ATTR_REVERSE) {
        /* Invert only the six pixels this cell owns. */
        g = (u8)(~g & 0xFCu);
    }
    if ((attr & ATTR_UNDERLINE) && scanline == 7u) {
        g = 0xFCu;
    }
    return g;
}

/* PURE: produce 8 pixel rows for one cell. */
void render_cell_bytes(u8 ch, u8 attr, u8 out[8])
{
    const u8 *glyph = font_glyph(ch);
    u8 i;

    for (i = 0; i < 8; ++i) {
        out[i] = glyph_row_byte(glyph, attr, i);
    }
}

/* PURE: 4 cells (24 px) pack into 3 bytes with constant shifts. */
void render_pack4(const u8 *g, u8 *out3)
{
    out3[0] = (u8)((g[0] & 0xFCu) | (g[1] >> 6));
    out3[1] = (u8)(((g[1] << 2) & 0xF0u) | (g[2] >> 4));
    out3[2] = (u8)(((g[2] << 4) & 0xC0u) | (g[3] >> 2));
}
```

- [ ] **Step 4: Run the tests to verify they pass**

Run: `make test`
Expected: PASS, ending in `ALL HOST TESTS PASSED`.

- [ ] **Step 5: Commit**

```bash
git add include/render.h src/render.c test/test_render.c
git commit -m "render: pack four 6-px cells into three bytes

24 px is exactly 3 bytes, so the packer needs only constant shifts. Tested
against a naive pixel-by-pixel reference painter across all four bit
phases.

Attributes now apply to the 6-px cell rather than the whole byte: reverse
masks to 0xFC and underline fills 0xFC, so neither bleeds into the
neighbouring cell.

host TDD"
```

---

### Task 5: Switch to 80 columns

**Files:**
- Modify: `include/screen.h:18` (`COLS`)
- Modify: `src/render.c` (`render_row_fast`, `render_cursor`)
- Test: `test/test_vtparse.c:86`

**Interfaces:**
- Consumes: `render_pack4()` (Task 4), `render_cell_span()` (Task 3), the ROM font (Task 1) and 6-px graphics (Task 2).
- Produces: `COLS == 80u`; `screen_t` grows from 3072 to 3840 bytes of cells. No public signature changes.

- [ ] **Step 1: Fix the width-dependent test first**

`test/test_vtparse.c:86` seeds column 60 to check that HT clamps at the line end. With 80 columns there is still a tab stop at 64, so the test would stop checking what it was written to check. Column `COLS - 4` has no stop beyond it at either width:

```c
    screen_cup(&s, 2, COLS - 4);
    vt_feed(&vt, &s, 0x09);
    CHECK(s.cx == COLS - 1);
```

- [ ] **Step 2: Run the tests to confirm they still pass at 64 columns**

Run: `make test`
Expected: PASS. This proves the test edit is width-neutral before the width changes.

- [ ] **Step 3: Flip COLS and update the comments around it**

In `include/screen.h`, line 18 and the module comment at the top:

```c
#define COLS 80u
```

The header comment says "A 64x24 grid of character cells"; change it to "An 80x24 grid of character cells".

- [ ] **Step 4: Run the tests to see the renderer break**

Run: `make test`
Expected: PASS for `test_screen` and `test_vtparse` (they are written against `COLS`), but the renderer is now wrong on target — it still writes one byte per cell and would run off the end of the display file. Nothing catches that on the host, which is why the next step is not optional.

- [ ] **Step 5: Rewrite the row blitter**

In `src/render.c`, replace `render_cell_to` and `render_row_fast` entirely with:

```c
/*
 * Blit one text row.
 *
 * Glyph lookup is hoisted out of the scanline loop on purpose: doing it inside
 * would cost 640 lookups per row instead of 80.
 *
 * Cells are walked in groups of eight because eight cells span six bytes, which
 * is three consecutive indices in each display file -- so both files are written
 * sequentially with no parity test in the inner loop.
 */
static const u8 *row_glyphs[COLS];
static u8 row_attrs[COLS];

static void render_row_fast(const screen_t *s, u8 r)
{
    u8 *even_dst[8];
    u8 *odd_dst[8];
    const cell_t *cell = &s->cells[r][0];
    u8 col, i, j;

    for (col = 0; col < COLS; ++col) {
        row_glyphs[col] = font_glyph(cell->ch);
        row_attrs[col] = cell->attr;
        ++cell;
    }

    for (i = 0; i < 8u; ++i) {
        u8 prow = (u8)((r << 3) + i);
        u16 off = ((u16)(prow & 0xC0u) << 5)
                | ((u16)(prow & 0x07u) << 8)
                | ((u16)(prow & 0x38u) << 2);
        even_dst[i] = (u8 *)(uintptr_t)(HIRES_FILE0 + off);
        odd_dst[i] = (u8 *)(uintptr_t)(HIRES_FILE1 + off);
    }

    for (i = 0; i < 8u; ++i) {
        u8 *ev = even_dst[i];
        u8 *od = odd_dst[i];

        for (j = 0; j < 10u; ++j) {
            u8 base = (u8)(j * 8u);   /* first cell of this group          */
            u8 fi = (u8)(1u + j * 3u); /* first file index of this group    */
            u8 g[8];
            u8 packed[3];
            u8 k;

            for (k = 0; k < 8u; ++k) {
                g[k] = glyph_row_byte(row_glyphs[base + k], row_attrs[base + k], i);
            }

            /* Scanline bytes 2+6j .. 4+6j. */
            render_pack4(&g[0], packed);
            ev[fi] = packed[0];
            od[fi] = packed[1];
            ev[fi + 1u] = packed[2];

            /* Scanline bytes 5+6j .. 7+6j. */
            render_pack4(&g[4], packed);
            od[fi + 1u] = packed[0];
            ev[fi + 2u] = packed[1];
            od[fi + 2u] = packed[2];
        }
    }
}
```

- [ ] **Step 6: Rewrite the cursor**

Still in `src/render.c`, replace `render_cursor`:

```c
/* Hardware: invert the six pixels of the cursor cell, in place. */
void render_cursor(const screen_t *s)
{
    u8 byte_idx, sh, mask0, mask1;
    u8 i;

    if (!(s->mode & MODE_CURSOR_VISIBLE)) {
        return;
    }

    /* The masks alone describe what to invert; the shift is not needed here. */
    render_cell_span(s->cx, &byte_idx, &sh, &mask0, &mask1);

    for (i = 0; i < 8u; ++i) {
        u8 prow = (u8)(s->cy * 8u + i);
        u8 *p = (u8 *)(uintptr_t)hires_addr(byte_idx, prow);

        *p ^= mask0;
        if (mask1 != 0) {
            u8 *q = (u8 *)(uintptr_t)hires_addr((u8)(byte_idx + 1u), prow);
            *q ^= mask1;
        }
    }
}
```

XOR replaces the previous read-complement-write: it inverts exactly the cell's own pixels and leaves the neighbouring bits in the same byte alone, which the old whole-byte write could not do.

- [ ] **Step 7: Run the tests**

Run: `make test`
Expected: PASS, ending in `ALL HOST TESTS PASSED`.

- [ ] **Step 8: Build the TAP and check the memory map**

Run:

```bash
make tap
grep -E "_row_glyphs|_row_attrs" build/term.map
```

Expected: `make tap` succeeds and both symbols appear. `screen_t` grew by 768 bytes and lives on the stack as a local in `main()`; confirm the build still links and note the top-of-BSS address from the map. If the link fails for space, move `screen_t scr` in `src/main.c:252` from a local to a file-scope `static` and rerun.

- [ ] **Step 9: Commit**

```bash
git add include/screen.h src/render.c test/test_vtparse.c
git commit -m "screen: switch the terminal to 80 columns

COLS 64 -> 80. It stays a compile-time constant, so screen.c and vtparse.c
follow without edits; screen_t grows from 3072 to 3840 bytes of cells.

render_row_fast now walks cells in groups of eight -- six bytes, three
consecutive indices in each display file -- and packs them four at a time.
Glyph lookup is hoisted out of the scanline loop to keep it at 80 lookups
per row rather than 640.

The cursor XORs its six pixels through render_cell_span instead of writing
whole bytes, so it no longer disturbs the cells sharing its bytes.

The HT clamp test moves to COLS - 4, which has no tab stop beyond it at
either width."
```

---

### Task 6: Tell the world it is 80 columns

**Files:**
- Modify: `terminfo/timex-vt102.terminfo:1,3`
- Modify: `tools/zesarux_stdio_bridge.py:519`
- Modify: `tools/if1_pty_bridge.py:6,35,39`
- Modify: `src/main.c:25-46` (demo stream)
- Modify: `README.md`

**Interfaces:**
- Consumes: `COLS == 80` from Task 5.
- Produces: no code interfaces. The PTY is sized 80×24 and `timex-vt102` advertises `cols#80`.

- [ ] **Step 1: Update the terminfo entry**

In `terminfo/timex-vt102.terminfo`, line 1 and line 3:

```text
timex-vt102|tc2048-vt102|tc2068-vt102|Timex 2048/2068 80-column VT102 terminal emulator,
```

```text
	cols#80, it#8, lines#24,
```

- [ ] **Step 2: Update both bridges**

`tools/zesarux_stdio_bridge.py:519`:

```python
    parser.add_argument("--cols", type=int, default=80)
```

`tools/if1_pty_bridge.py:39`:

```python
    parser.add_argument("--cols", type=int, default=80)
```

And the two prose references in the same file — the module docstring at line 6 and the `description=` at line 35 — change `64x24` to `80x24`.

- [ ] **Step 3: Widen the startup screen**

In `src/main.c`, the demo stream draws a 37-character box that now sits in the left half of an 80-column screen. Replace the box and the lines that follow it (lines 26–46) with:

```c
static const u8 demo_stream[] =
    "\x1b[2J"
    "\x1b[H"
    "\x1b(0"
    "lqqqqqqqqqqqqqqqqqqqqqqqqqqqqqqqqqqqqqqqqqqqqqqqqqqqqqqqqqqqqqqqqqqqqqqqqqqqqqqk\r\n"
    "x VT-102 TERMINAL EMULATOR                         80 columns, TT3000 6x8 font x\r\n"
    "x   \x1b[7mREVERSE\x1b[0m \x1b[4mUNDERLINE\x1b[0m test                                                     x\r\n"
    "mqqqqqqqqqqqqqqqqqqqqqqqqqqqqqqqqqqqqqqqqqqqqqqqqqqqqqqqqqqqqqqqqqqqqqqqqqqqqqqj"
    "\x1b(B"
    "\x1b[5;1H"
    "Version v" APP_VERSION_STR "  " APP_GIT_COMMIT "\r\n"
    "Built " APP_BUILD_DATE "\r\n"
    "\r\n"
    "Bridge quick help:\r\n"
    "  macOS -> Timex: pipe text through bridge.\r\n"
    "  Timex -> macOS: type here; bridge writes stdout.\r\n"
    "  make bridge-zrcp: immediate keys + local echo.\r\n"
    "  For text files, use --input-newline crlf.\r\n"
    "  ENTER sends CR. CAPS+0 sends Ctrl-H backspace.\r\n"
    "  SYMBOL+0 sends underscore (_). Raw mode is optional.\r\n"
    "\r\n"
    "Ready.";
```

The box is exactly 80 characters wide (`l`, 78 `q`, `k`) and each inner line is 80 visible
columns once the SGR escapes are discounted. Do not count by eye — verify:

```sh
python3 - <<'EOF'
import re
for s in open('src/main.c'):
    s = s.strip()
    if s.startswith('"') and ('lq' in s or s.startswith('"x ') or 'qj' in s):
        v = re.sub(r'\\x1b\[[0-9;]*[A-Za-z]', '', s).strip('"').replace('\\r\\n', '')
        print(len(v), repr(v[:20]))
EOF
```

Every box line must print 80. An off-by-one wraps the line and scrolls the screen on boot,
which reads as a renderer fault rather than a miscounted string.

- [ ] **Step 4: Update the README**

In `README.md`:

- line 5–6, the summary: `512x192 video for a 64x24 text display` becomes `512x192 video for an 80x24 text display`.
- line 105: `COLUMNS=64` becomes `COLUMNS=80`.
- line 132: `64-column terminfo entry` becomes `80-column terminfo entry`.
- line 202: delete the bullet `- The display is 64x24, not 80x24. Use timex-vt102 terminfo for best results.` and replace with `- The display is 80x24 monochrome. Use timex-vt102 terminfo for best results.`
- line 207: delete the bullet `- Programs with dense 80-column layouts may run but will not look good.`
- line 155: `sets the terminal size to 64x24` becomes `sets the terminal size to 80x24`.
- line 338: `cols#64` becomes `cols#80`.
- line 348–351: the `vt102` note says the system entry "usually advertises 80 columns" as a drawback; that is no longer a mismatch, so change it to note that the system `vt102` entry is now a reasonable fallback.
- Add a line under **Notes** recording the font: `- The 6x8 font is extracted from the Timex Terminal TT3000 ROM by tools/romfont.py; DEC line-drawing glyphs are authored in tools/genfont.py.`

- [ ] **Step 5: Verify the build and tests**

Run:

```bash
make test
make tap
```

Expected: host tests pass; `make tap` produces `build/term.tap`.

- [ ] **Step 6: Commit**

```bash
git add terminfo/timex-vt102.terminfo tools/zesarux_stdio_bridge.py tools/if1_pty_bridge.py src/main.c README.md
git commit -m "terminfo: advertise 80 columns across terminfo, bridges and docs

Both PTY bridges now size the terminal 80x24 and timex-vt102 carries
cols#80. The startup box is widened to the full 80 columns.

Drops the two README limitations this work existed to remove: the 64x24
display and dense 80-column layouts looking bad."
```

---

### Task 7: Browser live demo

**Files:**
- Create: `site/index.html`
- Create: `.github/workflows/pages.yml`
- Modify: `README.md`

**Interfaces:**
- Consumes: `build/term.tap` from `make tap` (Task 5).
- Produces: a GitHub Pages site at `https://dtz-labs.github.io/timex-vt100-emulator/`.

This mirrors the working setup in the sibling `attribute-raid` repository: the page embeds the `dtz-labs/jsspeccy3` fork, which adds the Timex machines that stock JSSpeccy 3 lacks.

There is no host behind the browser build, but the demo is still live rather than a screenshot: with `CONN_ZRCP_BRIDGE_ENABLE` clear, `conn_poll()` in `src/conn.c:359` moves the TX ring straight into RX, so keys typed on the emulated keyboard travel the full VT path and come back on screen. Connecting it to a real host is issue #1.

- [ ] **Step 1: Write the page**

Create `site/index.html`:

```html
<!DOCTYPE html>
<html lang="en">
<head>
<meta charset="utf-8">
<meta name="viewport" content="width=device-width, initial-scale=1">
<title>Timex VT102 Terminal — run it in the browser</title>
<style>
  body {
    background: #101010;
    color: #d8d8d8;
    font-family: "Courier New", monospace;
    margin: 0;
    padding: 1.5rem 1rem 3rem;
    text-align: center;
  }
  h1 { color: #ffff55; font-size: 1.5rem; margin: 0 0 0.25rem; }
  p  { margin: 0.4rem auto; max-width: 46rem; line-height: 1.45; }
  a  { color: #55ffff; }
  #jsspeccy {
    margin: 1.25rem auto;
    display: flex;
    justify-content: center;
  }
  .keys { color: #55ff55; }
  .hint { color: #ffff55; }
  .footer { margin-top: 1.5rem; font-size: 0.85rem; color: #909090; }
</style>
</head>
<body>
<h1>TIMEX VT102 TERMINAL</h1>
<p>A VT102 terminal for Timex 2048/2068-class machines, in
<strong>80&times;24</strong> monochrome text on Timex hi-res 512&times;192 video,
with the 6&times;8 font from the Timex Terminal TT3000 ROM. This page always runs
the latest TAP built from
<a href="https://github.com/dtz-labs/timex-vt100-emulator">master</a> on an
emulated <strong>Timex TC2048</strong>.
<span class="hint">Press the &#9654; button on the display to start.</span></p>
<p>There is no host at the other end here, so the terminal runs in loopback: what
you type goes through the keyboard mapping, out to the connection layer, back in,
and through the VT parser onto the screen. That exercises the real path &mdash;
it is a working terminal talking to itself. Attaching it to a shell over a
WebSocket is
<a href="https://github.com/dtz-labs/timex-vt100-emulator/issues/1">issue&nbsp;1</a>.</p>
<p class="keys">ENTER — CR &nbsp; CAPS+0 — backspace &nbsp; SYMBOL+SPACE — ESC
&nbsp; CAPS+SYMBOL+letter — Ctrl-letter</p>
<div id="jsspeccy"></div>
<p>Timex video needs our <a href="https://github.com/dtz-labs/jsspeccy3">fork of
JSSpeccy&nbsp;3</a>, which adds the TC2048 and TC2068; stock JSSpeccy has no
Timex video modes. TAPs are also in the
<a href="https://github.com/dtz-labs/timex-vt100-emulator/releases">releases</a>.</p>
<p class="footer">Emulation by
<a href="https://github.com/gasman/jsspeccy3">JSSpeccy&nbsp;3</a> (GPL-3), in the
<a href="https://github.com/dtz-labs/jsspeccy3">dtz-labs fork</a> with Timex support.</p>
<script src="jsspeccy/jsspeccy.js"></script>
<script>
JSSpeccy(document.getElementById('jsspeccy'), {
    machine: 2048,
    openUrl: 'term.tap',
    autoLoadTapes: true,
    // No autoStart: JSSpeccy builds its AudioContext inside start() and never
    // calls resume(), so starting from the visible play button keeps it inside
    // a user gesture -- the only way browsers allow audio (the terminal's BEL).
    zoom: 2
});
</script>
</body>
</html>
```

- [ ] **Step 2: Write the workflow**

Create `.github/workflows/pages.yml`:

```yaml
name: pages

on:
  push:
    branches:
      - master
  workflow_dispatch:

permissions:
  contents: read
  pages: write
  id-token: write

concurrency:
  group: ${{ github.workflow }}
  cancel-in-progress: true

env:
  # bump to move to a newer emulator build
  JSSPECCY_RELEASE: v3.2.0-timex.1

jobs:
  deploy:
    name: deploy browser terminal
    runs-on: ubuntu-latest
    environment:
      name: github-pages
      url: ${{ steps.deployment.outputs.page_url }}
    steps:
      - uses: actions/checkout@v4

      # Same pattern as release.yml: z88dk runs in a container, but the job
      # itself stays on the runner so gh, unzip and the Pages actions are all
      # still available.
      - name: Build the TAP with z88dk
        run: |
          set -eu
          docker run --rm -v "$PWD":/src -w /src \
            -e BUILD_DATE="$(date -u +%Y-%m-%dT%H:%M:%SZ)" \
            -e GIT_COMMIT="${GITHUB_SHA::12}" \
            z88dk/z88dk:latest make tap

      # Stock JSSpeccy 3 has no Timex machines, so the hi-res display has
      # nowhere to run in a browser; this pulls a tagged build of the fork
      # that adds them.
      - name: Fetch the emulator build
        env:
          GH_TOKEN: ${{ github.token }}
        run: |
          set -eu
          mkdir -p _emulator
          gh release download "$JSSPECCY_RELEASE" \
            --repo dtz-labs/jsspeccy3 \
            --pattern jsspeccy-dist.zip \
            --output _emulator/jsspeccy-dist.zip
          unzip -q _emulator/jsspeccy-dist.zip -d _emulator/dist

      - name: Assemble the site
        run: |
          set -eu
          mkdir -p _site
          cp -R site/. _site/
          cp -R _emulator/dist/jsspeccy _site/jsspeccy
          cp build/term.tap _site/term.tap

      - name: Check the pieces the page needs are all there
        run: |
          set -eu
          test -f _site/index.html
          test -f _site/term.tap
          test -f _site/jsspeccy/jsspeccy.js
          test -f _site/jsspeccy/jsspeccy-core.wasm
          test -f _site/jsspeccy/roms/tc2048.rom
          test -f _site/jsspeccy/tapeloaders/tape_2048.szx

      - name: Upload Pages artifact
        uses: actions/upload-pages-artifact@v4
        with:
          path: _site

      - name: Deploy to GitHub Pages
        id: deployment
        uses: actions/deploy-pages@v4
```

- [ ] **Step 3: Add the README annotation**

In `README.md`, immediately after the CI badge line (line 3) and before the summary paragraph:

```markdown
**[▶ Run the terminal in your browser](https://dtz-labs.github.io/timex-vt100-emulator/)** —
every push to `master` deploys the current TAP to GitHub Pages, where it runs on
an emulated Timex TC2048 in the [JSSpeccy 3](https://github.com/gasman/jsspeccy3)
fork that adds Timex video. There is no host behind it, so the terminal runs in
loopback: type and the bytes come back through the VT parser onto the screen.
Attaching it to a real shell is [issue #1](https://github.com/dtz-labs/timex-vt100-emulator/issues/1).
```

- [ ] **Step 4: Validate what can be validated locally**

Run:

```bash
python3 -c "import pathlib,html.parser as h; p=h.HTMLParser(); p.feed(pathlib.Path('site/index.html').read_text()); print('index.html parses')"
python3 -c "import sys,pathlib; d=pathlib.Path('.github/workflows/pages.yml').read_text(); assert 'term.tap' in d and 'tc2048.rom' in d; print('workflow references the right artifacts')"
make tap
test -f build/term.tap && echo "TAP built"
```

Expected: all three print their confirmation. The workflow itself can only be proven by pushing — note that GitHub Pages must be enabled for the repository with source "GitHub Actions", which is a one-time setting in repository settings.

- [ ] **Step 5: Commit**

```bash
git add site/index.html .github/workflows/pages.yml README.md
git commit -m "pages: publish a browser live demo of the terminal

Deploys the current TAP to GitHub Pages on every push to master, running
on an emulated TC2048 in the dtz-labs JSSpeccy 3 fork -- stock JSSpeccy
has no Timex video modes.

No host is attached, so the terminal runs in loopback: conn_poll moves TX
straight into RX when the bridge flag is clear, which means typed keys
travel the real keyboard -> conn -> VT parser -> renderer path. Attaching
it to a shell is issue #1.

Mirrors the working setup in attribute-raid."
```

---

### Task 8: Verify on the target

**Files:**
- Modify: `test/zesarux_smoke.py:73-85,95-112` (cell reader and expectations)
- Modify: `docs/uname_and_gcc.png`, `docs/man_ed.png`, `docs/cowsay.png` (retaken)
- Modify: `README.md` (only if a screenshot caption no longer matches)

**Interfaces:**
- Consumes: everything above.
- Produces: evidence that the display is correct on real Timex video, plus refreshed screenshots.

Host tests cannot see the display file, so this task is where the renderer is actually proven.

- [ ] **Step 1: Boot the TAP and check the startup screen**

Run:

```bash
make tap
make run-zrcp
```

Check, in the ZEsarUX window:
- the box drawn with DEC graphics spans the full width with no gap at any joint;
- the left and right margins are visibly equal, about two characters' worth in total;
- the box does not wrap or scroll the screen — if it does, the `l`/`q`/`k` count in `src/main.c` is wrong;
- REVERSE and UNDERLINE on line 3 stay inside their own cells and do not smear into neighbours.

- [ ] **Step 2: Drive it from a shell**

In a second terminal:

```bash
make install-terminfo
make shell-zrcp ZRCP_TERM=timex-vt102 ZRCP_CMD='zsh -l'
```

Then, in the Timex session, check:
- `stty size` reports `24 80`;
- `ls -l /usr/bin | head -40` fills the width without wrapping short lines;
- `man ed` paginates and the reverse-video status line at the bottom is intact;
- the cursor is visible and lands on the right cell — cursor drift is the symptom of a wrong `render_cell_span`.

- [ ] **Step 3: Check the cursor at every bit phase**

The cursor spans two bytes in half of all columns. In the Timex session:

```sh
printf 'ABCDEFGH\r'
```

then walk the cursor left and right across those eight columns with the arrow chords (`CAPS+SYMBOL+5` and `CAPS+SYMBOL+8`) and confirm at each stop that exactly one character is inverted, with no bleed into either neighbour.

- [ ] **Step 4: Rewrite the smoke test's cell reader**

`test/zesarux_smoke.py` reads cells straight out of video RAM and compares them to
expected glyph bytes. Its `glyph_addrs()` at line 73 assumes one cell is one byte
(`base = 0x6000 if (col & 1) else 0x4000`), which is exactly the assumption this
work removes — it must be replaced, not adjusted.

Replace `glyph_addrs` and `cell_bytes` (lines 73–85) with:

```python
LEFT_MARGIN_PX = 16
CELL_PX = 6


def cell_span(col):
    """Where cell `col` lives in a scanline: byte index, shift, and bit masks.

    Mirrors render_cell_span() in src/render.c. At 6 px a cell no longer owns a
    whole byte, so reading one back means masking it out of one or two bytes.
    """
    px = LEFT_MARGIN_PX + CELL_PX * col
    sh = px & 7
    mask0 = 0xFC >> sh
    mask1 = (0xFC << (8 - sh)) & 0xFF if sh > 2 else 0
    return px >> 3, sh, mask0, mask1


def byte_addr(byte_idx, prow):
    """Address of scanline byte `byte_idx` (0..63) on pixel row `prow`."""
    base = 0x6000 if (byte_idx & 1) else 0x4000
    off = ((prow & 0xC0) << 5) | ((prow & 0x07) << 8) | ((prow & 0x38) << 2)
    return base + off + (byte_idx >> 1)


def cell_bytes(s, row, col):
    """Read one cell's 8 glyph rows back, re-aligned into bits 7..2."""
    byte_idx, sh, mask0, mask1 = cell_span(col)
    out = []
    for i in range(8):
        prow = row * 8 + i
        g = ((rdbytes(s, byte_addr(byte_idx, prow), 1)[0] & mask0) << sh) & 0xFF
        if mask1:
            g |= (rdbytes(s, byte_addr(byte_idx + 1, prow), 1)[0] & mask1) >> (8 - sh)
        out.append(g)
    return bytes(out)
```

Then replace the `checks` tuple (lines 95–112) with the ROM font and 6-px graphics
expectations. The box is now 80 wide, so the upper-right corner moves from column
36 to column 79; the text rows are unchanged because the startup stream keeps the
same line structure.

```python
    checks = (
        ("upper-left l", 0, 0, bytes([0x00, 0x00, 0x00, 0x3C, 0x20, 0x20, 0x20, 0x20])),
        ("horizontal q", 0, 1, bytes([0x00, 0x00, 0x00, 0xFC, 0x00, 0x00, 0x00, 0x00])),
        (
            "upper-right k",
            0,
            79,
            bytes([0x00, 0x00, 0x00, 0xE0, 0x20, 0x20, 0x20, 0x20]),
        ),
        ("help B", 7, 0, bytes([0x00, 0xF0, 0x88, 0xF0, 0x88, 0x88, 0xF0, 0x00])),
        (
            "help ENTER E",
            12,
            2,
            bytes([0x00, 0xF8, 0x80, 0xF0, 0x80, 0x80, 0xF8, 0x00]),
        ),
        ("ready R", 15, 0, bytes([0x00, 0xF0, 0x88, 0x88, 0xF0, 0x90, 0x88, 0x00])),
    )
```

Note the deliberate spread of columns: 0 and 79 are phase 0 and phase 2 (single
byte), column 1 is phase 6 and column 2 is phase 4 — both of which straddle two
bytes. If the packer got a phase wrong, at least one of these six mismatches.

- [ ] **Step 5: Run the smoke test**

Run:

```bash
ZRCP_PORT=10140 SMOKE_WAIT=12 make smoke
```

Expected: PASS, with all six cells reporting `MATCH` and a final
`VERDICT: PASS - startup help rendered on TC2048`.

- [ ] **Step 6: Retake the screenshots**

Capture the three README images again at 80 columns, keeping the same subjects so the captions stay true: `uname` + `gcc` output, `man ed`, and `cowsay`. Save over `docs/uname_and_gcc.png`, `docs/man_ed.png`, `docs/cowsay.png`.

- [ ] **Step 7: Commit**

```bash
git add docs/*.png README.md test/zesarux_smoke.py
git commit -m "docs: retake screenshots at 80 columns

Verified on ZEsarUX TC2048: DEC box drawing joins across the full width,
margins are even, stty reports 24 80, and the cursor inverts exactly one
cell at every bit phase including the columns that straddle two bytes."
```

---

## Notes for whoever executes this

**Do the tasks in order.** Tasks 1, 2 and 4 each touch `test/test_render.c`, and the plan assumes the earlier edits are in place — Task 4's one-expression changes only make sense against the glyph-relative assertions Task 1 introduces.

**The renderer has no host test for the blit itself.** `render_flush` writes to absolute addresses `0x4000`/`0x6000`, so it only runs on target. That is why Tasks 3 and 4 push as much logic as possible into pure functions with reference-comparison tests: everything left in `render_row_fast` is addressing, and addressing is what Task 8 checks by eye.

**Do not optimise while implementing.** The spec accepts a 2–3× slower row repaint (D16). If it turns out to matter, measure with `z88dk-ticks` first and put the assembly fast path behind the `attr == 0` case, which is the overwhelming majority of cells.
