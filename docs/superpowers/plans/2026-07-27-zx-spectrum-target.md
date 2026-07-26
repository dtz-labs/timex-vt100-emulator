# ZX Spectrum 40-Column Target Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Ship a second TAP that runs the terminal on a stock ZX Spectrum at
40×24 on ULA graphics, built from the same source tree as the 80×24 Timex TAP.

**Architecture:** `COLS` stays a compile-time constant chosen by a single machine
define. `render.c` keeps only geometry-free pure code; each geometry gets its own
pure module and its own hardware blitter, and the Makefile links exactly one of
each pair. Before the second blitter is written, the first is made fast, so the
new one is written once in its final shape.

**Tech Stack:** C99 with z88dk (`zcc +zx -SO3 -clib=sdcc_iy`), host unit tests
with `cc`, ZEsarUX + ZRCP for target verification, Python 3 stdlib for tooling.

**Prerequisite:** `docs/superpowers/plans/2026-07-27-image-limit-gate-and-im2-relocation.md`
must be complete and merged. This plan grows the image; without that gate the
growth is unchecked, and without the relocation the margin is about 1.4 KB.

**Phases.** Tasks 1–5 are the Timex-side rework (no ZX code yet, existing build
stays green and gets faster). Tasks 6–7 are machine detection. Tasks 8–9 are the
ZX target proper. Tasks 10–11 are host integration and verification.

## Global Constraints

- C99. Four-space indent, K&R braces, matching existing sources.
- Fixed-width aliases from `include/types.h`: `u8`, `s8`, `u16`, `s16`.
- No float, no `malloc`, no recursion, no headers that shadow z88dk system headers.
- Pointer-oriented APIs: pass structs by pointer or out-pointer. **Never return a
  struct by value** — it crashes SDCC's z80 backend.
- Only hardware modules may include `z80.h`. Pure modules compile on the host and
  are host-tested.
- Host tests build with `cc -std=c99 -Wall -Wextra -Werror`. Tests use plain
  `assert` plus a local `CHECK` counter; keep cases explicit and deterministic.
- Exactly one of `-DTERM_TIMEX` / `-DTERM_ZX` is defined for every compilation,
  host and target alike. `COLS` derives from it; width is never passed separately.
- Timex geometry: 80 cells, 6 px, 16 px margin, scanline bytes 2..61 across two
  display files (`0x4000` even byte columns, `0x6000` odd).
- ZX geometry: 40 cells, 6 px, 8 px margin, scanline bytes 1..30 of one display
  file at `0x4000`; attributes at `0x5800` filled once with `0x47`.
- Four cells = 24 px = exactly 3 bytes in both geometries. This is the unit for
  packing *and* for dirty tracking.
- Commit after every task. Run `make test` before every commit.

---

### Task 1: Split render.c into pure and hardware halves

`render.c` documents a PURE/hardware split in its own header comment and then
holds both sides. Every later task depends on that split being real: the pure
geometry must exist twice (once per machine) while the shared core exists once.

**Files:**
- Modify: `src/render.c` (keep only the geometry-free pure code)
- Create: `src/render_hires.c` (pure, Timex geometry)
- Create: `src/blit_hires.c` (hardware, Timex)
- Create: `src/video_hires.c` (hardware, Timex — moved from `src/video.c`)
- Delete: `src/video.c`
- Modify: `include/render.h`, `include/video.h`
- Create: `include/blit.h`
- Modify: `test/run.sh`, `Makefile`

**Interfaces:**
- Consumes: nothing.
- Produces:
  - `render.c`: `void render_cell_bytes(u8 ch, u8 attr, u8 out[8]);`
    `void render_pack4(const u8 *g, u8 *out3);`
    `u8 render_glyph_row_byte(const u8 *glyph, u8 attr, u8 scanline);` (was the
    static `glyph_row_byte`; now shared, so it must be exported)
  - `render_hires.c`: `void render_cell_span(u8 col, u8 *byte_idx, u8 *sh, u8 *mask0, u8 *mask1);`
    `void render_row_bytes(const u8 *g, u8 *ev, u8 *od);`
  - `blit.h`: `void blit_flush(screen_t *s);` `void blit_cursor(const screen_t *s);`
    `u8 blit_scroll_region(screen_t *s, u8 top, u8 bot, s8 n);`
  - `video.h`: `void video_init(u8 wob);` `void video_clear(void);`

- [ ] **Step 1: Write the failing test**

Append to `test/test_render.c`, inside its existing `main()` before the summary
print, a call to a new test function; and add the function above `main()`:

```c
static void test_glyph_row_byte_is_shared(void)
{
    /* render_glyph_row_byte was static; it must now be callable so both
     * geometries can use one implementation. Row 1 of 'A' is 0x70 in the ROM
     * font; reverse inverts only the six owned pixels. */
    const u8 *a = font_glyph('A');

    CHECK(render_glyph_row_byte(a, 0, 1) == a[1]);
    CHECK(render_glyph_row_byte(a, ATTR_REVERSE, 1) == (u8)(~a[1] & 0xFCu));
    CHECK(render_glyph_row_byte(a, ATTR_UNDERLINE, 7) == 0xFCu);
    CHECK((render_glyph_row_byte(a, ATTR_REVERSE, 3) & 0x03u) == 0u);
}
```

- [ ] **Step 2: Run the test to verify it fails**

```bash
cd /Volumes/SSD/Programowanie/timex-vt100-emulator-feat-zx-spectrum-target
make test
```

Expected: compile error, `implicit declaration of function 'render_glyph_row_byte'`.

- [ ] **Step 3: Perform the split**

In `include/render.h`, keep `render_cell_bytes`, `render_pack4` and add:

```c
/*
 * One glyph byte for one scanline with attributes applied to the cell.
 * Shared by both geometries, so it lives here rather than being static.
 * PURE logic: host-tested, must not include z80.h.
 */
u8 render_glyph_row_byte(const u8 *glyph, u8 attr, u8 scanline);
```

Move `render_cell_span` and `render_row_bytes` declarations out of `render.h`
into a new `include/render_geom.h`, which both geometry modules implement:

```c
/*
 * render_geom.h -- the geometry-dependent half of the renderer.
 *
 * Exactly one implementation is linked: src/render_hires.c for the Timex
 * 80-column build, src/render_ula.c for the ZX 40-column build. They export the
 * same names, so they are alternatives and are never linked together.
 * PURE logic: host-tested, must not include z80.h.
 */
#ifndef RENDER_GEOM_H
#define RENDER_GEOM_H

#include "types.h"

/* Where cell `col` lives inside a scanline. byte_idx: scanline byte holding the
 * cell's leftmost pixel. sh: right shift taking a glyph byte (cell in bits 7..2)
 * into place. mask0: bits owned inside byte_idx. mask1: bits owned inside
 * byte_idx + 1, zero when the cell does not spill. */
void render_cell_span(u8 col, u8 *byte_idx, u8 *sh, u8 *mask0, u8 *mask1);

/* Pack one full scanline of COLS attribute-applied glyph bytes into the
 * destination row(s). The hi-res build writes two 32-byte display-file rows
 * (ev, od); the ULA build writes one and ignores `od`. */
void render_row_bytes(const u8 *g, u8 *ev, u8 *od);

#endif /* RENDER_GEOM_H */
```

Move the bodies of `render_cell_span` and `render_row_bytes`, and the
`RENDER_LEFT_MARGIN_PX` / `RENDER_CELL_PX` defines, from `src/render.c` into a
new `src/render_hires.c`. Rename `glyph_row_byte` to `render_glyph_row_byte`,
remove its `static`, and leave it in `src/render.c` with `render_cell_bytes` and
`render_pack4`.

Move `render_flush`, `render_row_fast`, `row_scanline_offset`,
`scroll_file_up_one`, `scroll_file_down_one`, `render_scroll_region` and
`render_cursor` from `src/render.c` into a new `src/blit_hires.c`, renaming the
three public ones to `blit_flush`, `blit_scroll_region`, `blit_cursor`. Create
`include/blit.h` declaring those three with the comments moved from `render.h`.

Rename `src/video.c` to `src/video_hires.c` and rename `video_hires_on` to
`video_init` in it and in `include/video.h`, keeping `video_mode_byte` exported
so its existing behaviour stays testable.

Update `src/main.c`: include `blit.h`, call `video_init(1)`, `blit_flush`,
`blit_cursor`. Update `src/vtparse.c` if it calls `render_scroll_region` — it
calls it through `screen.c`'s scroll bookkeeping, so check with
`grep -rn "render_scroll_region\|render_flush\|render_cursor" src/`.

- [ ] **Step 4: Update the host test wiring**

In `test/run.sh`, change the render line to link the split modules and define the
machine, and add the two-pass structure that Task 8 will extend:

```sh
TERM_DEF="-DTERM_TIMEX"

$CC $CFLAGS $TERM_DEF "$ROOT/test/test_render.c" "$ROOT/src/render.c" \
    "$ROOT/src/render_hires.c" "$ROOT/src/font.c" "$ROOT/src/hires.c" \
    -o "$OUT/test_render"
"$OUT/test_render"
```

