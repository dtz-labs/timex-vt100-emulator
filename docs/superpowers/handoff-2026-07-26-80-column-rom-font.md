# Handoff — 80-column ROM font branch

**Branch:** `feat/80-columns` · **Last reviewed commit:** `b2459e4`
**Spec:** `docs/superpowers/specs/2026-07-26-80-column-rom-font-design.md`
**Plan:** `docs/superpowers/plans/2026-07-26-80-column-rom-font.md`

The eight planned tasks are complete and each passed its own review. The final
whole-branch review found **no Critical issues**; it named two Important fixes and
five cleanups, and **all seven have since been applied** in `2b77c81`, `b3b7bba`,
`b89af4c`, `b6a250f`.

## ⚠️ PR #2 merged without those four commits

PR #2 was merged at `bcf1a73`, which is **one commit before** the fix wave landed.
So `master` currently carries every issue listed below, while the fixes sit on
`feat/80-columns`. They need a follow-up merge.

Verify with:

```sh
git log --oneline origin/master..origin/feat/80-columns
```

The `wip/final-fix-wave` branch (`690f88f`) holds an earlier, interrupted and never-tested
attempt at the same wave. It is superseded — **delete it rather than merge it**.

After the fixes: 6238 host checks pass (up from 5980), `make ci` clean, `make tap` builds,
and `make smoke` against real ZEsarUX still reports 6/6 cells matching.

---

## What the four commits fix

The sections below describe the state of `master` as merged. Each is already fixed on
`feat/80-columns`; they are kept here as the record of what was wrong and why it mattered.

### Important

#### 1. `include/hires.h:8` and `:18` — a header contract this branch made false

Line 8 says *"A character cell is 8 px wide = exactly one byte (always aligned)"*.
Lines 18–19 document `hires_addr`'s first parameter as *"character column `col`
(0..63)"*.

Both were true before this branch. Neither is now: the parameter is a **scanline
byte index**. Every caller passes one — `src/render.c:236`, `src/render.c:240`,
`test/zesarux_smoke.py:91-94`.

Why it matters: someone adding a status line or a partial repaint writes
`hires_addr(s->cx, prow)` exactly as the header instructs. For `cx = 40` that
resolves to file0 index 20 — pixel columns 320–327, around cell 51 — and silently
corrupts an unrelated region. `render_cursor` was precisely such a call before this
branch, so this is a demonstrated misuse pattern, not a hypothetical.

Comments only. Do not change code in `hires.c`/`hires.h`. Reword to describe a byte
column 0..63 and note that a 6-px cell spans one or two of them, pointing at
`render_cell_span()`.

#### 2. `test/test_font.c` — the containment assertion covers only half the font

`test_no_glyph_exceeds_six_pixels` walks `0x20..0x7E` only. The DEC graphics page
`0xDF..0xFE` was hand-redrawn on this branch and has no containment assertion.

The data is clean — confirmed three separate times — so this is not a live bug. It
is an asymmetry: `tools/romfont.py` has `check_pitch()`, which aborts extraction if
a ROM glyph lights bits 1..0, while `tools/genfont.py` has no equivalent and its
`row_byte()` slices `s[:8]`, so a 7- or 8-character pattern row typed into `GRAPH`
is silently accepted.

Uncaught failure today: someone fills one of the blank `GRAPH` entries
(`0x62-0x65`, `0x79-0x7D`) and types a 7-character row. The header regenerates
cleanly, all 5980 checks pass, and on target that glyph's 7th pixel overwrites the
leftmost pixel of the cell to its right at phases 0 and 2.

Extend the check to `0xDF..0xFE`, with a failure message that says which page
failed. While in the file, `test_glyph_graph_0x7E` is missing the `g[6]`/`g[7]`
assertions its predecessor had — both should be `0x00`.

---

### Cleanups

3. **`site/index.html` — add `sandbox: true`.** JSSpeccy's default UI includes an
   archive.org tape search that fetches from a third party. An earlier note kept it,
   reasoning that `ui: false` would also remove the ▶ button the page copy points
   at. That evaluated the wrong option: in the shipped `jsspeccy.js`, `sandbox`
   gates drag-and-drop, `File > Open…`, `File > Find games…` and the auto-load
   toggle, while the ▶ Unpause button is added unconditionally. **Verify against the
   real asset before applying** — if `sandbox` does gate the play button, do not
   apply it, because the page copy would become wrong:
   ```sh
   gh release download v3.2.0-timex.1 --repo dtz-labs/jsspeccy3 \
     --pattern jsspeccy-dist.zip --output /tmp/js.zip --clobber
   ```
4. **`README.md:7`** — the "fork that adds Timex video" link points at
   `gasman/jsspeccy3` (upstream, which has no Timex machines). `site/index.html:47`
   and `:53` link correctly to `dtz-labs/jsspeccy3` while still crediting upstream
   with GPL-3 attribution; mirror that shape.
