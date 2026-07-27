# ZX Spectrum Target: 40 Columns On The ULA, From One Source Tree — Design

**Date:** 2026-07-26
**Extends:** `2026-07-26-80-column-rom-font-design.md`. The 6×8 TT3000 font, the
packing scheme, and the cell/attribute model carry over unchanged. This spec adds
a second build target and supersedes D16 for the paths listed in §7.
**Baseline:** branch `feat/zx-spectrum-target`, cut from `feat/80-columns`
(commit `951b728`). The fix wave that PR #2 missed has since landed on `master`
via PR #3, so this baseline is fully contained in `master` and the branch adds
only this document.
**Status:** Approved design, pending implementation plan.

---

## 1. Purpose

Run the same terminal on a stock **ZX Spectrum 48K/128K**, using ULA graphics,
with the same 6-px TT3000 font.

The Timex hi-res mode composes a 512×192 image from two display files. A stock
Spectrum has one 256×192 file — exactly half the horizontal pixels. At the same
6-px cell pitch that is 42 columns; this spec settles on **40×24** (D18).

The user's initial framing was one universal TAP that detects its host at
startup. That was designed, costed, and **rejected in favour of two TAPs built
from one source tree** (D17). The reasoning is in §3.

---

## 2. Key decisions

Continuing the D-numbering.

| # | Decision | Rationale |
|---|----------|-----------|
| D17 | **Two TAPs from one source tree, selected in the Makefile. Not one universal binary.** | A universal binary forces the display width to be runtime state, which means rewriting the ~25 uses of `COLS` in `screen.c`/`vtparse.c` into `s->cols` and carrying both blitters in every image. Two targets keep `COLS` a compile-time constant, leave the pure core untouched, and let the linker drop the geometry that is not built. Cost: two release artifacts instead of one (§3, and the risk table in §11). |
| D18 | **The Spectrum build is 40×24, not the 42 that would fit.** | 4 cells × 6 px = 24 px = exactly 3 bytes, so 40 cells are 10 groups filling scanline bytes 1..30, with a byte-aligned 8-px margin each side. 42 cells would leave a 2-cell tail straddling a half-byte, needing a separate path in packing, scrolling, and cursor drawing. Two extra columns is not worth a second code path in three places. |
| D19 | **`COLS` stays a compile-time constant, derived from a single machine define.** | Direct consequence of D17. The build passes exactly one of `-DTERM_TIMEX` / `-DTERM_ZX`; `screen.h` derives `COLS` from it. Width is never passed independently, so a mismatched pair — a ZX build told it has 80 columns — cannot be expressed. **The second width costs `screen.c` and `vtparse.c` no change at all**; tab-stop bitmap sizing in `vtparse.h` already derives from `COLS`. (Both files *are* modified in this slice, but by the performance items in §7, not by the target split: `screen.c` for scrolling and dirty tracking, `vtparse.c` for the two places it sets `s->dirty` directly.) |
| D20 | **The Spectrum build is monochrome with one global colour.** | A cell is 6 px; an attribute block is 8 px. Per-cell colour is physically impossible — a colour change mid-line would corrupt the neighbouring cell. The attribute file is filled once with `0x47` (bright white on black), border black. This matches Timex hi-res, which is also monochrome, so `REVERSE` and `UNDERLINE` behave identically on both machines. |
| D21 | **Machine detection is kept, but as a wrong-machine guard and a banner line — not a mode selector.** | With D17 there is no mode to select. The Timex build on a Spectrum would render garbage (half its pixels go to `0x6000`, which the ULA does not display), so that build refuses to start on a non-SCLD machine. The reverse needs no guard: a Timex boots in Spectrum-compatible ULA mode, so the Spectrum TAP runs correctly on it at 40 columns. |
| D22 | **Holding CAPS SHIFT at boot bypasses the guard. Timex build only.** | Insurance for clones and interfaces that decode port `$FF` incompletely and could make a genuine Timex fail the probe. Costs a few dozen bytes and cannot be debugged any other way on real hardware. The Spectrum build has no guard, so the key does nothing there. Accepted consequence: holding CAPS SHIFT with the Timex TAP on a real Spectrum produces exactly the unreadable half-image D21 exists to prevent — that is the point of an override, and it is reachable only by deliberate action. |
| D23 | **The IM2 vector table moves off the hardcoded `0xD300`/`0xD4D4` it once used and gains a build-time image-limit gate.** | `main.c` used to write the table at a hardcoded `0xD300`, trampoline at `0xD4D4`, right above an image that once ended at `0xCD58`; the linker knew nothing about either address, so a build that grew past the table linked cleanly and then overwrote its own code during IM2 setup. The table moved through an interim `0xF900`, then to `0xE000` (`I = 0xE0`) after a fix-wave measurement found real stack/call depth reaching down to `0xEF3D` — deeper than `0xF900` and a considered `0xF000` fallback. That measurement's suspected "deep call chain" turned out to be one oversized stack frame in `main()` (`screen_t scr` and `vtparse_t vt`, together nearly the whole 4,123 bytes); hoisting both to file scope fixed the root cause and moved the table again, to `0xEE00` (`I = 0xEE`), this time because the image had grown, not because the stack still reached that deep. A single source of truth in `include/im2.h`, and `tools/check_image_limit.py`, fails the build if the image, the table, or the stack collide. See §8. |
| D24 | **The performance findings that touch the code this slice rewrites are done here, not deferred.** | Supersedes D16 for these paths only. Writing the ULA blitter as a copy of the current hi-res path would produce code that the review already shows must be rewritten. See §7 for what is in and what is explicitly not. |
| D25 | **T-state benchmarks are recorded, not enforced in CI.** | A regression threshold needs a pinned compiler; `ci.yml` uses `z88dk/z88dk:latest`. Numbers are printed and committed as reference points. |
| D26 | **A separate terminfo entry, `zx-vt102`, with `cols#40`.** | The host must be told the real width or every wrapped line will be wrong. `timex-vt102` keeps `cols#80`. |