Add `$TERM_DEF` to the `test_screen` and `test_vtparse` lines too, since Task 8
makes `screen.h` require it.

In the `Makefile`, replace `SOURCES := $(sort $(wildcard src/*.c))` with an
explicit list, because the wildcard would sweep both geometries into one build:

```make
# Explicit, not a wildcard: render_hires.c/render_ula.c and blit_hires.c/
# blit_ula.c are ALTERNATIVES exporting the same symbols. A wildcard would link
# both and fail on duplicate symbols.
COMMON_SOURCES := src/conn.c src/font.c src/keybuf.c src/keymap.c src/main.c \
	src/render.c src/screen.c src/vtparse.c
TIMEX_SOURCES := $(COMMON_SOURCES) src/hires.c src/render_hires.c \
	src/blit_hires.c src/video_hires.c
SOURCES := $(TIMEX_SOURCES)
```

- [ ] **Step 5: Run the tests to verify they pass**

```bash
make test
```

Expected: every existing test passes plus the new `render_glyph_row_byte` checks.
The check count printed by `test_render` rises by 4.

- [ ] **Step 6: Verify the target build is unchanged in behaviour**

```bash
make tap
make smoke
```

Expected: both pass. This task moved code between files and renamed three
functions; it must not change a single rendered pixel.

- [ ] **Step 7: Commit**

```bash
git add -A
git commit -m "render: split the pure core from the geometry and the hardware

render.c documented a PURE/hardware split and held both halves. The pure,
geometry-free core stays; the 80-column geometry moves to render_hires.c
behind render_geom.h, the display writes move to blit_hires.c behind blit.h,
and video.c becomes video_hires.c.

SOURCES stops being a wildcard: the geometry modules are alternatives that
export the same symbols, so a wildcard would link both.

No behaviour change."
```

---

### Task 2: Dirty tracking in four-cell groups

One boolean per row means one changed character repaints 80 cells: 80 glyph
lookups and 512 video stores. Four cells is the packer's natural unit, so
group-level tracking costs nothing to translate into blits.

**Files:**
- Modify: `include/screen.h`, `src/screen.c`, `src/vtparse.c`, `src/blit_hires.c`,
  `src/main.c`, `test/test_screen.c`

**Interfaces:**
- Consumes: `blit_flush`, `blit_scroll_region` from Task 1.
- Produces, in `screen.h`:
  ```c
  #define DIRTY_GROUP_COLS 4u
  #define DIRTY_GROUPS ((COLS + DIRTY_GROUP_COLS - 1u) / DIRTY_GROUP_COLS)
  #define DIRTY_BYTES ((DIRTY_GROUPS + 7u) / 8u)
  void screen_mark_cell(screen_t *s, u8 row, u8 col);
  void screen_mark_span(screen_t *s, u8 row, u8 c0, u8 c1);
  void screen_mark_row(screen_t *s, u8 row);
  void screen_clear_marks(screen_t *s, u8 row);
  u8 screen_group_dirty(const screen_t *s, u8 row, u8 group);
  u8 screen_row_dirty(const screen_t *s, u8 row);
  ```
  `screen_t.dirty` changes from `u8 dirty[ROWS]` to `u8 dirty[ROWS][DIRTY_BYTES]`.

- [ ] **Step 1: Write the failing test**

Add to `test/test_screen.c`, above `main()`, and call it from `main()`:

```c
static void test_dirty_groups(void)
{
    screen_t s;
    u8 g;

    screen_init(&s);
    for (g = 0; g < DIRTY_GROUPS; ++g) {
        screen_clear_marks(&s, 0);
    }
    CHECK(screen_row_dirty(&s, 0) == 0);

    /* One character dirties exactly one group. Column 5 is in group 1. */
    screen_cup(&s, 0, 5);
    screen_putc(&s, 'X');
    CHECK(screen_row_dirty(&s, 0) != 0);
    CHECK(screen_group_dirty(&s, 0, 1) != 0);
    CHECK(screen_group_dirty(&s, 0, 0) == 0);
    CHECK(screen_group_dirty(&s, 0, 2) == 0);

    /* A span marks every group it touches and none beyond. Columns 4..9 span
     * groups 1 and 2. */
    screen_clear_marks(&s, 1);
    screen_mark_span(&s, 1, 4, 9);
    CHECK(screen_group_dirty(&s, 1, 0) == 0);
    CHECK(screen_group_dirty(&s, 1, 1) != 0);
    CHECK(screen_group_dirty(&s, 1, 2) != 0);
    CHECK(screen_group_dirty(&s, 1, 3) == 0);

    /* The last column falls in the last group, with nothing past it. */
    screen_clear_marks(&s, 2);
    screen_mark_cell(&s, 2, COLS - 1u);
    CHECK(screen_group_dirty(&s, 2, DIRTY_GROUPS - 1u) != 0);
    CHECK(screen_row_dirty(&s, 2) != 0);

    /* screen_init must leave every row fully dirty: a fresh screen paints once. */
    screen_init(&s);
    CHECK(screen_row_dirty(&s, 0) != 0);
    CHECK(screen_group_dirty(&s, 0, 0) != 0);
    CHECK(screen_group_dirty(&s, 0, DIRTY_GROUPS - 1u) != 0);
}

static void test_scroll_migrates_dirty_groups(void)
{
    screen_t s;
    u8 r;

    screen_init(&s);
    for (r = 0; r < ROWS; ++r) {
        screen_clear_marks(&s, r);
    }

    /* Dirty one group on row 5 only, then scroll the whole screen up one. The
     * mark must travel with the content to row 4, not be dropped or smeared. */
    screen_mark_cell(&s, 5, 5);
    screen_scroll(&s, 1);

    CHECK(screen_group_dirty(&s, 4, 1) != 0);
    /* The row that moved away is now row 4's old content; row 5 received row 6,
     * which was clean, but the freed bottom row must be dirty because it was
     * blanked. */
    CHECK(screen_row_dirty(&s, ROWS - 1u) != 0);
}
```

- [ ] **Step 2: Run the test to verify it fails**

```bash
make test
```

Expected: compile error, `DIRTY_GROUPS` undeclared and
`screen_mark_cell` implicitly declared.

- [ ] **Step 3: Implement the group API**

In `include/screen.h`, replace the `dirty` field and add the API:

```c
/*
 * Dirty tracking granularity. Four cells is 24 px is exactly three scanline
 * bytes in BOTH geometries, so it is the packer's natural unit and translates
 * into blits without arithmetic. One boolean per row would repaint COLS cells
 * for a one-character change.
 */
#define DIRTY_GROUP_COLS 4u
#define DIRTY_GROUPS ((COLS + DIRTY_GROUP_COLS - 1u) / DIRTY_GROUP_COLS)
#define DIRTY_BYTES ((DIRTY_GROUPS + 7u) / 8u)
```

and in `screen_t`:

```c
    u8 dirty[ROWS][DIRTY_BYTES];  /* per-row bitmap of dirty four-cell groups */
```

Declare the six functions with comments. In `src/screen.c`:

```c
void screen_mark_cell(screen_t *s, u8 row, u8 col)
{
    u8 g = (u8)(col / DIRTY_GROUP_COLS);

    s->dirty[row][g >> 3] |= (u8)(1u << (g & 7u));
}

void screen_mark_span(screen_t *s, u8 row, u8 c0, u8 c1)
{
    u8 g = (u8)(c0 / DIRTY_GROUP_COLS);
    u8 last = (u8)(c1 / DIRTY_GROUP_COLS);

    for (; g <= last; ++g) {
        s->dirty[row][g >> 3] |= (u8)(1u << (g & 7u));
    }
}

void screen_mark_row(screen_t *s, u8 row)
{
    screen_mark_span(s, row, 0, (u8)(COLS - 1u));
}

void screen_clear_marks(screen_t *s, u8 row)
{
    u8 b;

    for (b = 0; b < DIRTY_BYTES; ++b) {
        s->dirty[row][b] = 0;
    }
}

u8 screen_group_dirty(const screen_t *s, u8 row, u8 group)
{
    return (u8)(s->dirty[row][group >> 3] & (u8)(1u << (group & 7u)));
}

u8 screen_row_dirty(const screen_t *s, u8 row)
{
    u8 b, any = 0;

    for (b = 0; b < DIRTY_BYTES; ++b) {
        any |= s->dirty[row][b];
    }
    return any;
}
```

Replace every existing assignment:

