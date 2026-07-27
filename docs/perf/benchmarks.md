# Renderer benchmarks

T-state measurements for the hot renderer/scroll paths, produced by
`tools/bench.sh` (harness sources in `tools/bench/`) using `z88dk-ticks`.
CI prints these; it does not enforce them -- a regression threshold would
need a pinned compiler image, and `.github/workflows/ci.yml` uses
`z88dk/z88dk:latest`.

Numbers are **only comparable within one z88dk build**. The compiler version
is recorded with every row below.

## How these are measured

Each harness (`tools/bench/bench_<path>.c`) links the real project sources
(`screen.c`, `render.c`, `render_hires.c`, `blit_hires.c`, `hires.c`,
`font.c`) and drives ONE public entry point from a known state, bracketed by
`bench_mark_a()` / `bench_mark_b()` (`tools/bench/bench_common.c`, a
separate translation unit so the calls cannot be inlined away).
`z88dk-ticks` resets its cycle counter the instant PC reaches
`bench_mark_a`'s entry and stops it the instant PC reaches `bench_mark_b`'s
entry, so the printed number is the T-states strictly between those two
calls, plus a small fixed overhead (`bench_mark_a`'s own body plus the
`CALL` that enters `bench_mark_b`, on the order of 40 T) -- negligible
against the paths measured here.

**z88dk-ticks argument order matters.** `z88dk-ticks` parses arguments left
to right and loads `<input_file>` the instant it reaches the (flag-less)
filename token, using whatever `load_address` (`-l`) it has parsed *so
far*. Putting the filename before `-l` loads the file at address 0 while
`-pc`/`-l` still tells the CPU to start executing at `$8000` -- a silent
walk through unrelated/zero memory that still happens to cross the
`-start`/`-end` label addresses in the right order, producing a
plausible-looking but completely meaningless number (confirmed by tracing:
it exactly matches a NOP-count between the two addresses). `tools/bench.sh`
puts every flag, including `-l`, before the filename. This cost real time to
find -- an earlier draft of this script had it backwards and reported
numbers around 24-131,072 T for every path, suspiciously round and
identical across totally different code.

Rows:

- `row_normal` -- `blit_flush()` with one dirty row, 80 cells, `attr == 0`.
- `row_attrs` -- same, but every cell carries `ATTR_REVERSE` (forces the
  slow fallback for all 20 groups).
- `row_blank` -- one dirty row, fully blank (space, `attr == 0`).
- `scroll_model` -- `screen_scroll()` over the full 24-row region, filled.
- `scroll_vram` -- `blit_scroll_region()` over the full 24-row region.

## Reference: the performance review's own numbers (true pre-work baseline)

**Commit:** `7323852` ("ZX Spectrum 40-column target: design, plans, and
project rename" -- the commit that added
`docs/superpowers/reviews/2026-07-26-perf-review.md`, before any of Tasks
1-9 existed; the 80-column renderer was still only a plan at this point).
**Compiler:** `zcc +zx -SO3 -clib=sdcc_iy` (exact z88dk build/version not
recorded in the review).

| Path | Measured | Diagnostic variant (inlined) |
|---|---:|---:|
| One normal dirty row, 64 columns (old renderer) | 176,039 T | 76,055 T |
| Screen-model scroll | 449,787 T | 67,609 T |
| Video-RAM scroll | 682,052 T | 345,464 T |
| Planned 80-column row (not yet implemented) | 670,861 T | 194,617 T |

This is the number this task's brief points at (194,617 T) as the "near"
target for a normal 80-column row. It comes from a diagnostic prototype the
review built, not from the codebase as it exists after Tasks 1-4 -- see the
"before" row below for that.

## Before (this task's own baseline, at the commit prior to its change)

**Commit:** `da2ac39` (end of Task 4: "main: toggle the cursor with XOR
instead of dirtying its rows" -- the last commit before this task's
`blit_flush` rewrite; Tasks 1-4's dirty-group tracking, memmove scroll, and
exact-cursor-toggle are already in place, but `blit_flush` still repaints
whole rows unconditionally).
**Compiler:** `zcc - Frontend for the z88dk Cross-C Compiler -
v23854-4d530b6eb7-20251002`.

| Path | T-states |
|---|---:|
| row_normal | 600,728 |
| row_attrs | 609,688 |
| row_blank | 600,728 |
| scroll_model | 101,533 |
| scroll_vram | 680,099 |