---

## 3. Why not one universal TAP

Both options were designed to the point of costing. Recording the comparison so
it is not re-litigated.

A universal binary needs the width to be runtime state. That means:

- `screen_t` gains a `cols` field and all ~25 `COLS` uses in `screen.c` and
  `vtparse.c` become `s->cols`;
- `cells[ROWS][80]` stays at maximum size on the Spectrum, wasting 1920 bytes;
- both blitters are linked into every image;
- the dirty-group metadata (§7) has to be sized for the maximum width and
  bounded at runtime.

The memory pressure that first raised the question turned out to be a red
herring: what looked like the program's headroom was actually the gap before a
hardcoded IM2 table — `0xD300 - 0xCD58 = 1,448` bytes on the older 64-column
`master` build, down to about 1,000 bytes on this branch's 80-column image —
with `0xFD58 - 0xD4D4 = 0x2884 = 10,372` bytes (~10.1 KB; an earlier note here
said "roughly 11 KB", which the arithmetic does not support) sitting unused
above it (§8, which relocates that table to reclaim the gap rather than
leaving it as a hazard). Two TAPs are therefore chosen on **simplicity**, not
on memory — they delete a refactor of the pure core that would otherwise be
the largest single piece of this work.

The cost of D17 is two artifacts and a user who must pick the right one. That is
softened by D21: the Spectrum TAP runs on **ZX Spectrums and the TC2048**, so it
is the safe default for anyone unsure between those. It is not a universal
fallback — a TS2068 cannot load either TAP, for the tape-format reason in §11.

---

## 4. ULA screen layout

One display file at `0x4000`–`0x57FF`, attributes at `0x5800`–`0x5AFF`.

```
byte:  0        1  ..............................  30       31
      [margin ][      40 cells x 6 px = 240 px      ][margin ]
       8 px                                           8 px
```

Cells occupy scanline bytes 1..30. Bytes 0 and 31 are never written, so the
margins stay black without being cleared per row.

Cell geometry, mirroring `render_cell_span()` for hi-res with a different
margin and a 32-byte scanline:

```
px       = 8 + 6 * col          /* 8 px left margin       */
byte_idx = px >> 3              /* 1 .. 30                */
sh       = px & 7               /* bit phase, period 4    */
mask0    = 0xFC >> sh
mask1    = (sh > 2) ? (0xFC << (8 - sh)) & 0xFF : 0
```

The bit phase repeats every four cells, exactly as in hi-res, because 24 px is a
whole number of bytes. `render_pack4()` is therefore reused **verbatim**: one
group of four cells produces three bytes written to `row[1 + 3j]` through
`row[3 + 3j]` for `j = 0..9`.

Scanline addressing needs no new mathematics. The ZX "thirds" interleave that
`row_scanline_offset()` already implements is the interleave used *inside each*
hi-res display file, so the same function applied to base `0x4000` is the ULA
address. Scrolling is likewise the existing helper run over one file instead of
two.

**Attribute area.** Filled once at startup with `0x47`. Nothing else writes it,
so scrolling never has to move attributes.