| Site | Was | Becomes |
|---|---|---|
| `screen.c:15` (init) | `s->dirty[r] = 1;` | `screen_mark_row(s, r);` |
| `screen.c:41` (putc) | `s->dirty[s->cy] = 1;` | `screen_mark_cell(s, s->cy, s->cx);` |
| `screen.c:125` (scroll) | `s->dirty[r] = 1;` | `screen_mark_row(s, r);` |
| `screen.c:177` (blank_cells) | `s->dirty[row] = 1;` | `screen_mark_span(s, row, (u8)c0, (u8)c1);` |
| `screen.c:243` (insert_chars) | `s->dirty[row] = 1;` | `screen_mark_span(s, (u8)row, (u8)cx, (u8)(COLS - 1u));` |
| `screen.c:262` (delete_chars) | `s->dirty[row] = 1;` | `screen_mark_span(s, (u8)row, (u8)cx, (u8)(COLS - 1u));` |
| `vtparse.c:105` | `s->dirty[s->cy] = 1;` | `screen_mark_cell(s, s->cy, s->cx);` |
| `vtparse.c:414` | `s->dirty[r] = 1;` | `screen_mark_row(s, r);` |

**Note on `screen_putc`:** mark *before* the cursor advances, so the mark lands
on the cell just written. Read `screen.c:39-50` and place the call immediately
after the two cell assignments.

In `src/blit_hires.c`, change `blit_flush` to skip clean rows via
`screen_row_dirty` and clear with `screen_clear_marks`. Rendering only the dirty
groups is Task 5; for now keep the whole-row repaint so this task changes only
the representation:

```c
void blit_flush(screen_t *s)
{
    u8 r;

    for (r = 0; r < ROWS; ++r) {
        if (!screen_row_dirty(s, r)) {
            continue;
        }
        blit_row_full(s, r);
        screen_clear_marks(s, r);
    }
}
```

- [ ] **Step 4: Remove the dirty-flag clearing from the scroll**

This is §7 item 7 of the spec and a prerequisite for Task 4, not an optional
extra. In `src/blit_hires.c`, `blit_scroll_region` currently ends with a loop
clearing `s->dirty[row]` for the whole region. **Delete that loop.** Clearing it
discards changes that were never written to video RAM; `main.c` compensates today
by re-dirtying the bottom row, and Task 4 removes that compensation.

- [ ] **Step 5: Migrate the marks through the model scroll**

In `src/screen.c`'s `scroll_region`, the `screen_mark_row` loop currently dirties
the whole region unconditionally. Replace it so marks travel with content: move
each surviving row's `dirty` bytes alongside its cells, and fully mark only the
blanked rows.

```c
    if (n > 0) {                       /* scroll up: content moves toward top */
        for (r = top; r <= bot - absn; ++r) {
            for (c = 0; c < (int)COLS; ++c) {
                s->cells[r][c] = s->cells[r + absn][c];
            }
            for (c = 0; c < (int)DIRTY_BYTES; ++c) {
                s->dirty[r][c] = s->dirty[r + absn][c];
            }
        }
        for (r = bot - absn + 1; r <= bot; ++r) {
            blank_row(s, r);
            screen_mark_row(s, (u8)r);
        }
    } else {                           /* scroll down: content moves toward bot */
        for (r = bot; r >= top + absn; --r) {
            for (c = 0; c < (int)COLS; ++c) {
                s->cells[r][c] = s->cells[r - absn][c];
            }
            for (c = 0; c < (int)DIRTY_BYTES; ++c) {
                s->dirty[r][c] = s->dirty[r - absn][c];
            }
        }
        for (r = top; r <= top + absn - 1; ++r) {
            blank_row(s, r);
            screen_mark_row(s, (u8)r);
        }
    }
```

Delete the trailing `for (r = top; r <= bot; ++r) { screen_mark_row(...); }`.

- [ ] **Step 6: Fix the existing tests that read the old field**

`test/test_screen.c` reads `s.dirty[r]` in 8 places. Convert each to
`screen_row_dirty(&s, r)`. Do not weaken any assertion: where a test asserted a
row was dirty, it must still assert that.

- [ ] **Step 7: Run the tests to verify they pass**

```bash
make test
```

Expected: all pass, including the two new functions.

- [ ] **Step 8: Verify on the target**

```bash
make tap && make smoke
```

Expected: passes. Rendering output is unchanged; only which rows get repainted
changed, and at this point still whole rows.

- [ ] **Step 9: Commit**

```bash
git add -A
git commit -m "screen: track dirty state in four-cell groups

One boolean per row meant a one-character change repainted the whole row --
80 glyph lookups and 512 video stores. Four cells is 24 px is exactly three
scanline bytes in both geometries, so group marks translate into blits with
no arithmetic.

Marks now travel with content through a scroll instead of being reapplied to
the whole region, and blit_scroll_region no longer clears the region's marks:
clearing discarded changes that had never reached video RAM."
```

---

### Task 3: Block model scrolling

Copying every two-byte cell through nested loops with 16-bit index arithmetic
costs 449,787 T. A single overlapping `memmove` of the retained rows costs
67,609 T — 6.65× less.

**Files:**
- Modify: `src/screen.c`, `test/test_screen.c`

**Interfaces:**
- Consumes: the dirty-group API from Task 2.
- Produces: no new interface; `scroll_region` internals only.

- [ ] **Step 1: Write the failing test**

Add to `test/test_screen.c` and call from `main()`:

```c
static void test_scroll_moves_content_exactly(void)
{
    screen_t s;
    u8 r;

    /* Fill each row with a distinguishable character, then scroll and check
     * that content moved by exactly the right distance and freed rows blanked.
     * This must hold for a partial region too, which is where an off-by-one in
     * a block move would hide. */
    screen_init(&s);
    for (r = 0; r < ROWS; ++r) {
        screen_cup(&s, r, 0);
        screen_putc(&s, (u8)('A' + (r % 26u)));
    }

    screen_set_scroll_region(&s, 2, 8);
    screen_cup(&s, 8, 0);
    screen_scroll(&s, 1);

    CHECK(s.cells[2][0].ch == (u8)('A' + 3u));   /* row 3 moved to row 2 */
    CHECK(s.cells[7][0].ch == (u8)('A' + 8u));   /* row 8 moved to row 7 */
    CHECK(s.cells[8][0].ch == BLANK_CH);         /* freed row blanked */
    CHECK(s.cells[1][0].ch == (u8)('A' + 1u));   /* above the region untouched */
    CHECK(s.cells[9][0].ch == (u8)('A' + 9u));   /* below the region untouched */

    screen_init(&s);
    for (r = 0; r < ROWS; ++r) {
        screen_cup(&s, r, 0);
        screen_putc(&s, (u8)('A' + (r % 26u)));
    }
    screen_set_scroll_region(&s, 2, 8);
    screen_scroll(&s, -1);

    CHECK(s.cells[3][0].ch == (u8)('A' + 2u));   /* row 2 moved down to row 3 */
    CHECK(s.cells[8][0].ch == (u8)('A' + 7u));   /* row 7 moved down to row 8 */
    CHECK(s.cells[2][0].ch == BLANK_CH);         /* freed row blanked */

    /* A scroll larger than the region must clear it, not read out of bounds. */
    screen_init(&s);
    screen_set_scroll_region(&s, 2, 8);
    screen_scroll(&s, 100);
    CHECK(s.cells[2][0].ch == BLANK_CH);
    CHECK(s.cells[8][0].ch == BLANK_CH);
}
```

- [ ] **Step 2: Run the test to verify it fails or passes for the wrong reason**

```bash
make test
```

Expected: this passes against the current cell-by-cell implementation. That is
correct and intended — it is a **characterisation test** that pins the exact
behaviour before the implementation is replaced. Confirm it passes now, so that
a failure after Step 3 unambiguously means the block move is wrong.

- [ ] **Step 3: Replace the cell loops with block moves**

In `src/screen.c`, add `#include <string.h>` and rewrite the two content loops
in `scroll_region`. The rows are contiguous `cell_t[COLS]` arrays inside a 2-D
array, so a whole run of rows moves in one call:

```c
    if (n > 0) {                       /* scroll up: content moves toward top */
        u8 keep = (u8)(bot - absn - top + 1);

        if (keep != 0) {
            memmove(&s->cells[top][0], &s->cells[top + absn][0],
                    (size_t)keep * COLS * sizeof(cell_t));
            memmove(&s->dirty[top][0], &s->dirty[top + absn][0],
                    (size_t)keep * DIRTY_BYTES);
        }
        for (r = bot - absn + 1; r <= bot; ++r) {
            blank_row(s, r);
            screen_mark_row(s, (u8)r);
        }
    } else {                           /* scroll down: content moves toward bot */
        u8 keep = (u8)(bot - (top + absn) + 1);

        if (keep != 0) {
            memmove(&s->cells[top + absn][0], &s->cells[top][0],
                    (size_t)keep * COLS * sizeof(cell_t));
            memmove(&s->dirty[top + absn][0], &s->dirty[top][0],
                    (size_t)keep * DIRTY_BYTES);
        }
        for (r = top; r <= top + absn - 1; ++r) {
            blank_row(s, r);
            screen_mark_row(s, (u8)r);
        }
    }
```

`memmove` is required, not `memcpy`: the ranges overlap whenever `absn` is less
than the region height.

- [ ] **Step 4: Run the tests to verify they pass**

```bash
make test
```

Expected: all pass, including `test_scroll_moves_content_exactly` and
`test_scroll_migrates_dirty_groups` from Task 2.

- [ ] **Step 5: Verify on the target**