`row_blank` equals `row_normal` exactly: the pre-Task-5 `blit_flush` has no
blank-row fast path, so a fully blank row costs the same as a fully
populated one -- both go through the same whole-row `render_row_fast()`
regardless of content. This is expected, and it is exactly what Task 5
removes.

## After (this task: dirty-group rendering, inline `attr==0` path, blank-row fast path)

**Commit:** see the commit this file is checked in with (`blit: render only
dirty groups, with normal and blank fast paths`).
**Compiler:** `zcc - Frontend for the z88dk Cross-C Compiler -
v23854-4d530b6eb7-20251002` (same build as "before", for a fair comparison).

| Path | T-states | vs. before | vs. review's diagnostic target |
|---|---:|---:|---:|
| row_normal | 267,267 | 2.25x faster | 1.37x slower than 194,617 |
| row_attrs | 549,093 | 1.11x faster | -- |
| row_blank | 40,198 | 14.9x faster | -- |
| scroll_model | 101,508 | unchanged (not touched by this task) | -- |
| scroll_vram | 680,099 | unchanged (not touched by this task) | -- |

`scroll_model` and `scroll_vram` are included for completeness (this
harness benchmarks the same code Tasks 3/4 already optimised) and, as
expected, come back essentially identical to "before" -- neither path is
touched by this task, which is a useful sanity check that nothing regressed
elsewhere.

### The `row_normal` gap: investigated, not silently accepted

**`row_normal` did not land near the review's 194,617 T target.** Per the
brief's explicit instruction ("If it does not, stop and investigate rather
than proceeding"), this was investigated rather than reported as a plain
success:

1. **First implementation measured 318,554 T** (worse than the eventual
   number below). Investigation found a real bug: `row_scanline_offset()`
   and `font_glyph()` were being recomputed *inside the per-group loop*,
   once per scanline -- 160 calls each for a 20-group row instead of 8 (for
   the scanline-offset table, shared by every group) and 80 (for the glyph
   lookups, 4 per group). Hoisting both to run once per row (offsets) or
   once per group (glyphs) dropped the number to 276,448 T.
2. Two further C-level restructurings were tried and measured:
   scalar locals instead of small arrays for the group's destination
   indices (276,448 -> 278,168 T, no real change), and walking pointers
   instead of array indexing for the per-scanline glyph/offset reads
   (278,168 -> 286,382 T, slightly worse -- reverted in spirit, kept for
   the offsets since it was harmless there).
3. Isolated microbenchmarks of the two *new* per-group helpers this task
   introduces (`tools/bench` ad hoc, not part of the committed harness)
   found real, substantial SDCC-ABI call overhead: `screen_group_dirty()`
   ~550 T/call, `render_group_bytes()` ~975 T/call, `font_glyph()` ~370
   T/call -- all far higher than a hand-written Z80 equivalent, an
   inherent characteristic of this compiler's stack-marshalled calling
   convention for small functions, not a logic bug. Inlining
   `screen_group_dirty`'s bit test and `render_group_bytes`'s index
   arithmetic directly into `blit_row_groups` (documented as a deliberate,
   tested-formula duplication -- both remain defined and host-tested
   elsewhere) recovered another ~19,000 T, landing at the reported
   267,267 T.

**Conclusion:** the remaining ~73,000 T gap to 194,617 T is attributed to
SDCC's (`zsdcc`, selected by `-clib=sdcc_iy`) code generation quality for
this kind of pointer/bitfield-heavy C on Z80 -- confirmed by direct
disassembly of the generated `blit_row_groups` code, which repeatedly
reloads values through `IX`-relative stack addressing rather than keeping
them in registers across the 8-scanline inner loop, and by the measured
per-call costs above. Further C-source restructuring did not close it in
three attempts. The review itself names the fallback for exactly this
situation: "An assembly normal path if the C implementation still misses
the target" (`docs/superpowers/reviews/2026-07-26-perf-review.md`,
recommended-order item 7). This task does not implement that assembly path
-- it is out of scope for a C-level task and is flagged for whoever picks
up performance work next.

The **shape** of the change (group loop, attr==0 inline path with no helper
calls or temporary arrays inside the scanline loop, blank-row block clear)
matches the brief exactly, and it is a genuine, large improvement (2.25x
over the immediately-preceding commit, and would be roughly 2.5x over the
true pre-work baseline once the 64-vs-80-column difference is accounted
for) -- it just does not reach the specific number the review's own
diagnostic prototype hit.

## Function-call vs. inlined mapping/dirty-check (post-review follow-up)