5. **`.github/workflows/pages.yml`** — the assemble step copies only
   `_emulator/dist/jsspeccy`, dropping the zip's root `COPYING`, so the page serves
   minified GPL-3 JavaScript without its licence text. Copy it to
   `_site/jsspeccy/COPYING`, **add a matching `test -f` line to the verification
   step**, and link it from the page footer. That verification list must stay in
   sync with what the page needs — it has already been wrong once, see below.
6. **`src/render.c`** — the comment above `row_glyphs`/`row_attrs` explains they are
   file-scope "to keep 240 bytes off the Z80 stack", four lines above `u8 g[COLS];`
   putting 80 bytes back on the same frame. Hoisting `g` to file scope is safe (the
   non-reentrancy argument in that comment applies to it verbatim) and makes the
   comment true. Behaviour must not change.
7. **Stale comments.** `src/font.c:2` says "8x8 glyph lookup" while
   `include/font.h:8` says the ASCII cell is 6 px wide in an 8-row glyph.
   `include/render.h`'s header block lists only `render_cell_bytes` as PURE, though
   `render_cell_span`, `render_pack4` and `render_row_bytes` all are.
   `test/test_render.c`'s banner no longer describes what the file tests.

---

## Human-only work

- **Retake the three README screenshots at 80 columns.** `README.md` on `master`
  already claims 80×24 a few lines above images that still show 64, so the public
  README contradicts itself right now.
- **ZEsarUX visual pass** — even margins, DEC box joints across the full width,
  cursor inverting exactly one cell at every bit phase, and whether 6 px at 80
  columns is comfortable to read. This is the one spec §10 item no automated
  evidence covers.

## Known and accepted — not defects

- Renderer speed. Spec decision **D16** explicitly defers optimisation. If it ever
  matters, measure with `z88dk-ticks` first, then add a Z80 fast path for the
  `attr == 0` case, which is the overwhelming majority of cells.
- 64-column mode dropped entirely, no runtime switch.
- TT3000 ROM glyph bitmaps committed to an MIT repo — raised during design, decided
  deliberately, recorded in spec §9, which also notes that repacking the previous
  glyphs is a complete substitute should it ever need undoing.

## Deferred findings triaged as "ship"

Kept here so they are not rediscovered as surprises.

| Item | Why it can ship |
|---|---|
| `render_cell_bytes` has no production caller but stays a public symbol | Pre-existing; tens of bytes of TAP. Mark "test-only export" if kept. |
| Ghost cursor on `ESC D`/`ESC M`/`SU`/`SD`/`IL`/`DL` scrolls without a pre-flush | Pre-existing and, if anything, narrower now (6-px ghost, was 8). Worth its own issue. |
| `pages.yml` uses `cancel-in-progress: true`; `release.yml` and GitHub's Pages template use `false` | Self-healing on the next push. Repo now carries both conventions. |
| No `actions/configure-pages` step | Verified genuinely unnecessary — the page uses only relative URLs and jsspeccy resolves worker/wasm/rom relative to the script URL. |
| `recv()`/`hexline()` in `test/zesarux_smoke.py` have no command/response correlation | Developer-run harness, not CI. Residual risk is a false PASS, not a false FAIL. Exposure roughly doubled (straddling cells now take up to 16 round-trips). Worth its own issue. |
| `genfont.py` output is not regeneration-checked in CI | Unlike `romfont.py`, `genfont.py` needs no ROM, so `diff <(python3 tools/genfont.py) src/font_graph_data.h` in `make ci` would be free. Good companion to fix 2. |

## Things this run learned the hard way

Recorded because each cost real time and would otherwise be repeated.

- **The deploy check's `test -f` list was missing `jsspeccy-worker.js`.** The runtime
  chain is `jsspeccy.js` → `jsspeccy-worker.js` → `jsspeccy-core.wasm`; the list
  covered links 1 and 3. Had the worker gone missing, the wasm check would still
  pass and the deploy would go green with a dead emulator. Found only by downloading
  the real asset and reading what the shipped JS actually fetches. The bug was
  inherited by copying `attribute-raid`'s workflow, which has the same gap.
- **Host tests never execute `render_flush`/`render_cursor`** — they write to
  absolute `0x4000`/`0x6000`. A wrong index passes the whole suite. That is why
  `render_row_bytes` was extracted: so the group-loop arithmetic has a permanent
  test against an independent reference painter.
- **A test and its implementation written by the same author can be wrong together.**
  The packer was validated against a reference painter from the same plan; it only
  became trustworthy once a reviewer derived the bit placement independently.
- **The plan's own startup box was 79 columns, not 80**, sitting directly beneath its
  own warning to count carefully. Instructions to "count carefully" are not
  instructions. The plan now carries a runnable width check instead.