```bash
make tap && make smoke
```

- [ ] **Step 6: Commit**

```bash
git add -A
git commit -m "screen: scroll the model with memmove instead of cell loops

Copying every two-byte cell through nested loops with 16-bit index arithmetic
measured 449,787 T; one overlapping memmove of the retained rows measured
67,609 T. The dirty-group bytes move in the same shape, so marks stay attached
to their content.

memmove, not memcpy: the ranges overlap whenever the scroll distance is less
than the region height."
```

---

### Task 4: Exact cursor toggling

`main.c` dirties the cursor's old and new rows on every input batch, which forces
two full row repaints per keystroke. XOR-toggling the cursor cell is exact and
costs nothing.

**Files:**
- Modify: `src/main.c`, `include/blit.h`, `src/blit_hires.c`

**Interfaces:**
- Consumes: `blit_cursor` from Task 1, the group API from Task 2.
- Produces: `void blit_cursor_toggle(const screen_t *s);` replacing
  `blit_cursor`. It XORs the cursor cell's six pixels, so calling it twice
  restores the screen exactly.

- [ ] **Step 1: Read the current behaviour before changing it**

```bash
sed -n '80,115p' src/main.c
```

Note the two `scr->dirty[scr->cy] = 1;` sites at lines 89 and 110 (already
converted to `screen_mark_cell` by Task 2). The review warns that line 110
*accidentally* preserves a character during deferred wrap, and cannot simply be
deleted — it is safe to remove only because Task 2 Step 4 stopped
`blit_scroll_region` from clearing marks. Confirm that change is in place before
proceeding.

- [ ] **Step 2: Rename and document the toggle**

In `include/blit.h`, replace the `blit_cursor` declaration:

```c
/*
 * XOR the cursor cell's six pixels in place. Idempotent in pairs: call once to
 * show the cursor, once more to hide it, and the screen is bit-identical to
 * before. Does NOT mark anything dirty -- that is the point. When
 * MODE_CURSOR_VISIBLE is clear it does nothing, so pairs still balance.
 *
 * Hardware-only.
 */
void blit_cursor_toggle(const screen_t *s);
```

In `src/blit_hires.c`, rename `blit_cursor` to `blit_cursor_toggle`. Its body
already XORs, so no logic changes.

- [ ] **Step 3: Restructure the main loop**

In `src/main.c`, the loop must toggle the cursor **off** before the parser can
move it and **on** after rendering. Replace the render block inside `for (;;)`:

```c
        pump_keybuf_to_conn();
        conn_poll();
        blit_cursor_toggle(&scr);          /* hide: the parser may move it */
        if (pump_conn(&vt, &scr)) {
            sync_keyboard_modes(&scr);
            blit_flush(&scr);
        }
        blit_cursor_toggle(&scr);          /* show at its (possibly new) place */
```

and remove both `screen_mark_cell(&scr, scr.cy, scr.cx)` calls that Task 2 left
at the old lines 89 and 110.

**The toggle must bracket the parser call even when nothing arrives**, because
`blit_cursor_toggle` reads `s->cx`/`s->cy` at call time: hiding at the old
position and showing at the new one is exactly what the pairing achieves.

In the startup sequence before the loop, replace `render_cursor(&scr)` with
`blit_cursor_toggle(&scr)`.

- [ ] **Step 4: Verify on the target**

There is no host test for this — it is pure hardware behaviour. Verify in ZEsarUX:

```bash
make tap && make run-zrcp
```

Then, through the ZRCP bridge:

1. Type a character. It must appear, with the cursor one cell to its right and
   exactly one cursor block visible on screen.
2. Type enough characters to wrap a line, then keep typing. There must be **no
   leftover cursor block** anywhere on the previous line.
3. Fill the screen so it scrolls. After the scroll, exactly one cursor block is
   visible and no stale block remains at the pre-scroll position.

Test 3 is the one that fails if Task 2 Step 4 was skipped.

- [ ] **Step 5: Commit**

```bash
git add -A
git commit -m "main: toggle the cursor with XOR instead of dirtying its rows

Dirtying the old and new cursor rows on every input batch forced two full row
repaints per keystroke. The cursor cell is XORed off before the parser runs
and on after rendering, which is exact and marks nothing dirty.

Only safe now that blit_scroll_region has stopped clearing the region's dirty
marks: main.c's row dirtying was accidentally preserving a character during
deferred wrap."
```

---

### Task 5: The fast blit path

**Files:**
- Modify: `src/blit_hires.c`, `include/blit.h`
- Create: `docs/perf/benchmarks.md`
- Create: `tools/bench.sh`

**Interfaces:**
- Consumes: everything from Tasks 1–4.
- Produces: `blit_flush` renders only dirty groups, with an inline `attr == 0`
  group path and a blank-row block clear. No signature change.

- [ ] **Step 1: Record the baseline before optimising**

Create `tools/bench.sh`, which builds a harness and reports T-states so the
improvement is measured, not assumed:

```sh
#!/bin/sh
# Measure T-states for the renderer's hot paths with z88dk-ticks.
#
# CI prints these; it does not enforce them. A regression threshold would need a
# pinned compiler image, and .github/workflows/ci.yml uses z88dk/z88dk:latest.
set -e

ROOT="$(cd "$(dirname "$0")/.." && pwd)"
OUT="$ROOT/build/bench"
mkdir -p "$OUT"

echo "compiler: $(zcc 2>&1 | head -1)"
echo "see docs/perf/benchmarks.md for the recorded reference numbers"

# Each harness calls one path N times from a known state; z88dk-ticks reports
# the cycle count between the entry and exit breakpoints.
for path in row_normal row_attrs row_blank scroll_model scroll_vram; do
    printf '%-14s ' "$path"
    z88dk-ticks "$OUT/bench_$path.bin" -counter 2>/dev/null | tail -1
done
```

Run it and record the numbers in `docs/perf/benchmarks.md` under a "before"
heading, together with the exact z88dk version, since the numbers are only
comparable within one compiler.

- [ ] **Step 2: Add the group-to-bytes mapping**

In `src/blit_hires.c`, a four-cell group maps to three scanline bytes that
alternate between the two display files. From `render_row_bytes`, eight cells
occupy `ev[fi], od[fi], ev[fi+1], od[fi+1], ev[fi+2], od[fi+2]` where
`fi = 1 + 3j` for the eight-cell block `j`. So group `g` (four cells) lands on:

```c
/*
 * Destination bytes for four-cell group `g`, in left-to-right order.
 * Eight cells span six scanline bytes = three indices in each display file, so
 * an even group takes ev[fi], od[fi], ev[fi+1] and an odd group takes
 * od[fi+1], ev[fi+2], od[fi+2], where fi = 1 + 3 * (g / 2).
 */
static void blit_group_dst(u8 g, u8 *ev, u8 *od, u8 **dst)
{
    u8 fi = (u8)(1u + 3u * (g >> 1));

    if ((g & 1u) == 0u) {
        dst[0] = &ev[fi];
        dst[1] = &od[fi];
        dst[2] = &ev[fi + 1u];
    } else {
        dst[0] = &od[fi + 1u];
        dst[1] = &ev[fi + 2u];
        dst[2] = &od[fi + 2u];
    }
}
```

- [ ] **Step 3: Write the failing test for the mapping**

The mapping is pure arithmetic, so it is host-testable even though the blitter is
not. Move `blit_group_dst` into `src/render_hires.c` as
`render_group_bytes(u8 g, u8 *idx3, u8 *file3)` — returning indices and a file
selector rather than pointers, so it needs no display memory — declare it in
`render_geom.h`, and add to `test/test_render.c`:

```c
static void test_group_byte_mapping(void)
{
    u8 idx[3], file[3];
    u8 g, k;
    u8 seen_ev[32], seen_od[32];

    for (k = 0; k < 32u; ++k) { seen_ev[k] = 0; seen_od[k] = 0; }

    /* Every group's three bytes must be distinct, inside the margins, and no
     * byte may be claimed by two groups -- that would mean cells overwriting
     * each other's pixels. */
    for (g = 0; g < DIRTY_GROUPS; ++g) {
        render_group_bytes(g, idx, file);
        for (k = 0; k < 3u; ++k) {
            CHECK(idx[k] >= 1u && idx[k] <= 30u);
            if (file[k] == 0u) {
                CHECK(seen_ev[idx[k]] == 0u);
                seen_ev[idx[k]] = 1u;
            } else {
                CHECK(seen_od[idx[k]] == 0u);
                seen_od[idx[k]] = 1u;
            }
        }
    }
}
```

Run `make test`; expect `render_group_bytes` undeclared. Implement it, then
re-run and expect a pass.

- [ ] **Step 4: Render only dirty groups, with the two fast paths**

Rewrite `blit_flush` in `src/blit_hires.c`:

```c
void blit_flush(screen_t *s)
{
    u8 r;

    for (r = 0; r < ROWS; ++r) {
        if (!screen_row_dirty(s, r)) {
            continue;
        }
        if (blit_row_is_blank(s, r)) {
            blit_row_clear(s, r);      /* block clear, no font data touched */
        } else {
            blit_row_groups(s, r);     /* only the groups that changed */
        }
        screen_clear_marks(s, r);
    }
}
```