---

## 5. Build matrix and file layout

| Target | Machine | Width | Video | Defines |
|---|---|---|---|---|
| `make tap` | Timex TC2048 (TS2068 — see §11) | 80×24 | SCLD hi-res, two files | `-DTERM_TIMEX` |
| `make tap-zx` | ZX Spectrum 48K/128K | 40×24 | ULA, one file | `-DTERM_ZX` |
| `make if1` | as `tap` | 80×24 | | `+ -DCONN_BACKEND_IF1` |
| `make if1-zx` | as `tap-zx` | 40×24 | | `+ -DCONN_BACKEND_IF1` |

Per D19 the width is not a separate define. `screen.h` derives `COLS` from the
machine define and fails to compile if neither or both are set.

Artifacts, stated explicitly so the target-to-filename mapping is not guessed
(the target suffixes the machine last, the artifact places it first):

| Target | Artifact |
|---|---|
| `make tap` | `build/term.tap` |
| `make tap-zx` | `build/term-zx.tap` |
| `make if1` | `build/term-if1.tap` |
| `make if1-zx` | `build/term-zx-if1.tap` |

`RELEASE_NAME` in the Makefile and the release workflow gain the same `-zx`
element, so a release ships four TAPs.

File layout after the split. The pairs are alternatives — the Makefile compiles
exactly one of each into a given image.

| File | Built for | Contents |
|---|---|---|
| `render.c` | host + both targets | shared pure core: `glyph_row_byte`, `render_cell_bytes`, `render_pack4` |
| `render_hires.c` | host + Timex | pure: `render_cell_span`, `render_row_bytes` for 512 px / 80 cells |
| `render_ula.c` | host + ZX | pure: `render_cell_span`, `render_row_bytes` for 256 px / 40 cells |
| `blit_hires.c` | Timex only | hardware: flush, cursor, scroll across two display files |
| `blit_ula.c` | ZX only | hardware: flush, cursor, scroll over one display file |
| `video_hires.c` | Timex only | SCLD mode register, clear both files |
| `video_ula.c` | ZX only | attribute fill, border, clear one file |
| `machine.c` | both targets | port probes, guard, banner name |
| `machine_class.c` | host + both targets | pure: `machine_classify()` |
| `hires.c` / `hires.h` | host + Timex | survives as its own module with its own test, unchanged; `blit_hires.c` calls it rather than absorbing it |
| `ula.c` / `ula.h` (new) | host + ZX | its counterpart: `ula_addr()`, the one-file address helper |
| `video.c` | **removed** | split into `video_hires.c` and `video_ula.c`; `video.h` keeps the shared prototypes |
| `screen.c` | modified by §7 only | untouched by the target split; the performance items rewrite its scrolling and dirty tracking |
| `vtparse.c` | modified by §7 only | untouched by the target split, but it writes `s->dirty` directly in two places (`vtparse.c:105`, `vtparse.c:414`), so the dirty-group change reaches it |

`render_hires.c` and `render_ula.c` export the **same symbol names**
(`render_cell_span`, `render_row_bytes`), so they are alternatives, never linked
together. The host keeps both under test through the two-pass `test/run.sh` of
§10: the `-DTERM_TIMEX` pass links `render_hires.c`, the `-DTERM_ZX` pass links
`render_ula.c`, and `test_render.c` gates its geometry-specific assertions on the
same define. Linking both into one binary would be a duplicate-symbol error and
is never done.

This split also repairs an existing inconsistency: `render.c` documents a
PURE/hardware separation in its own header comment while holding both sides.

---

## 6. Machine detection

Ported to C from `docs/hardware/detect_machine_ay.asm` in the `attribute-wars`
repository (reference detector, 2026-06-23). No `.asm` file needs to enter the
build: `z80_inp(0x00FF)` performs the same 16-bit-port I/O transaction the
floating-bus probe requires — the full port in `BC`, so A15–A8 are `0x00`. (It is
not literally inlined as `ld bc,$00ff / in a,(c)`; under `sdcc_iy` it is a library
call that loads `BC` from `HL` and does `in l,(c)`. The bus cycle is equivalent,
which is what matters here.)

**Pure decision table** — the only part that can be tested without hardware:

```c
u8 machine_classify(u8 scld, u8 ay_timex, u8 ay_zx);
```