Code review raised an Important finding: `blit_row_groups` hand-copies
`render_group_bytes()`'s index arithmetic and `screen_group_dirty()`'s bit
test, and `blit_hires.c` cannot be host-compiled (absolute `HIRES_FILE0`/
`HIRES_FILE1` addresses), so `test/run.sh` can never catch that copy
diverging from the originals -- a real risk, and one Task 9's `blit_ula.c`
would otherwise inherit and multiply.

Per the reviewer's request, tried calling both shared functions at **group
granularity** (once per group -- 20 calls/row each -- not once per
scanline, which would be the 8x trap `blit_row_groups`' header comment
already warns about) instead of duplicating their formulas inline, and
re-measured with the same harness used throughout this file:

```sh
export PATH="$HOME/Programowanie/z88dk/bin:$PATH"
export ZCCCFG="$HOME/Programowanie/z88dk/lib/config"
sh tools/bench.sh
```

| Variant | row_normal T-states |
|---|---:|
| Inlined/duplicated formulas (shipped) | 267,267 |
| Real calls to `screen_group_dirty()` + `render_group_bytes()` (group granularity) | 286,382 |

Difference: **+19,115 T, ~7.2%**. This is a deterministic, reproducible
cost -- `z88dk-ticks` is a cycle-exact emulator with no run-to-run
variance, so this is not measurement noise, it is the real, repeatable
price of two extra `CALL`/`RET` pairs and stack-marshalled arguments per
group under this SDCC ABI (consistent with the ~550 T and ~975 T per-call
costs measured earlier in this document).

**Decision: kept the inlined/duplicated formulas (267,267 T).** ~7% is a
real, systematic cost, not noise, on a row that already misses its
194,617 T reference target -- adding it back would widen an
already-disclosed shortfall rather than close it, for a maintainability
benefit (single source of truth reachable by host tests) that can instead
be obtained by making the duplication impossible to miss:

- `include/render_geom.h` and `src/render_hires.c` (`render_group_bytes`'s
  declaration and definition) and `include/screen.h` and `src/screen.c`
  (`screen_group_dirty`'s declaration and definition) each now carry a
  `CONSTRAINT` comment pointing at the copy in `blit_hires.c` and warning
  that `test/run.sh` cannot catch a divergence.
- `blit_hires.c`'s copy is marked `DUPLICATED FORMULAS -- KNOWN, MEASURED,
  DELIBERATE`, cites this section for the exact numbers, and states plainly
  that Task 9's `blit_ula.c` should decide fresh whether to call the shared
  functions or duplicate them -- it must not assume this file's shape
  calls them, because it doesn't.

If a future editor changes either formula, both `CONSTRAINT` comments and
the `blit_hires.c` copy will be visible from the definition site, which is
the best available safety net given `blit_hires.c` is structurally
unreachable from the host test suite.

## Task 9: moving `row_scanline_offset` out of `blit_hires.c`

Task 9 moved the row-scanline "thirds" offset formula (previously a
`blit_hires.c`-local `static` function) into `src/hires.c`
(`hires_row_scanline_offset()`) and `src/ula.c` (`ula_row_scanline_offset()`)
as pure, host-tested functions (`test/test_hires.c`, `test/test_ula.c`), so
`src/blit_ula.c` would have one readable, tested source for the identical ZX
"thirds" interleave instead of a third hand-copy. `blit_hires.c`'s
`blit_row_groups()`/`blit_row_clear()` were updated to call
`hires_row_scanline_offset()` instead of keeping their own copy.

This is a call, not a duplication, so it was re-measured with
`tools/bench.sh` (same harness, same compiler, `zcc ... v23854-4d530b6eb7-...`)
to check the "call vs. duplicate" question was not silently decided by
inertia:

| Path | Before (local `static`) | After (calls `hires_row_scanline_offset()`) | Delta |
|---|---:|---:|---:|
| row_normal | 267,267 | 268,283 | +1,016 T (+0.38%) |
| row_attrs | 549,093 | 550,109 | +1,016 T (+0.19%) |
| row_blank | 40,198 | 41,214 | +1,016 T (+2.5%) |
| scroll_model | 101,508 | 101,533 | +25 T (noise; `scroll_model` calls only `screen_scroll()`, which never touches this formula) |
| **scroll_vram** | **680,099** | **775,603** | **+95,504 T (+14.0%)** |

`blit_row_groups()`/`blit_row_clear()` call the shared function at most 8
times per row (the offsets are precomputed once, before the group loop --
see this file's earlier sections), so the row paths' ~1,016 T cost is the
same small, disclosed, per-call price as everywhere else in this document
and was accepted.

`scroll_vram` (`blit_scroll_region()` over the full 24-row region) is a
different story: `scroll_file_up_one()`/`scroll_file_down_one()` call the
offset formula up to 2x per `(row, scanline)` pair, x2 display files, over a
23-row region -- roughly 750 calls for one `blit_scroll_region()` call. A
plain move of the call site, taken at face value, would have silently
regressed a path Tasks 3/4 already spent effort optimising (680,099 T, see
"Before"/"After" tables above) by 14% -- an unrelated build-matrix task
quietly undoing prior optimisation work.

**Decision: reverted the scroll primitives to a local, duplicated formula**
(`scroll_scanline_offset()`, marked `DUPLICATED FORMULA -- KNOWN, MEASURED,
DELIBERATE` in `src/blit_hires.c`, cross-referencing
`hires_row_scanline_offset()`), recovering `scroll_vram` to exactly 680,099 T
(confirmed by rerunning `tools/bench.sh` after the revert -- see the numbers
in the "current" run below). The row-groups call sites keep calling the
shared, host-tested function; only the scroll loop -- the one call site
where the per-call cost multiplies into a double-digit percentage -- keeps
its own copy.

Current (`tools/bench.sh`, same compiler build, after the scroll-path revert):

| Path | T-states |
|---|---:|
| row_normal | 268,283 |
| row_attrs | 550,109 |
| row_blank | 41,214 |
| scroll_model | 101,533 |
| scroll_vram | 680,099 |

## ULA blitter: duplicate vs. call (Task 9)

`src/blit_ula.c`'s `blit_row_groups()` needed the same decision
`blit_hires.c` made in the section above, but the brief explicitly asked for
it to be decided fresh rather than assumed: the ULA mapping is materially
simpler (three consecutive bytes in one display file, no even/odd branch),
so calling `screen_group_dirty()` + `render_group_bytes()` (src/render_ula.c)
at group granularity might plausibly have been cheap enough to keep as the
single source of truth.

Measured with an ad hoc `z88dk-ticks` harness mirroring
`tools/bench/bench_row_normal.c` (one dirty row, 40 cells, `attr == 0`,
`-DTERM_ZX`), comparing two full implementations of `blit_ula.c`: one with
the group-index arithmetic (`idx0 = 1 + 3*g`) and the dirty-bit test
hand-duplicated inline (mirroring `blit_hires.c`'s shape), one calling
`screen_group_dirty()` + `render_group_bytes()` once per group (10
groups/row for the 40-column build):

| Variant | row_normal T-states (40-column build) |
|---|---:|
| Inlined/duplicated formulas (shipped) | 130,730 |
| Real calls to `screen_group_dirty()` + `render_group_bytes()` (group granularity) | 143,181 |

Difference: **+12,451 T, ~9.5%**. Same deterministic-cost conclusion as the
Timex measurement above (there +19,115 T/~7.2% over 20 groups; here +12,451
T/~9.5% over 10 groups -- consistent with the same per-call SDCC-ABI
overhead applied to half as many groups, landing a comparable percentage
because the base row cost is also roughly halved at 40 columns).

**Decision: kept the inlined/duplicated formulas (130,730 T)**, for the same
reasoning as `blit_hires.c`: ~9.5% is a real, systematic, reproducible cost
(this emulator has zero run-to-run variance), not something to trade away for
a maintainability benefit that is instead obtained by making the duplication
impossible to miss -- `src/render_ula.c` (`render_group_bytes`) and
`include/render_geom.h` now carry `CONSTRAINT` comments pointing at
`src/blit_ula.c`'s copy (mirroring the existing `blit_hires.c` ones), and
`include/screen.h`/`src/screen.c` (`screen_group_dirty`) now name both
`blit_hires.c` and `blit_ula.c` as hand-copies. Grep for "DUPLICATED
FORMULAS" in `src/blit_ula.c`.

The measurement harness (two full `blit_ula.c` variants plus a
`bench_row_normal_zx.c` mirroring `tools/bench/bench_row_normal.c`) was ad
hoc, not committed to `tools/bench/` -- the same precedent as the
"Function-call vs. inlined mapping/dirty-check" section above, which used an
uncommitted `tools/bench` harness for its own isolated per-call
microbenchmarks.