`blit_row_is_blank` returns true when every cell in the row is `BLANK_CH` with
`attr == 0`. `blit_row_clear` writes zero to the row's 8 scanlines in both files
across scanline bytes 1..30 without consulting the font.

`blit_row_groups` walks the dirty groups. For each, it reads the four cells'
attributes first: when all four are zero it uses an inlined packer with no
function calls and no temporary arrays; otherwise it falls back to the general
path through `render_glyph_row_byte` and `render_pack4`. The review measured this
distinction as the dominant cost — inlining the normal packer took an
80-column row from 670,861 T to 194,617 T.

- [ ] **Step 5: Verify correctness before speed**

```bash
make test && make tap && make smoke
```

Expected: pass. Then in ZEsarUX, confirm visually that reverse video and
underline still render — those take the fallback path, which is the one most
likely to be broken by this change.

- [ ] **Step 6: Measure and record**

```bash
sh tools/bench.sh
```

Record the "after" numbers in `docs/perf/benchmarks.md` beside the "before"
ones. The 80-column normal row should land near 194,617 T. **If it does not, stop
and investigate rather than proceeding** — Task 9 writes the ULA blitter in this
shape, and copying an unoptimised shape is exactly what this task exists to
prevent.

- [ ] **Step 7: Commit**

```bash
git add -A
git commit -m "blit: render only dirty groups, with normal and blank fast paths

A dirty row repainted all 80 cells regardless of what changed. It now renders
only the four-cell groups whose marks are set, takes an inlined packer path
when all four cells have no attributes, and block-clears a fully blank row
without touching font data.

Numbers recorded in docs/perf/benchmarks.md. CI prints them and does not
enforce them: a threshold would need a pinned compiler image."
```

---

### Task 6: The machine classifier

The port probes cannot run on the host, but the decision table can. Extracting it
is what makes detection testable at all.

**Files:**
- Create: `include/machine.h`, `src/machine_class.c`, `test/test_machine_class.c`
- Modify: `test/run.sh`, `Makefile`

**Interfaces:**
- Consumes: nothing.
- Produces:
  ```c
  #define MACHINE_ZX48 0u
  #define MACHINE_ZX128 1u
  #define MACHINE_TC2048 2u
  #define MACHINE_TS2068 3u
  u8 machine_classify(u8 scld, u8 ay_timex, u8 ay_zx);
  const char *machine_name(u8 machine);
  u8 machine_class_has_scld(u8 machine);
  ```

- [ ] **Step 1: Write the failing test**

Create `test/test_machine_class.c`:

```c
/*
 * Host tests for the machine decision table. The port probes themselves are
 * hardware and are verified in ZEsarUX; this covers the logic that turns three
 * probe results into a machine identity, including the combinations the
 * hardware cannot actually produce.
 */
#include "machine.h"
#include <assert.h>
#include <stdio.h>
#include <string.h>

static int checks;
#define CHECK(c) do { assert(c); ++checks; } while (0)

static void test_table(void)
{
    /* Timex family: the Timex-port AY is what separates 2068 from 2048. */
    CHECK(machine_classify(1, 1, 0) == MACHINE_TS2068);
    CHECK(machine_classify(1, 1, 1) == MACHINE_TS2068);
    CHECK(machine_classify(1, 0, 1) == MACHINE_TC2048);  /* AY expansion */
    CHECK(machine_classify(1, 0, 0) == MACHINE_TC2048);

    /* Sinclair family: ay_timex is never probed, so it must not matter. */
    CHECK(machine_classify(0, 0, 1) == MACHINE_ZX128);
    CHECK(machine_classify(0, 1, 1) == MACHINE_ZX128);
    CHECK(machine_classify(0, 0, 0) == MACHINE_ZX48);
    CHECK(machine_classify(0, 1, 0) == MACHINE_ZX48);
}

static void test_scld_class(void)
{
    CHECK(machine_class_has_scld(MACHINE_TC2048));
    CHECK(machine_class_has_scld(MACHINE_TS2068));
    CHECK(!machine_class_has_scld(MACHINE_ZX48));
    CHECK(!machine_class_has_scld(MACHINE_ZX128));
}

static void test_names(void)
{
    CHECK(strcmp(machine_name(MACHINE_ZX48), "ZX48") == 0);
    CHECK(strcmp(machine_name(MACHINE_ZX128), "ZX128") == 0);
    CHECK(strcmp(machine_name(MACHINE_TC2048), "TC2048") == 0);
    CHECK(strcmp(machine_name(MACHINE_TS2068), "TS2068") == 0);
    /* An out-of-range value must not index past the table. */
    CHECK(strcmp(machine_name(99), "?") == 0);
}

int main(void)
{
    test_table();
    test_scld_class();
    test_names();
    printf("test_machine_class: %d checks passed\n", checks);
    return 0;
}
```

- [ ] **Step 2: Run to verify it fails**

Add to `test/run.sh`:

```sh
$CC $CFLAGS $TERM_DEF "$ROOT/test/test_machine_class.c" "$ROOT/src/machine_class.c" \
    -o "$OUT/test_machine_class"
"$OUT/test_machine_class"
```

Run `make test`. Expected: `fatal error: 'machine.h' file not found`.

- [ ] **Step 3: Implement**

Create `include/machine.h` with the constants, the three prototypes, and a
comment recording that the table is ported from
`attribute-wars/docs/hardware/detect_machine_ay.asm`. Create
`src/machine_class.c`:

```c
/*
 * machine_class.c -- the machine decision table (pure, host-tested).
 *
 * Ported from the reference detector in attribute-wars,
 * docs/hardware/detect_machine_ay.asm. The probes live in machine.c; this file
 * holds only the logic that turns their results into an identity, so it can be
 * tested without hardware.
 *
 * Assumptions inherited from the reference detector: factory-standard machines;
 * TC2048 AY expansions use ZX128-compatible ports; a ZX48 with a Melodik-style
 * AY looks like a ZX128; a TC2048 modified with an AY at $F5/$F6 looks like a
 * 2068.
 */
#include "machine.h"

u8 machine_classify(u8 scld, u8 ay_timex, u8 ay_zx)
{
    if (scld) {
        return ay_timex ? MACHINE_TS2068 : MACHINE_TC2048;
    }
    return ay_zx ? MACHINE_ZX128 : MACHINE_ZX48;
}

u8 machine_class_has_scld(u8 machine)
{
    return (u8)(machine == MACHINE_TC2048 || machine == MACHINE_TS2068);
}

const char *machine_name(u8 machine)
{
    switch (machine) {
    case MACHINE_ZX48:   return "ZX48";
    case MACHINE_ZX128:  return "ZX128";
    case MACHINE_TC2048: return "TC2048";
    case MACHINE_TS2068: return "TS2068";
    default:             return "?";
    }
}
```

Add `src/machine_class.c` to `COMMON_SOURCES` in the Makefile.

- [ ] **Step 4: Run to verify it passes**

```bash
make test
```

Expected: `test_machine_class: 16 checks passed`.

- [ ] **Step 5: Commit**

```bash
git add -A
git commit -m "machine: add the host-testable machine decision table

Ported from the reference detector in attribute-wars. The port probes are
hardware and cannot be host-tested; the table that turns three probe results
into a machine identity can be, including the combinations the hardware never
produces."
```

---

### Task 7: The port probes, the guard and the banner

**Files:**
- Create: `src/machine.c`
- Modify: `include/machine.h`, `src/main.c`, `Makefile`

**Interfaces:**
- Consumes: `machine_classify`, `machine_name`, `machine_class_has_scld`.
- Produces: `u8 machine_detect(void);` (runs the probes once, caches, returns a
  `MACHINE_*` value) and `u8 machine_caps_shift_held(void);`.

- [ ] **Step 1: Write the probes**

Create `src/machine.c`. It is hardware, so it may include `z80.h` and is never
compiled on the host.