| `scld` | `ay_timex` | `ay_zx` | Result |
|---|---|---|---|
| 1 | 1 | — | `MACHINE_TS2068` |
| 1 | 0 | 1 | `MACHINE_TC2048` (AY expansion present) |
| 1 | 0 | 0 | `MACHINE_TC2048` |
| 0 | — | 1 | `MACHINE_ZX128` |
| 0 | — | 0 | `MACHINE_ZX48` |

`ay_timex` is only probed when `scld` is set; `machine_classify` treats it as 0
otherwise, and the host tests cover all eight input combinations including the
unreachable ones.

**Hardware layer** (`machine.c`): `machine_probe_scld()`,
`machine_probe_ay_timex()`, `machine_probe_ay_zx()`.

- The SCLD probe writes and reads back three patterns on port `$FF`, toggling
  only bits 3..5 — the palette bits — so screen mode, interrupt control, and
  EXROM/DOCK selection are preserved. The original value is restored on both
  exits. On a Spectrum the port is unattached: the write is harmless and the
  read-back does not match.
- The AY probes write and read back `$55`, `$AA`, `$3C` in register 11, then
  restore the original value. Register 11 remains **selected** afterwards,
  because the currently selected register cannot be read back. This is inherited
  from the reference detector and must be repeated as a comment on the probe in
  `machine.c`, since a later AY user would otherwise be surprised by it.

**Preconditions.** Detection runs once at startup, with interrupts disabled,
**before** video initialisation and **before** the IM2 handler is installed.

**Guard behaviour (D21, D22).** The Timex build calls `machine_probe_scld()`
first. If no SCLD is present and CAPS SHIFT is not held, it prints a short
message using the ULA text mode that is already active at boot and halts, rather
than painting an unreadable half-image. The Spectrum build never refuses; it only
reports the detected machine.

The CAPS SHIFT bypass is sampled in the same startup window as the probes —
keyboard half-row port `0xFEFE`, bit 0, active low — read directly, before the
IM2 handler is installed, since `keymap`/`keybuf` are not running yet.

Both builds print the detected name in the startup banner, so a detection fault
on real hardware is visible rather than silent.

---

## 7. Performance work carried into this slice

`docs/superpowers/reviews/2026-07-26-perf-review.md` measured the current
renderer and the 80-column implementation. It is committed alongside this spec
so every number below can be traced. Relevant figures, all from `z88dk-ticks`,
CPU T-states only, excluding display contention:

| Path | Measured | Diagnostic variant |
|---|---:|---:|
| 80-column row, current implementation | 670,861 T (~192 ms) | 194,617 T |
| Screen-model scroll | 449,787 T | 67,609 T |
| Video-RAM scroll | 682,052 T | 345,464 T |

A 40-column row is roughly half the work of an 80-column row, which extrapolates
to **~335,000 T (~96 ms) per row and ~2.3 s for a full repaint** if the ULA
blitter simply mirrors the current hi-res one. That is why D24 exists — this is
not a copy-and-adjust job. The 40-column figures are extrapolations from the
80-column measurements and must be confirmed by the benchmark in §9.

**In scope**, because this slice rewrites these paths anyway:

1. **Dirty tracking in groups of four cells** instead of one boolean per row.
   Four cells is the packer's natural unit. Sizing follows `COLS`:
   `DIRTY_GROUPS = (COLS + 3) / 4` and `DIRTY_BYTES = (DIRTY_GROUPS + 7) / 8` —
   20 groups in 3 bytes per row at 80 columns (72 bytes total), 10 groups in
   2 bytes per row at 40 columns (48 bytes total). `screen_putc` marks one group;
   erase, insert, and delete mark ranges.
2. **Exact cursor toggling.** The cursor is XOR-toggled off before an input batch
   is processed and back on after rendering, instead of dirtying its old and new
   rows. This removes the row dirtying in `main.c`.
3. **Block model scrolling.** Replace the cell-by-cell nested copy in `screen.c`
   with one overlapping `memmove()` for retained rows plus blanking of released
   rows, moving the dirty metadata in step.
4. **Hoisted blit addressing.** Compute the text-row base once, step scanlines by
   incrementing the address high byte, and handle both display files in one loop
   in the Timex build.
5. **An inline `attr == 0` group path.** Detect attributes per group; only
   affected groups take the general path. No helper calls or temporary arrays in
   the normal loop.
6. **A blank-row fast path.** A row whose cells are all blank with no attributes
   is cleared blockwise instead of packed cell by cell. The review notes this
   needs no additional font data.
7. **`render_scroll_region()` stops clearing the dirty flags of the scrolled
   region.** This is a hard prerequisite of items 2 and 3, not an optional extra:
   the review shows that clearing the whole region discards changes that were
   never in video RAM, and that `main.c` currently compensates by dirtying the
   bottom row again. Item 2 removes that compensation, so this must land in the
   same slice or deferred wrap will drop characters.