```c
/*
 * machine.c -- runtime machine identification (hardware; target only).
 *
 * Ported from attribute-wars, docs/hardware/detect_machine_ay.asm.
 *
 * MUST run before video initialisation and before the IM2 handler is installed,
 * with interrupts disabled: it writes port 0xFF, which on a Timex is the display
 * mode register.
 *
 * The SCLD probe toggles ONLY bits 3..5 -- the palette bits -- so screen mode,
 * interrupt control and EXROM/DOCK selection are preserved, and it restores the
 * original value on both exits. On a Spectrum port 0xFF is unattached: the write
 * is harmless and the read-back does not match.
 *
 * The AY probes leave register 11 SELECTED, because the currently selected
 * register cannot be read back. Any later AY user must select its own register
 * before writing.
 */
#include "machine.h"
#include <z80.h>

#define PORT_SCLD 0x00FFu
#define PORT_AY_TIMEX_SEL 0x00F5u
#define PORT_AY_TIMEX_DAT 0x00F6u
#define PORT_AY_ZX_SEL 0xFFFDu
#define PORT_AY_ZX_DAT 0xBFFDu
#define PORT_KEY_CAPS 0xFEFEu

static u8 detected;
static u8 have_detected;

static u8 probe_scld(void)
{
    u8 original = (u8)z80_inp(PORT_SCLD);
    u8 pattern;
    u8 i;
    static const u8 bits[3] = { 0x08u, 0x10u, 0x20u };

    for (i = 0; i < 3u; ++i) {
        pattern = (u8)(original ^ bits[i]);
        z80_outp(PORT_SCLD, pattern);
        if ((u8)z80_inp(PORT_SCLD) != pattern) {
            z80_outp(PORT_SCLD, original);   /* harmless on a Spectrum */
            return 0;
        }
    }
    z80_outp(PORT_SCLD, original);
    return 1;
}

static u8 probe_ay(u16 sel_port, u16 dat_port)
{
    static const u8 patterns[3] = { 0x55u, 0xAAu, 0x3Cu };
    u8 original;
    u8 i;

    z80_outp(sel_port, 11u);                 /* register 11 is fully writable */
    original = (u8)z80_inp(dat_port);

    for (i = 0; i < 3u; ++i) {
        z80_outp(dat_port, patterns[i]);
        if ((u8)z80_inp(dat_port) != patterns[i]) {
            z80_outp(dat_port, original);
            return 0;
        }
    }
    z80_outp(dat_port, original);
    return 1;
}

u8 machine_detect(void)
{
    u8 scld, ay_timex = 0, ay_zx = 0;

    if (have_detected) {
        return detected;
    }

    scld = probe_scld();
    if (scld) {
        ay_timex = probe_ay(PORT_AY_TIMEX_SEL, PORT_AY_TIMEX_DAT);
    }
    if (!scld || !ay_timex) {
        ay_zx = probe_ay(PORT_AY_ZX_SEL, PORT_AY_ZX_DAT);
    }

    detected = machine_classify(scld, ay_timex, ay_zx);
    have_detected = 1;
    return detected;
}

u8 machine_caps_shift_held(void)
{
    /* Keyboard half-row 0xFEFE, bit 0, active low. Read directly: keymap and
     * the IM2 handler are not running this early. */
    return (u8)(((u8)z80_inp(PORT_KEY_CAPS) & 0x01u) == 0u);
}
```

**Note the AY probe ordering:** the ZX-port probe also runs on a Timex without a
Timex-port AY, which is what identifies a TC2048 carrying a ZX-compatible AY
expansion. That matches the reference detector's `probe_ay_tc2048`, which simply
jumps to `probe_ay_zx`.

- [ ] **Step 2: Wire the guard into startup**

In `src/main.c`, before `video_init`:

```c
    {
        u8 machine = machine_detect();

#ifdef TERM_TIMEX
        if (!machine_class_has_scld(machine) && !machine_caps_shift_held()) {
            guard_refuse();     /* prints and halts; never returns */
        }
#endif
        banner_machine = machine;
    }
```

`guard_refuse()` writes a short message using the ULA text mode already active at
boot and halts with interrupts disabled. Implement it in `src/main.c` with the
ROM print routine rather than the terminal's own renderer, which is not
initialised yet:

```c
/* The terminal's renderer is not up yet and, on this machine, would not be
 * readable anyway. Use the boot-time ULA text mode via the ROM. */
static void guard_refuse(void) __naked
{
    __asm
        di
        ld      hl,#guard_msg
guard_loop:
        ld      a,(hl)
        or      a
        jr      z,guard_halt
        inc     hl
        push    hl
        rst     #0x10
        pop     hl
        jr      guard_loop
guard_halt:
        halt
        jr      guard_halt
guard_msg:
        .ascii  "THIS BUILD NEEDS A TIMEX (SCLD)."
        .db     13
        .ascii  "USE THE -ZX TAP, OR HOLD CAPS SHIFT."
        .db     13,0
    __endasm;
}
```

- [ ] **Step 3: Put the machine name in the banner**

The startup stream in `src/main.c` is a string literal. Feed the machine name
through the parser after it, so no literal has to be duplicated:

```c
    vt_feed_text(&vt, &scr, "\r\nHW: ");
    vt_feed_text(&vt, &scr, machine_name(banner_machine));
    vt_feed_text(&vt, &scr, machine_class_has_scld(banner_machine)
                 ? "  80x24 hi-res\r\n" : "  40x24 ULA\r\n");
```

Add the helper next to the existing feed loop:

```c
static void vt_feed_text(vtparse_t *vt, screen_t *scr, const char *p)
{
    while (*p != '\0') {
        vt_feed(vt, scr, (u8)*p);
        ++p;
    }
}
```

- [ ] **Step 4: Verify on the target**

```bash
make tap && make run-tc2048
```

Expected: the banner reads `HW: TC2048  80x24 hi-res`. Then:

```bash
make tap && make run TIMEX_MACHINE=TC2048
```

The guard case needs a non-Timex machine, which the current `run` target does not
offer; verify it in Task 11 once the ZEsarUX smoke covers all five combinations.

- [ ] **Step 5: Commit**

```bash
git add -A
git commit -m "machine: probe the ports, guard the wrong machine, name it in the banner

The SCLD probe toggles only the palette bits of port 0xFF and restores them,
so it is harmless on a Spectrum and non-destructive on a Timex. The AY probes
identify TS2068 against TC2048 and ZX128 against ZX48, and leave register 11
selected, as the reference detector documents.

The Timex build refuses to start without an SCLD rather than painting a
half-image; holding CAPS SHIFT overrides that for clones whose port decoding
fools the probe."
```

---

### Task 8: The ULA geometry and the two-pass host suite

**Files:**
- Create: `src/render_ula.c`, `src/ula.c`, `include/ula.h`
- Modify: `include/screen.h`, `test/run.sh`, `test/test_render.c`

**Interfaces:**
- Consumes: `render_geom.h` from Task 1.
- Produces: `render_ula.c` exporting the same `render_cell_span`,
  `render_row_bytes`, `render_group_bytes` as `render_hires.c`; and
  `u16 ula_addr(u8 col, u8 prow);` in `ula.h`.

- [ ] **Step 1: Make `screen.h` derive COLS from the machine define**

In `include/screen.h`, replace `#define COLS 80u`:

```c
/*
 * Exactly one machine define selects the geometry. The width is never passed
 * separately, so a mismatched pair -- a ZX build told it has 80 columns --
 * cannot be expressed.
 */
#if defined(TERM_TIMEX) && defined(TERM_ZX)
#error "define exactly one of TERM_TIMEX / TERM_ZX, not both"
#elif defined(TERM_TIMEX)
#define COLS 80u
#elif defined(TERM_ZX)
#define COLS 40u
#else
#error "define exactly one of TERM_TIMEX / TERM_ZX"
#endif
```

- [ ] **Step 2: Write the failing test**

In `test/test_render.c`, gate the geometry-specific assertions and add the ULA
set:

```c
#ifdef TERM_ZX
static void test_ula_geometry(void)
{
    u8 byte_idx, sh, mask0, mask1;
    u8 ev[32], od[32];
    u8 g[COLS];
    u8 c;

    /* Column 0 starts on a byte boundary 8 px in; the phase repeats every 4. */
    render_cell_span(0, &byte_idx, &sh, &mask0, &mask1);
    CHECK(byte_idx == 1u);
    CHECK(sh == 0u);
    CHECK(mask0 == 0xFCu);
    CHECK(mask1 == 0u);

    render_cell_span(1, &byte_idx, &sh, &mask0, &mask1);
    CHECK(byte_idx == 1u);
    CHECK(sh == 6u);
    CHECK(mask1 != 0u);            /* spills into byte 2 */

    render_cell_span(4, &byte_idx, &sh, &mask0, &mask1);
    CHECK(byte_idx == 4u);         /* four cells later, three bytes on */
    CHECK(sh == 0u);

    /* The last cell must stay inside byte 30, leaving byte 31 as margin. */
    render_cell_span((u8)(COLS - 1u), &byte_idx, &sh, &mask0, &mask1);
    CHECK(byte_idx <= 30u);
    CHECK(byte_idx + (mask1 ? 1u : 0u) <= 30u);

    /* A full row of solid cells must fill bytes 1..30 and leave 0 and 31 alone. */
    for (c = 0; c < COLS; ++c) { g[c] = 0xFCu; }
    for (c = 0; c < 32u; ++c) { ev[c] = 0xAAu; od[c] = 0xAAu; }
    render_row_bytes(g, ev, od);
    CHECK(ev[0] == 0xAAu);
    CHECK(ev[31] == 0xAAu);
    for (c = 1; c < 31u; ++c) {
        CHECK(ev[c] == 0xFFu);
    }
}
#endif
```

Run with `make test`; the ULA pass does not exist yet, so add it in Step 3 and
watch it fail on the missing `src/render_ula.c`.

- [ ] **Step 3: Make `test/run.sh` two-pass**

Restructure `test/run.sh` so the width-sensitive suites run twice:

```sh
# The pure core is width-parameterised at compile time, so it is compiled and
# run once per geometry. This is what keeps 40-column wrapping, tabs and erase
# behaviour under test without any runtime width plumbing.
for TERM_DEF in -DTERM_TIMEX -DTERM_ZX; do
    case "$TERM_DEF" in
        -DTERM_TIMEX) GEOM_SRC="$ROOT/src/render_hires.c"; SUFFIX=timex ;;
        -DTERM_ZX)    GEOM_SRC="$ROOT/src/render_ula.c";   SUFFIX=zx ;;
    esac
    echo "--- geometry: $TERM_DEF ---"

    $CC $CFLAGS $TERM_DEF "$ROOT/test/test_screen.c" "$ROOT/src/screen.c" \
        -o "$OUT/test_screen_$SUFFIX"
    "$OUT/test_screen_$SUFFIX"

    $CC $CFLAGS $TERM_DEF "$ROOT/test/test_vtparse.c" "$ROOT/src/vtparse.c" \
        "$ROOT/src/screen.c" -o "$OUT/test_vtparse_$SUFFIX"
    "$OUT/test_vtparse_$SUFFIX"

    $CC $CFLAGS $TERM_DEF "$ROOT/test/test_render.c" "$ROOT/src/render.c" \
        "$GEOM_SRC" "$ROOT/src/font.c" "$ROOT/src/hires.c" \
        -o "$OUT/test_render_$SUFFIX"
    "$OUT/test_render_$SUFFIX"
done
```

Keep the width-independent suites (`test_font`, `test_conn`, `test_keybuf`,
`test_keymap`, `test_hires`, `test_machine_class`) outside the loop, compiled
once with `-DTERM_TIMEX`.

**`test_screen.c` and `test_vtparse.c` must not hardcode 80.** Any assertion
mentioning column 79 becomes `COLS - 1`. Grep for literal `79` and `80` in both
files and convert every occurrence that means "the width".

- [ ] **Step 4: Implement the ULA geometry**

Create `src/render_ula.c`:

```c
/*
 * render_ula.c -- 40-column geometry on the plain ZX Spectrum display file.
 *
 * 40 cells x 6 px = 240 px = 30 bytes, centred in the 32-byte scanline with an
 * 8-px margin each side, so cells occupy bytes 1..30 and bytes 0 and 31 are
 * never written. Four cells is 24 px is exactly 3 CONSECUTIVE bytes here --
 * simpler than hi-res, where the same three bytes alternate between two files.
 *
 * The alternative to render_hires.c: same symbols, never linked together.
 * PURE logic: host-tested, must not include z80.h.
 */
#include "render_geom.h"
#include "render.h"
#include "screen.h"

#define RENDER_LEFT_MARGIN_PX 8u
#define RENDER_CELL_PX 6u

void render_cell_span(u8 col, u8 *byte_idx, u8 *sh, u8 *mask0, u8 *mask1)
{
    u16 px = (u16)(RENDER_LEFT_MARGIN_PX + RENDER_CELL_PX * (u16)col);
    u8 s = (u8)(px & 7u);

    *byte_idx = (u8)(px >> 3);
    *sh = s;
    *mask0 = (u8)(0xFCu >> s);
    *mask1 = (s > 2u) ? (u8)((0xFCu << (8u - s)) & 0xFFu) : 0u;
}

void render_group_bytes(u8 g, u8 *idx3, u8 *file3)
{
    u8 k;

    /* Three consecutive bytes, all in the single display file. */
    for (k = 0; k < 3u; ++k) {
        idx3[k] = (u8)(1u + 3u * g + k);
        file3[k] = 0u;
    }
}

void render_row_bytes(const u8 *g, u8 *ev, u8 *od)
{
    u8 j;

    (void)od;                      /* one display file: nothing to interleave */
    for (j = 0; j < DIRTY_GROUPS; ++j) {
        render_pack4(&g[j * 4u], &ev[1u + 3u * j]);
    }
}
```

Create `include/ula.h` and `src/ula.c` with `ula_addr`, mirroring `hires.c`:

```c
u16 ula_addr(u8 col, u8 prow)
{
    u16 off = ((u16)(prow & 0xC0u) << 5)      /* which third      */
            | ((u16)(prow & 0x07u) << 8)      /* scanline in cell */
            | ((u16)(prow & 0x38u) << 2);     /* char row in third */
    return (u16)(0x4000u + off + (u16)col);
}
```

- [ ] **Step 5: Run the tests to verify they pass**

```bash
make test
```

Expected: both geometry passes green, with `test_screen`, `test_vtparse` and
`test_render` each reported twice.

- [ ] **Step 6: Commit**

```bash
git add -A
git commit -m "render: add the 40-column ULA geometry and a two-pass host suite

40 cells of 6 px fill scanline bytes 1..30 of the single display file, leaving
bytes 0 and 31 as margin. render_pack4 is reused verbatim: four cells are three
consecutive bytes here, where hi-res alternates them between two files.

COLS now derives from TERM_TIMEX / TERM_ZX, and the width-sensitive host suites
compile and run once per geometry, so 40-column wrapping and tab behaviour are
tested without any runtime width plumbing."
```

---

### Task 9: The ULA blitter, video init and the build matrix

**Files:**
- Create: `src/blit_ula.c`, `src/video_ula.c`
- Modify: `Makefile`

**Interfaces:**
- Consumes: `render_ula.c`, `blit.h`, `video.h`, the group API.
- Produces: the `tap-zx` and `if1-zx` targets and their artifacts.

- [ ] **Step 1: Write the ULA video init**

Create `src/video_ula.c`:

```c
/*
 * video_ula.c -- plain ZX Spectrum display setup (hardware; ZX build only).
 *
 * Monochrome by necessity: a cell is 6 px, an attribute block is 8 px, so a
 * colour change mid-line would corrupt the neighbouring cell. The attribute
 * file is filled once and never touched again -- which also means scrolling
 * never has to move attributes.
 */
#include "video.h"
#include <z80.h>

#define ULA_BITMAP 0x4000u
#define ULA_ATTRS 0x5800u
#define ULA_BITMAP_LEN 6144u
#define ULA_ATTRS_LEN 768u
#define ULA_ATTR_WHITE_ON_BLACK 0x47u   /* BRIGHT white ink, black paper */

void video_init(u8 wob)
{
    (void)wob;                      /* the ZX build is always white on black */
    z80_outp(0xFE, 0x00);           /* black border */
    video_clear();
}

void video_clear(void)
{
    volatile u8 *bitmap = (volatile u8 *)ULA_BITMAP;
    volatile u8 *attrs = (volatile u8 *)ULA_ATTRS;
    u16 i;

    for (i = 0; i < ULA_BITMAP_LEN; ++i) {
        bitmap[i] = 0;
    }
    for (i = 0; i < ULA_ATTRS_LEN; ++i) {
        attrs[i] = ULA_ATTR_WHITE_ON_BLACK;
    }
}
```

- [ ] **Step 2: Write the ULA blitter**

Create `src/blit_ula.c` as the single-file counterpart of `blit_hires.c` from
Task 5 — **written directly in the fast shape**, never as a copy of the
pre-Task-5 version. It differs only in that there is one display file:
`blit_flush` walks rows and dirty groups identically; `blit_row_clear` zeroes
bytes 1..30 of eight scanlines in one file; `blit_scroll_region` calls the
existing scanline helpers on base `0x4000` alone; `blit_cursor_toggle` XORs
through `ula_addr` instead of `hires_addr`.

Reuse the scanline-offset helper by moving `row_scanline_offset` from
`blit_hires.c` into `src/ula.c` and `src/hires.c` respectively, or into a shared
pure module if both copies would be identical — they are, since the ZX thirds
interleave is the same inside each hi-res file as it is in the ULA file.

- [ ] **Step 3: Add the build matrix**

In the `Makefile`, add the ZX source list, the targets and the artifacts:

```make
ZX_SOURCES := $(COMMON_SOURCES) src/ula.c src/render_ula.c src/blit_ula.c \
	src/video_ula.c
ZX_TARGET_NAME ?= term-zx
ZX_IF1_TARGET_NAME ?= term-zx-if1

TIMEX_DEFS := -DTERM_TIMEX
ZX_DEFS := -DTERM_ZX
```

Add `tap-zx` and `if1-zx` targets mirroring `tap` and `if1`, each passing its own
source list and defines, each followed by the image-limit check. Add all four to
`.PHONY` and to `release-build`, and add `machine.c` to `COMMON_SOURCES` — it is
in both builds.

**`src/machine.c` is target-only**, so it must not appear in any host test link
line; it is in `COMMON_SOURCES` for the TAP builds only, which is already the
case since `test/run.sh` lists its sources explicitly.

- [ ] **Step 4: Build both TAPs**

```bash
make tap && make tap-zx && make if1 && make if1-zx
ls -l build/term.tap build/term-zx.tap build/term-if1.tap build/term-zx-if1.tap
```