**Explicitly out of scope**, deferred to their own slices:

- The remaining half of the scroll/dirty **contract** defect (P0 in the review):
  `main.c` predicts scrolls from raw bytes and so misses the parser's VT, FF,
  `ESC D`, `ESC M`, and `ESC E` paths, which can move stale pixels or an inverted
  cursor. Items 2, 3, and 7 above implement four of the five steps of the
  review's robust model; the fifth — removing the raw-byte prediction from
  `main.c` and letting the parser drive every scroll — is deferred. This is a
  pre-existing correctness bug, not one this slice introduces, but the split is
  **uncomfortable and deliberate**: it is recorded as a risk in §11 rather than
  presented as clean separation.
- A Z80 assembly normal path.
- A page-aligned scanline-major font (`FONT_SCANLINE[8][256]`), which the review
  itself gates behind the IM2 reservation.
- Arbitrary scroll distance `n` for IL/DL.

---

## 8. Memory map and the IM2 hazard

Measured from `build/term.map` of the current 80-column build, after hoisting
`main()`'s two large locals to file scope (see below):

```
0x4000  display file        (ULA bitmap / hi-res even columns)
0x5800  attributes          (used by the ZX build, unused in hi-res)
0x6000  hi-res odd columns  (free RAM on a Spectrum — no conflict)
0x8000  program start (ORG)
0xDE62  end of image (__BSS_END_tail) -- was 0xCF1C before `screen_t scr` and
        `vtparse_t vt` moved from main()'s stack frame into BSS
        (0xDE62 - 0xCF1C = 3,910 bytes)
        0xEE00 - 0xDE62 = 3,998 bytes free
0xEE00  IM2 vector table, I = 0xEE                      <- IM2_TABLE_BASE / IM2_VECTOR_PAGE in include/im2.h
        257 bytes are architecturally required (0xEE00-0xEF00, one full page
        plus one byte for the worst-case vector read landing on the table's
        last byte). The installer writes exactly those 257 bytes with a
        single memset. The gate deliberately reserves one byte more than
        that, 258 (0xEE00-0xEF01 = 258 bytes: 0xEF01 - 0xEE00 + 1), as a
        margin against a future installer change that writes past its own
        boundary -- not because the code touches that 258th byte today.
0xEFEF  IM2 trampoline: JP keyboard_im2_isr, 3 bytes    <- IM2_TRAMPOLINE in include/im2.h
        table+trampoline end at 0xEFF2 (0xEFEF + 3 bytes); 0xFE8F - 0xEFF2 =
        3,741 bytes measured clear to the post-hoist low-water mark below;
        0xFD58 - 0xEFF2 = 3,430 bytes clear of the more conservative
        __crt_stack_size-implied floor
0xFD58  stack floor AS __crt_stack_size WOULD IMPLY (__register_sp 0xFF58
        minus __crt_stack_size 0x0200 = 0xFD58) -- post-hoist, this is now
        the MORE conservative of the two figures (see narrative below), not
        the less
0xFE8F  MEASURED low-water mark of real call/stack depth on target, after the
        hoist (im2-guard follow-up; not a linker symbol) -- see below
0xFF58  stack seed
```

The terminal uses IM2 so the keyboard is sampled 50 times a second and buffered;
with rendering costing many frames per row, a polled keyboard would drop
keystrokes.

Two hazards follow, and both are addressed by D23:

1. **Silent overwrite.** Neither address is known to the linker. An image that
   grows past the table base links cleanly and then destroys itself during IM2
   setup. `tools/check_image_limit.py` reads the map, compares the end of the
   image (including BSS) against the table base, and fails the build. It also
   checks the **other** side — that the table and trampoline clear the stack —
   because after relocation both live in the region the stack grows down into.
   Wired into `make tap`, `make tap-zx`, and CI.