Expected: four TAPs, each preceded by its image-limit line. The ZX image should
be **smaller** than the Timex one: half the cells means a smaller `screen_t` and
one display file's worth of blit code.

- [ ] **Step 5: First light on the target**

```bash
"$ZX" --machine 48k --tape build/term-zx.tap
```

Expected: the banner renders at 40 columns, reading `HW: ZX48  40x24 ULA`, in
white on black with black borders. Check specifically that the left and right
margins are clean — stray pixels in byte 0 or 31 mean the span arithmetic is
wrong.

- [ ] **Step 6: Commit**

```bash
git add -A
git commit -m "blit: add the ULA blitter, ZX video init and the two-TAP build matrix

blit_ula.c is written directly in the fast shape established for blit_hires.c:
dirty-group rendering, an inlined no-attribute path and a blank-row block
clear. Writing it as a copy of the old whole-row blitter would have produced
code that already had to be rewritten.

SOURCES splits into TIMEX_SOURCES and ZX_SOURCES; make tap-zx and make if1-zx
join make tap and make if1, each gated on the image limit."
```

---

### Task 10: Host integration

**Files:**
- Create: `terminfo/zx-vt102.terminfo`
- Modify: `tools/if1_pty_bridge.py`, `tools/zesarux_stdio_bridge.py`, `src/main.c`,
  `README.md`, `CLAUDE.md`, `Makefile`

- [ ] **Step 1: Add the terminfo entry**

Copy `terminfo/timex-vt102.terminfo` to `terminfo/zx-vt102.terminfo`, change the
entry name to `zx-vt102`, change `cols#80` to `cols#40`, and update the
description line to name the ZX Spectrum build. Leave every other capability
identical — the parser is the same.

Register it with the existing check target so a malformed entry fails CI:

```make
TERMINFO_SRCS := terminfo/timex-vt102.terminfo terminfo/zx-vt102.terminfo

terminfo-check: $(TERMINFO_SRCS)
	@for f in $(TERMINFO_SRCS); do tic -c -x "$$f" || exit 1; done
```

Update `install-terminfo` to install both.

- [ ] **Step 2: Verify the entry compiles and reports 40**

```bash
make terminfo-check
tic -x -o "$HOME/.terminfo" terminfo/zx-vt102.terminfo
TERM=zx-vt102 tput cols
```

Expected: `40`.

- [ ] **Step 3: Teach the bridges the width**

Both bridges size the PTY and export `TERM`. Add a `--cols` option defaulting to
80 and a `--term` default that follows it, so a 40-column run cannot silently be
told it is 80 — the failure mode is subtly wrong wrapping, which is hard to
diagnose. Where the scripts call `TIOCSWINSZ`, pass the chosen width.

- [ ] **Step 4: Draw the banner with a loop**

The 80-column box does not fit in 40. In `src/main.c`, replace the literal box
rules in `demo_stream` with a loop that emits `COLS - 2` horizontal glyphs
between the corners, in the DEC graphics charset already selected there. This
also removes roughly 400 bytes of string literal from the image.

Verify by eye at both widths that the box closes exactly at the right margin.

- [ ] **Step 5: Update the documentation**

In `README.md`: document both TAPs, which machine each is for, both terminfo
entries, and that the ZX build also runs on a TC2048 while the Timex build
refuses on a Spectrum. Replace the "designed but not yet implemented" note added
during the rename with the real instructions.

In `CLAUDE.md`: the Project Structure section names `hires.c` and `video.c` as
the hardware-isolation points. Update it to the post-split layout and note the
`TERM_TIMEX` / `TERM_ZX` rule and the two-pass host suite.

- [ ] **Step 6: Commit**

```bash
git add -A
git commit -m "terminfo: add zx-vt102 at 40 columns, and document both targets

A host told the terminal is 80 columns wide when it is 40 wraps every line
wrong, so the entry, the bridges and the README all carry the real width.

The banner box is drawn with a loop over COLS rather than stored as two string
literals, which also removes about 400 bytes from the image."
```

---

### Task 11: Full verification

**Files:**
- Modify: `test/zesarux_smoke.py`, `Makefile`, `docs/perf/benchmarks.md`

- [ ] **Step 1: Add the ULA cell reader**

`test/zesarux_smoke.py` reads cells through `cell_span`/`byte_addr`/`cell_bytes`,
which encode the hi-res two-file layout. Add the ULA variants — one file at
`0x4000`, scanline bytes 1..30, margin 8 px — and select between them with a
command-line flag defaulting to hi-res.

- [ ] **Step 2: Cover all five machine/TAP combinations**

Add a `smoke-zx` target and extend the script so `make smoke` and `make smoke-zx`
between them cover:

| TAP | ZEsarUX machine | Expected |
|---|---|---|
| `term.tap` | `TC2048` | 80 columns, banner `TC2048`, glyphs correct |
| `term-zx.tap` | `48k` | 40 columns, banner `ZX48`, glyphs correct |
| `term-zx.tap` | `TC2048` | 40 columns, banner `TC2048` — **the safe-default claim** |
| `term.tap` | `48k` | guard message on screen, program halted |
| `term.tap` | `48k`, CAPS SHIFT held | guard bypassed, program running |

The third row is the one that carries the argument that the ZX TAP is a safe
default; without it that claim is asserted but never verified. The fifth needs a
held key during load — ZEsarUX can be driven to hold CAPS SHIFT through ZRCP
before the tape loads.

- [ ] **Step 3: Record the final benchmarks**

```bash
sh tools/bench.sh
```

Add the 40-column row figure to `docs/perf/benchmarks.md`. **Compare it against
the spec's extrapolation of roughly half the 80-column cost.** If it differs by
more than about 20%, record the real number and note in the spec §7 that the
halving estimate was wrong in which direction — the spec explicitly commits to
confirming it.

- [ ] **Step 4: Wire the benchmark into CI as a printed step**

Add `bench` to the `ci` target's prerequisites only if z88dk is present in the CI
image; otherwise add it as a separate, non-blocking workflow step. It must print,
never gate (D25).

- [ ] **Step 5: Final gate**

```bash
make ci
make tap && make tap-zx && make if1 && make if1-zx
make smoke && make smoke-zx
```

Expected: everything green, four TAPs built, five smoke combinations passing.

- [ ] **Step 6: Commit**

```bash
git add -A
git commit -m "smoke: verify all five TAP and machine combinations

Adds the ULA cell reader and covers the combination that carries the whole
safe-default argument -- the ZX TAP on a TC2048 -- plus the guard refusing on
a Spectrum and CAPS SHIFT bypassing it.

Records the measured 40-column row cost against the design's extrapolation."
```

---

## Self-Review

**Spec coverage.** Milestone 3 is Task 1; milestone 4 is Tasks 2–5; milestone 5
is Tasks 6–7; milestone 6 is Task 8; milestone 7 is Task 9; milestone 8 is
Task 10; milestone 9 is Task 11. §7's in-scope items map to: item 1 → Task 2,
item 2 → Task 4, item 3 → Task 3, item 4 → Task 5, item 5 → Task 5, item 6 →
Task 5, item 7 → Task 2 Step 4. §10's host requirements are Tasks 2, 3, 6, 8;
its five target combinations are Task 11 Step 2; the negative build check is
Task 8 Step 1's `#error` pair, exercised implicitly by every compilation and
explicitly worth a manual `cc -DTERM_TIMEX -DTERM_ZX` attempt. §9's terminfo,
bridges and banner are Task 10.

**Placeholder scan.** Two steps describe work without pasting a full listing:
Task 9 Step 2 (`blit_ula.c`) and Task 10 Step 3 (the bridge `--cols` option).
Both are deliberate: the first is a transliteration of a file whose final shape
is produced in Task 5 and cannot be written before it exists, and the second is a
two-line change to scripts whose current argument handling must be read first.
Every other code step carries its code.

**Type consistency.** `render_cell_span(u8, u8*, u8*, u8*, u8*)`,
`render_row_bytes(const u8*, u8*, u8*)` and `render_group_bytes(u8, u8*, u8*)`
have identical signatures in `render_hires.c` and `render_ula.c` — required,
since they are alternatives behind `render_geom.h`. `blit_flush(screen_t *)`,
`blit_cursor_toggle(const screen_t *)` and `blit_scroll_region(screen_t *, u8,
u8, s8)` are declared once in `blit.h` and implemented twice. `video_init(u8)`
and `video_clear(void)` likewise. The dirty API introduced in Task 2 is used with
exactly those names in Tasks 3, 4, 5, 8 and 9. `machine_classify`,
`machine_name` and `machine_class_has_scld` are defined in Task 6 and consumed in
Task 7 with matching signatures.

**One gap worth naming.** Task 5 Step 1 asks for baseline numbers from a harness
that Task 5 itself creates, so the "before" measurement happens after four tasks
have already changed the code. The numbers in `perf-review.md` are the true
pre-work baseline; `docs/perf/benchmarks.md` should record both, labelled by the
commit they were taken at, or the improvement attributed to Task 5 will silently
include Tasks 2–4.