2. **An artificially small margin.** The table originally sat at `0xD300`, right
   above the image, leaving only about 1,000 bytes of headroom while
   `0xFD58 - 0xD4D4 = 0x2884 = 10,372` bytes (~10.1 KB) between the trampoline
   at `0xD4D4` and the stack sat unused. Relocating the table raises the image
   margin to several kilobytes.

   The address is not free choice — it must satisfy all of:

   - the table base is `I << 8`, so it is 256-byte aligned;
   - the table is 257 bytes of one repeated value `X`, so any vector read yields
     the same address;
   - the trampoline therefore sits at `0x0101 * X` and occupies 3 bytes;
   - both must clear the stack, which lives in the same free region, with margin
     for its high-water mark.

   The base becomes a single named constant (`include/im2.h`), shared by the
   assembly and the gate script, so the two can never disagree.

   **Chosen values, as of the im2-guard follow-up:** base `0xEE00`
   (`I = 0xEE`), fill `0xEF`, trampoline `0xEFEF`. This is the third address
   this table has held. A 2026-07-27 fix wave had moved it from an interim
   `0xF900` (fill `0xFA`, trampoline `0xFAFA`, sized purely against
   `__crt_stack_size` -- 512 bytes configured, not measured) to `0xE000` (fill
   `0xE1`, trampoline `0xE1E1`), after a canary-fill measurement on target
   found real call/stack depth reaching down to `0xEF3D` -- deep enough that a
   direct hexdump caught live plaintext from an injected session overwriting
   the `0xF900` table while the program kept running. That fix wave flagged
   the "deep call chain" behind `0xEF3D` as a separate, unfixed problem, most
   likely in the render/scroll path.

   **It turned out to be one oversized stack frame, not a chain.** `main()`
   (`src/main.c`) declared `screen_t scr` and `vtparse_t vt` as locals:
   `screen_t` alone is `24 * 80 = 1,920` `cell_t`, 2 bytes each = `3,840`
   bytes, plus its scalars, with `vtparse_t` beside it, together accounting
   for nearly all of the measured `0xFF58 - 0xEF3D = 4,123` bytes (the `0xFF58`
   seed down to `0xEF3D` -- a call-stack low-water mark that includes return
   addresses and nested frames, not literally `sizeof(scr) + sizeof(vt)`,
   which is `3,840 + 38 (scr scalars) + 51 (vt) = 3,929` bytes -- the
   remaining ~194 bytes is real call-stack overhead). The im2-guard follow-up
   hoisted both to file-scope statics in `main.c`, for the same reason
   `render.c:100` keeps `row_glyphs`/`row_attrs`/`row_pixels` off the stack:
   it turns stack depth the linker cannot see into BSS that
   `tools/check_image_limit.py`'s `__BSS_END_tail` reading can. That grew the
   image end from `0xCF1C` to `0xDE62` (`0xDE62 - 0xCF1C = 3,910` bytes) -- so
   the table has to move again, this time because the image grew, not
   because the stack still reaches as deep as it used to.

   Remeasuring after the hoist (ZEsarUX ZRCP, same target: two full runs of
   five passes each -- boot + banner render, ~2.6 KB of injected text forcing
   scroll, a keypress burst, a heavier ~7 KB session with scroll-region
   changes and insert/delete-line churn, a second keypress burst -- sampled
   with ZRCP's `get-visualmem-written-dump` rather than a canary fill) found:
   the boot pass in both runs showed *every* byte from the new image end
   (`0xDE62`) to `0xFFFF` written 3-5 times, uniformly, regardless of session
   content -- the Spectrum's own power-on RAM test, not program activity, and
   excluded from the figures below. Past that artifact, the real low-water
   mark measured identically in both runs at **0xFE8F** (`0xFF58 - 0xFE8F =
   201` bytes below the `0xFF58` seed) -- comfortably inside the 512-byte
   `__crt_stack_size` budget, down from `0xFF58 - 0xEF3D = 4,123` measured
   bytes before the hoist. A third, still-heavier pass (a full 24-row screen
   clear+redraw plus scroll-region extremes, meant to check whether the mark
   trends deeper still, as it had across passes in the prior fix wave) was
   started but did not finish in the time available, so this measurement,
   though reproduced twice, is not proof that `0xFE8F` is the program's
   absolute deepest reach.

   Given that gap, `0xEE00` is chosen with margin against **both** figures,
   not `0xFE8F` alone: it gives `0xEE00 - 0xDE62` = 3,998 bytes of image
   margin, `0xFE8F - 0xEFF2` = 3,741 bytes of clearance below the measured
   mark, and `0xFD58 - 0xEFF2` = 3,430 bytes of clearance below the more
   conservative `__crt_stack_size`-implied floor -- comparable in scale to the
   prior fix wave's `4,324` / `3,417` split, despite the image having grown by
   nearly 4 KB since. See the hoist report referenced from this document's
   history for the full measurement writeup and raw data. Re-verified on
   target after the move: table contents, the `JP` opcode at the trampoline,
   `I=EE`, and a simulated keypress reaching `keybuf` through the relocated
   interrupt.

   **The gate's stack-floor arithmetic is, for the first time, on the
   conservative side of measured reality.** Before the hoist, the assumed
   floor `0xFD58` (`__register_sp` minus `__crt_stack_size`) understated real
   usage: measured depth reached `0xEF3D`, and `0xFD58 - 0xEF3D = 0xE1B =
   3,611` bytes past what the assumption allowed for. After the hoist,
   measured usage (`0xFE8F`) sits *above* (shallower than) the assumed floor:
   `0xFE8F - 0xFD58 = 311` bytes of margin. The 512-byte configured allowance
   is no longer fiction the program blows through (measured depth
   `0xFF58 - 0xEF3D = 4,123` bytes was over 8x the 512-byte budget) -- it is
   now bigger than what two measured sessions actually used. Whether it
   is bigger than what the untested heaviest session would use is the one
   question this measurement does not answer; `tools/check_image_limit.py`'s
   `stack_floor = __register_sp - __crt_stack_size` computation was left
   unchanged rather than tightened on that open question.

**The margin figures above are measured on this branch**, which already carries
the 80-column build. `row_glyphs[80]`, `row_attrs[80]`, and `row_pixels[80]`
account for the growth over the older 64-column `master` figure of 1,448 bytes;
the real pre-relocation margin on this branch (before the D23 table ever
moved) measured about 1,000 bytes -- a separate, older number from the
`0xEE00` margins above, kept here only as the original motivation for moving
the table out of the image's way at all.

---

## 9. Host integration

**terminfo.** `terminfo/zx-vt102.terminfo` is `timex-vt102` with `cols#40` and
its own name and description. `timex-vt102` is unchanged. The README documents
which entry goes with which TAP and how to `tic` both.

**Bridges.** `tools/if1_pty_bridge.py` and `tools/zesarux_stdio_bridge.py` gain a
width option so the PTY window size and exported `TERM` match the running build.
A wrong width here produces subtly wrong wrapping, so it must not default
silently to 80 for a 40-column terminal.

**Banner.** The 80-column box does not fit in 40. The banner frame is drawn with
a loop over `COLS` rather than stored as two string literals, which also saves
roughly 400 bytes of image — relevant given §8. It carries the detected machine
name and the active geometry.

---

## 10. Testing

**Host** (`make test`, `-std=c99 -Wall -Wextra -Werror`):

- `test/run.sh` compiles the pure suite **twice**, once with `-DTERM_TIMEX`
  (80 columns) and once with `-DTERM_ZX` (40); both must pass. This exercises
  wrapping, DECAWM, tab stops
  (the last stop lands on 32 at 40 columns), erase-in-line, insert/delete
  character, and cursor clamping at both widths **without any production code
  change** — the direct payoff of D19.
- `test_machine_class.c` (new, named for its module per the `test_<module>.c`
  convention): `machine_classify()` over all eight input combinations.
- `test_render.c`: ULA cell span, row packing (10 groups into bytes 1..30, bytes
  0 and 31 untouched), and the 6-px containment check extended to the ULA
  geometry. Existing hi-res assertions unchanged.
- Dirty-group tests: one `screen_putc` marks exactly one group; a scroll migrates
  group metadata with the rows instead of discarding it.
- Cursor tests for §7 item 2: an XOR toggle off followed by a toggle on restores
  the exact prior pixels, and neither marks a row or group dirty.
- A negative build check that `screen.h` refuses to compile when neither or both
  of `TERM_TIMEX` / `TERM_ZX` are defined (§5).

**Target** (ZEsarUX):

- `term.tap` on `--machine TC2048`: 80 columns, banner reads `TC2048`.
- `term-zx.tap` on `--machine 48k`: 40 columns, banner reads `ZX48`.
- `term-zx.tap` on `--machine TC2048`: 40 columns, banner reads `TC2048`, output
  correct. **This is the combination that proves D21's reverse claim** — the one
  that makes the Spectrum TAP the safe default and so softens D17's main cost.
  Without it that claim is sold but never verified.
- `term.tap` on `--machine 48k`: the guard message appears and the program halts
  (D21).
- `term.tap` on `--machine 48k` with CAPS SHIFT held during load: the guard is
  bypassed and the program runs on into its (unreadable) half-image, proving the
  D22 override works rather than merely being specified.
- `test/zesarux_smoke.py` gains a ULA cell reader — one file, scanline bytes
  1..30 — alongside the existing hi-res reader.

**Benchmarks** (D25): `z88dk-ticks` over one normal row at each width, one model
scroll, and one video scroll. Results are committed to `docs/perf/benchmarks.md`
as reference numbers, with the compiler version that produced them recorded
alongside, and printed by CI, not enforced.

This is a **deliberate reduction** of the harness the review asks for. The review
specifies coverage of mixed attribute rows, blank rows, full repaint, both scroll
directions, regions crossing the rows 7/8 and 15/16 third boundaries, arbitrary
`n`, IL, DL, LF, VT, FF, deferred wrap, IND, RI, NEL, cursor hide/show, and stack
high-water. What is kept here is the minimum needed to confirm the §7 numbers and
the §7 extrapolation for 40 columns. The rest belongs with the contract fix that
§7 defers, since most of those cases exist to test exactly that contract.

**Build gate** (§8): image-limit check on every TAP target.

---

## 11. Risks

| Risk | Handling |
|---|---|
| **TS2068 cannot load Spectrum-format TAPs.** Its ROM differs; z88dk targets it separately with `+ts2068`. | Accepted and documented. Detection recognises the machine, but the Timex TAP targets the TC2048. Running on a TS2068 needs a Spectrum-ROM cartridge or an emulator. |
| Clones or interfaces that decode port `$FF` incompletely make a genuine Timex fail the guard. | CAPS SHIFT bypass (D22), plus the machine name in the banner so the fault is visible. |
| The 80-column image may already be close to the IM2 table; the known margin is from the 64-column build. | First implementation task measures it; the gate then makes any future overrun a build failure (§8). |
| Two artifacts, and users may pick the wrong one. | The Spectrum TAP runs on ZX Spectrums **and the TC2048**, so it is the safe default between those two — verified by the fourth smoke combination in §10. It is not a universal fallback: a TS2068 loads neither TAP (row 1). The Timex TAP refuses rather than showing garbage. |
| The 40-column T-state figures are extrapolated, not measured. | The benchmark in §10 confirms them before the performance work is called done. |
| **The scroll/dirty contract is split across two slices** (§7). This slice implements four of the five steps of the review's robust model and defers the fifth. The deferred piece — `main.c` predicting scrolls from raw bytes — is a P0 the review places *before* the packer work. | Item 7 in §7 is mandatory precisely because removing the `main.c` cursor-row dirtying without it would drop characters on deferred wrap. The residual defect is unchanged from today's behaviour, not worsened. If the deferred piece proves entangled during implementation, pull it in rather than working around it. |

---

## 12. Out of scope

Colour via ULA attributes; 128K memory banking; AY sound including BEL through
the AY; DECCOLM or any runtime width switch; a universal single-TAP build; the
assembly renderer; the page-aligned font; the scroll/dirty contract fix (§7).

---

## 13. Milestones

1. Measure the baseline 80-column image size and the true IM2 margin; add
   `tools/check_image_limit.py` and wire it into the TAP targets and CI.
2. Relocate the IM2 vector table behind a single named base constant.
3. Split `render.c` into the shared core plus `render_hires.c`, and move the
   hardware paths into `blit_hires.c` and `video_hires.c`. No behaviour change;
   the existing tests must stay green.
4. Apply the performance items from §7 to `screen.c` and `blit_hires.c`, and
   record the 80-column benchmark numbers.
5. Add `machine_class.c` with host tests, then `machine.c` with the probes, the
   guard, and the banner name.
6. Add `render_ula.c` with host tests for span, packing, and containment.
7. Add `blit_ula.c` and `video_ula.c`, written **directly in the fast form**
   established by milestone 4; add the `tap-zx` and `if1-zx` targets; teach
   `test/run.sh` the second compile at `-DTERM_ZX`.
8. Add `zx-vt102.terminfo` and register it with the Makefile's existing
   `terminfo-check` target; add the bridge width option, the loop-drawn banner,
   and the README and `CLAUDE.md` updates (the latter names `hires.c` and
   `video.c` as the hardware-isolation points, which §5 changes).
9. ZEsarUX smoke on all **five** combinations in §10; record the 40-column
   benchmark numbers and wire the benchmark run into CI as a printed step.

The performance work is milestone 4, **before** the ULA blitter exists, so that
`blit_ula.c` is written once in its final shape. Doing it the other way round
would create the copy of the slow path that D24 exists to prevent.

**Milestones 1 and 2 should land as their own pull request, first.** They protect
the *current* 80-column build, depend on nothing in this spec, and by §8's own
argument must land before the image grows. Holding them behind the whole ZX
target would leave the hazard armed for the length of this work.
