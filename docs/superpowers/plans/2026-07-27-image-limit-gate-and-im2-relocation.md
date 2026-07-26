# Image-Limit Gate And IM2 Relocation Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Make it impossible for the program image to silently collide with the
hardcoded IM2 interrupt table, and move that table out of the way so the image
has room to grow.

**Architecture:** A Python script parses the z88dk link map and fails the build
when the image end crosses the IM2 table, or when the table and its trampoline
collide with the stack. The IM2 installer then moves from hand-written assembly
literals to a single header constant, and finally to a high address. Each stage
is verifiable on its own: the gate is meaningful before anything moves, the
refactor changes no addresses, and only the last task changes behaviour.

**Tech Stack:** C99 compiled with z88dk (`zcc +zx -SO3 -clib=sdcc_iy`), Python 3
standard library only, ZEsarUX with its ZRCP remote protocol for target
verification.

**Why this is its own plan:** it protects the *current* 80-column build, depends
on nothing in the ZX Spectrum target design, and by §8 of that spec must land
before the image grows. It ships as its own pull request, first.

## Global Constraints

- C99. Four-space indent, K&R braces, matching existing sources.
- Fixed-width aliases from `include/types.h`: `u8`, `s8`, `u16`, `s16`.
- No float, no `malloc`, no recursion, no headers that shadow z88dk system headers.
- Pointer-oriented APIs: pass structs by pointer or out-pointer. Never return a
  struct by value — it crashes SDCC's z80 backend.
- Only hardware modules may include `z80.h`. Pure modules must compile on the host.
- Host tests build with `cc -std=c99 -Wall -Wextra -Werror`.
- Python: standard library only. `make ci` runs `python3 -m py_compile` over
  `tools/*.py` and `test/zesarux_smoke.py`; new scripts must pass it.
- Target build: `make tap`. The Makefile auto-detects z88dk; on this host it
  lives at `$HOME/Programowanie/z88dk` and is **not** on `PATH` by default.
- IM2 hardware rules, all load-bearing: the vector table base is `I << 8`, so it
  is 256-byte aligned; the table is one repeated byte `X` so that any bus value
  yields the same vector; the trampoline therefore sits at `0x0101 * X` and
  occupies 3 bytes; the current installer writes **258** bytes (it seeds one byte
  then `LDIR`s 257), not the 257 the architecture requires.

**Measured baseline** (from `build/term.map` and `build/term_CODE.bin` of the
current build — re-measure, do not trust these if the branch has moved):

| Symbol | Value | Meaning |
|---|---|---|
| `__BSS_END_tail` | `$CD58` | end of image, BSS included |
| `__register_sp` | `$FF58` | initial stack pointer |
| `__crt_stack_size` | `$0200` | 512 bytes of stack, so the stack floor is `$FD58` |
| (hardcoded) | `$D300` | IM2 table base, `I = $D3` |
| (hardcoded) | `$D4D4` | IM2 trampoline, `= 0x0101 * 0xD4` |

Margin today: `$D300 - $CD58` = 1448 bytes. Free between the trampoline end
(`$D4D7`) and the stack floor (`$FD58`): 10,369 bytes.

---

### Task 1: The image-limit gate

A build that outgrows the IM2 table currently links cleanly and then destroys
itself at startup. This task makes that a build failure. It changes no addresses,
so it is useful immediately and cannot break the running program.

**Files:**
- Create: `tools/check_image_limit.py`
- Create: `test/test_check_image_limit.py`
- Modify: `Makefile` (variables near `MAP :=`, the `$(TAP)` and `$(IF1_TAP)`
  recipes, the `.PHONY` list, and the `ci` target)

**Interfaces:**
- Consumes: nothing from earlier tasks.
- Produces: `tools/check_image_limit.py`, invoked as
  `python3 tools/check_image_limit.py <map-file> --im2-base 0xD300 --im2-fill 0xD4`,
  exiting 0 on success and 1 on violation. Task 3 changes only the two argument
  values, via the Makefile variables `IM2_TABLE_BASE` and `IM2_TABLE_FILL`.

- [ ] **Step 1: Write the failing test**

Create `test/test_check_image_limit.py`. It writes small synthetic map files to a
temporary directory and checks the analysis function, so it needs no z88dk.

```python
#!/usr/bin/env python3
"""Unit tests for tools/check_image_limit.py. Plain asserts, no pytest."""
import os
import sys
import tempfile

sys.path.insert(0, os.path.join(os.path.dirname(__file__), "..", "tools"))

import check_image_limit as cil

CHECKS = 0


def check(cond, what):
    global CHECKS
    CHECKS += 1
    assert cond, what


def write_map(tmpdir, bss_end, register_sp="$FF58", stack_size="$0200"):
    path = os.path.join(tmpdir, "t.map")
    with open(path, "w") as fh:
        fh.write("CHAR_BELL                       = $0007 ; const, local, , x, , y\n")
        fh.write("__BSS_END_tail                  = %s ; const, public, def, , ,\n" % bss_end)
        fh.write("__register_sp                   = %s ; const, local, , x, , y\n" % register_sp)
        fh.write("__crt_stack_size                = %s ; const, local, , x, , y\n" % stack_size)
    return path


def test_parses_symbols():
    with tempfile.TemporaryDirectory() as d:
        syms = cil.parse_map(write_map(d, "$CD58"))
        check(syms["__BSS_END_tail"] == 0xCD58, "image end parsed")
        check(syms["__register_sp"] == 0xFF58, "stack pointer parsed")
        check(syms["__crt_stack_size"] == 0x0200, "stack size parsed")


def test_ok_when_image_fits():
    with tempfile.TemporaryDirectory() as d:
        rep = cil.analyse(write_map(d, "$CD58"), 0xD300, 0xD4)
        check(rep.ok, "an image ending below the table passes")
        check(rep.image_margin == 0xD300 - 0xCD58, "margin is table base minus image end")


def test_fails_when_image_reaches_the_table():
    with tempfile.TemporaryDirectory() as d:
        rep = cil.analyse(write_map(d, "$D300"), 0xD300, 0xD4)
        check(not rep.ok, "an image touching the table base fails")
        check(any("image" in p for p in rep.problems), "the problem names the image")


def test_fails_when_table_hits_the_stack():
    with tempfile.TemporaryDirectory() as d:
        # Table at 0xFD00 spans 0xFD00..0xFE01, past the 0xFD58 stack floor.
        rep = cil.analyse(write_map(d, "$CD58"), 0xFD00, 0xFE)
        check(not rep.ok, "a table overlapping the stack fails")
        check(any("stack" in p for p in rep.problems), "the problem names the stack")


def test_rejects_a_misaligned_table_base():
    with tempfile.TemporaryDirectory() as d:
        rep = cil.analyse(write_map(d, "$CD58"), 0xD301, 0xD4)
        check(not rep.ok, "a base that is not 256-byte aligned fails")
        check(any("aligned" in p for p in rep.problems), "the problem names alignment")


def test_table_and_trampoline_must_not_overlap():
    with tempfile.TemporaryDirectory() as d:
        # Fill 0xD3 puts the trampoline at 0xD3D3, inside the 0xD300..0xD401 table.
        rep = cil.analyse(write_map(d, "$CD58"), 0xD300, 0xD3)
        check(not rep.ok, "a trampoline inside the table fails")
        check(any("trampoline" in p for p in rep.problems), "the problem names the trampoline")


def main():
    test_parses_symbols()
    test_ok_when_image_fits()
    test_fails_when_image_reaches_the_table()
    test_fails_when_table_hits_the_stack()
    test_rejects_a_misaligned_table_base()
    test_table_and_trampoline_must_not_overlap()
    print("check_image_limit: %d checks passed" % CHECKS)


if __name__ == "__main__":
    main()
```

- [ ] **Step 2: Run the test to verify it fails**

```bash
cd /Volumes/SSD/Programowanie/timex-vt100-emulator-feat-zx-spectrum-target
python3 test/test_check_image_limit.py
```

Expected: `ModuleNotFoundError: No module named 'check_image_limit'`.

- [ ] **Step 3: Write the script**

Create `tools/check_image_limit.py`.

```python
#!/usr/bin/env python3
"""
Fail the build when the program image collides with the IM2 interrupt table,
or when that table and its trampoline collide with the stack.

main.c installs the IM2 vector table by writing absolute addresses. The linker
knows nothing about those writes: it will happily place code where the table is
about to land, producing a build that links cleanly and then overwrites itself
during interrupt setup. This script is the only thing standing between that and
a shipped TAP.

Reads the z88dk link map, whose lines look like:

    __BSS_END_tail                  = $CD58 ; const, public, def, , ,
"""
import argparse
import re
import sys

SYMBOL_RE = re.compile(r"^(\S+)\s*=\s*\$([0-9A-Fa-f]+)")

# The installer seeds one byte and then LDIRs 257 more, so it touches 258 bytes.
# 257 is what IM2 architecturally requires (I*256 + any bus byte, worst case
# 0x__FF, needs a high byte at +0x100); the extra byte is harmless but real, and
# the gate must reserve what the code actually writes.
IM2_TABLE_BYTES = 258
IM2_TRAMPOLINE_BYTES = 3  # JP nnnn

REQUIRED = ("__BSS_END_tail", "__register_sp", "__crt_stack_size")


class Report(object):
    def __init__(self):
        self.ok = True
        self.problems = []
        self.image_end = 0
        self.image_margin = 0
        self.stack_floor = 0
        self.table_end = 0
        self.trampoline = 0

    def fail(self, message):
        self.ok = False
        self.problems.append(message)


def parse_map(path):
    """Return {symbol: int} for every `NAME = $HEX` line in the map."""
    symbols = {}
    with open(path, "r") as fh:
        for line in fh:
            m = SYMBOL_RE.match(line)
            if m:
                symbols.setdefault(m.group(1), int(m.group(2), 16))
    return symbols


def analyse(map_path, im2_base, im2_fill):
    rep = Report()
    symbols = parse_map(map_path)

    missing = [s for s in REQUIRED if s not in symbols]
    if missing:
        rep.fail("map is missing %s; is this a z88dk link map?" % ", ".join(missing))
        return rep

    rep.image_end = symbols["__BSS_END_tail"]
    rep.stack_floor = symbols["__register_sp"] - symbols["__crt_stack_size"]
    rep.table_end = im2_base + IM2_TABLE_BYTES
    rep.trampoline = 0x0101 * im2_fill
    trampoline_end = rep.trampoline + IM2_TRAMPOLINE_BYTES
    rep.image_margin = im2_base - rep.image_end

    if im2_base & 0xFF:
        rep.fail(
            "IM2 table base 0x%04X is not 256-byte aligned; it must equal I << 8"
            % im2_base
        )

    if rep.image_end > im2_base:
        rep.fail(
            "image ends at 0x%04X, past the IM2 table base 0x%04X: "
            "interrupt setup would overwrite %d bytes of the program"
            % (rep.image_end, im2_base, rep.image_end - im2_base)
        )

    if im2_base <= rep.trampoline < rep.table_end:
        rep.fail(
            "IM2 trampoline 0x%04X (0x0101 * 0x%02X) lies inside the table "
            "0x%04X..0x%04X; pick a fill byte outside the table's own range"
            % (rep.trampoline, im2_fill, im2_base, rep.table_end - 1)
        )

    highest = max(rep.table_end, trampoline_end)
    if highest > rep.stack_floor:
        rep.fail(
            "IM2 data reaches 0x%04X, past the stack floor 0x%04X "
            "(SP 0x%04X minus %d bytes of stack)"
            % (
                highest,
                rep.stack_floor,
                symbols["__register_sp"],
                symbols["__crt_stack_size"],
            )
        )

    return rep


def main(argv=None):
    ap = argparse.ArgumentParser(description=__doc__)
    ap.add_argument("map", help="path to the z88dk .map file")
    ap.add_argument(
        "--im2-base",
        required=True,
        type=lambda v: int(v, 0),
        help="IM2 vector table base address, e.g. 0xD300",
    )
    ap.add_argument(
        "--im2-fill",
        required=True,
        type=lambda v: int(v, 0),
        help="byte the table is filled with, e.g. 0xD4",
    )
    args = ap.parse_args(argv)

    rep = analyse(args.map, args.im2_base, args.im2_fill)

    print(
        "image ends 0x%04X | IM2 table 0x%04X..0x%04X | trampoline 0x%04X | "
        "stack floor 0x%04X | margin %d bytes"
        % (
            rep.image_end,
            args.im2_base,
            rep.table_end - 1,
            rep.trampoline,
            rep.stack_floor,
            rep.image_margin,
        )
    )

    if not rep.ok:
        for problem in rep.problems:
            sys.stderr.write("image-limit: %s\n" % problem)
        return 1
    return 0


if __name__ == "__main__":
    sys.exit(main())
```

- [ ] **Step 4: Run the test to verify it passes**

```bash
python3 test/test_check_image_limit.py
```

Expected: `check_image_limit: 11 checks passed`.

- [ ] **Step 5: Wire the gate into the Makefile**

In `Makefile`, add these variables directly after the `IF1_MAP := $(IF1_APP).map`
line:

```make
# IM2 vector table placement. main.c writes these addresses absolutely, so the
# linker cannot know about them -- check_image_limit.py is what enforces them.
IM2_TABLE_BASE ?= 0xD300
IM2_TABLE_FILL ?= 0xD4
CHECK_IMAGE_LIMIT = python3 tools/check_image_limit.py
```

Append the check to both TAP recipes, so neither build can ship a colliding
image:

```make
$(TAP): $(SOURCES) $(HEADERS) $(BUILD_META) | $(BUILD_DIR) check-z88dk
	@echo "ZCC $(TAP)"
	@$(Z88DK_ENV) "$(ZCC)" $(Z88DK_TARGET) $(Z88DK_CFLAGS) $(Z88DK_DEFS) \
		$(SOURCES) -o "$(APP)" -create-app $(Z88DK_LDFLAGS)
	@$(CHECK_IMAGE_LIMIT) "$(MAP)" --im2-base $(IM2_TABLE_BASE) --im2-fill $(IM2_TABLE_FILL)

$(IF1_TAP): $(SOURCES) $(HEADERS) $(BUILD_META) | $(BUILD_DIR) check-z88dk
	@echo "ZCC $(IF1_TAP)"
	@$(Z88DK_ENV) "$(ZCC)" $(Z88DK_TARGET) $(Z88DK_CFLAGS) $(Z88DK_DEFS) \
		$(IF1_DEFS) $(SOURCES) -o "$(IF1_APP)" -create-app $(Z88DK_LDFLAGS)
	@$(CHECK_IMAGE_LIMIT) "$(IF1_MAP)" --im2-base $(IM2_TABLE_BASE) --im2-fill $(IM2_TABLE_FILL)
```

Add a `host-test` companion so the script's own tests run in CI. Change:

```make
host-test:
	CC="$(CC)" sh test/run.sh
```

to:

```make
host-test:
	CC="$(CC)" sh test/run.sh
	python3 test/test_check_image_limit.py
```

Add `test/test_check_image_limit.py` to the `python-check` target so it is
syntax-checked with the rest:

```make
python-check:
	python3 -m py_compile tools/*.py test/zesarux_smoke.py test/test_check_image_limit.py
```

- [ ] **Step 6: Verify the gate runs and reports the real margin**

```bash
make host-test
make tap
```

Expected: `make host-test` ends with the host suite passing plus
`check_image_limit: 11 checks passed`. `make tap` ends with a line of the form
`image ends 0x…… | IM2 table 0xD300..0xD401 | trampoline 0xD4D4 | stack floor 0xFD58 | margin N bytes`.

**Record the reported margin — this is the spec's milestone-1 measurement.**
The 1448-byte figure in the design is from the 64-column build; the 80-column
build on this branch is larger and its true margin is unknown until this runs.

- [ ] **Step 7: Verify the gate actually fails when it should**

Do not trust an assertion that has never fired. Force a violation:

```bash
make tap IM2_TABLE_BASE=0x9000 IM2_TABLE_FILL=0x91
```

Expected: exit status 2 from make, with
`image-limit: image ends 0x…… past the IM2 table base 0x9000: interrupt setup would overwrite … bytes of the program` on stderr, and **no** TAP left behind
(`.DELETE_ON_ERROR:` is already set in the Makefile).

- [ ] **Step 8: Commit**

```bash
git add tools/check_image_limit.py test/test_check_image_limit.py Makefile
git commit -m "build: fail the build when the image collides with the IM2 table

The IM2 vector table and trampoline are written to absolute addresses by
main.c, so the linker cannot know about them. An image that grows past the
table links cleanly and then overwrites itself during interrupt setup.

Parses the link map for the image end, the stack pointer and the stack size,
and checks the image against the table, the trampoline against the table, and
both against the stack floor."
```

---

### Task 2: One source of truth for the IM2 addresses

The addresses live as literals inside `__asm` blocks, where the gate script
cannot see them and a future edit can desynchronise them. This task moves them
into a header without changing a single address, so the program behaves exactly
as before and any failure is unambiguously caused by the refactor.

**Files:**
- Create: `include/im2.h`
- Modify: `src/main.c` (`install_keyboard_im2()`, currently at lines 190–207)
- Modify: `Makefile` (derive `IM2_TABLE_BASE` / `IM2_TABLE_FILL` from the header)

**Interfaces:**
- Consumes: the Makefile variables `IM2_TABLE_BASE` and `IM2_TABLE_FILL` from Task 1.
- Produces: `include/im2.h` defining `IM2_TABLE_BASE`, `IM2_TABLE_FILL` and
  `IM2_TRAMPOLINE`. Task 3 changes only the values in this header.

- [ ] **Step 1: Write the header**

Create `include/im2.h`.

```c
/*
 * im2.h -- placement of the Z80 interrupt-mode-2 vector table.
 *
 * In IM2 the CPU builds the vector address from the I register (high byte) and
 * a byte read off the data bus (low byte). On a Spectrum that bus byte is not
 * reliably any particular value, so the table is filled with ONE repeated byte:
 * whatever the bus supplies, the vector read yields the same address.
 *
 * That gives three linked constants:
 *   IM2_TABLE_BASE = I << 8          (so the base is 256-byte aligned)
 *   IM2_TABLE_FILL = the repeated byte
 *   IM2_TRAMPOLINE = 0x0101 * fill   (where the vector read lands)
 *
 * These addresses are written ABSOLUTELY at runtime. The linker does not model
 * them, so tools/check_image_limit.py parses this header and the link map and
 * fails the build if the image, the table, or the stack overlap. Changing a
 * value here changes the gate too -- that is the point of the single source.
 *
 * The build reads IM2_TABLE_BASE and IM2_TABLE_FILL out of this file with sed,
 * so keep each on its own line in the form `#define NAME 0xNNNN`.
 */
#ifndef IM2_H
#define IM2_H

#define IM2_TABLE_BASE 0xD300
#define IM2_TABLE_FILL 0xD4
#define IM2_TRAMPOLINE 0xD4D4

/* The table must be 256-byte aligned and the trampoline must be where the
 * vector read lands, or interrupts jump into nothing. Checked at compile time
 * so a typo cannot reach the target. */
#if (IM2_TABLE_BASE & 0xFF) != 0
#error "IM2_TABLE_BASE must be 256-byte aligned (it is I << 8)"
#endif
#if IM2_TRAMPOLINE != (0x0101 * IM2_TABLE_FILL)
#error "IM2_TRAMPOLINE must equal 0x0101 * IM2_TABLE_FILL"
#endif

#endif /* IM2_H */
```

- [ ] **Step 2: Rewrite the installer to use it**

In `src/main.c`, add `#include "im2.h"` and `#include <string.h>` to the includes,
then replace the whole `install_keyboard_im2()` function:

```c
/* Loaded into I by the asm below; a file-scope constant so the address comes
 * from im2.h rather than a literal the gate script cannot see. */
static const u8 im2_vector_page = (u8)(IM2_TABLE_BASE >> 8);

static void install_keyboard_im2(void)
{
    u8 *table = (u8 *)(uintptr_t)IM2_TABLE_BASE;
    u8 *tramp = (u8 *)(uintptr_t)IM2_TRAMPOLINE;
    u16 isr = (u16)(uintptr_t)&keyboard_im2_isr;

    intrinsic_di();

    /* 257 entries: the vector read can land on the last table byte and still
     * needs a high byte after it. */
    memset(table, IM2_TABLE_FILL, 257u);

    tramp[0] = 0xC3u;              /* JP nnnn */
    tramp[1] = (u8)(isr & 0xFFu);
    tramp[2] = (u8)(isr >> 8);

    __asm
        ld      a,(_im2_vector_page)
        ld      i,a
        im      2
        ei
    __endasm;
}
```

Add `#include <stdint.h>` if `uintptr_t` is not already available in `main.c`;
`src/render.c` already uses this `(u8 *)(uintptr_t)` idiom for absolute
addresses, so follow it.

Note the deliberate change from 258 written bytes to 257: the old code seeded a
byte and then `LDIR`ed 257 more. 257 is what the architecture requires. The gate
still reserves 258 (Task 1), which is now conservative rather than exact — that
is the safe direction, and Task 3 does not depend on the extra byte.

- [ ] **Step 3: Derive the Makefile variables from the header**

In `Makefile`, replace the two hardcoded defaults from Task 1 with values read
out of the header, so the two can never disagree:

```make
# Parsed out of include/im2.h so the gate and the program cannot disagree.
IM2_TABLE_BASE ?= $(shell sed -n 's/^#define IM2_TABLE_BASE[[:space:]]*\(0x[0-9A-Fa-f]*\).*/\1/p' include/im2.h)
IM2_TABLE_FILL ?= $(shell sed -n 's/^#define IM2_TABLE_FILL[[:space:]]*\(0x[0-9A-Fa-f]*\).*/\1/p' include/im2.h)
```

- [ ] **Step 4: Verify the parse and the build**

```bash
make print-vars | grep -i im2 || true
make tap
```

Expected: `make tap` prints the same
`IM2 table 0xD300..0xD401 | trampoline 0xD4D4` line as before Task 2. If either
variable comes out empty the `sed` did not match — fix the header formatting,
not the `sed`, since the header documents the required form.

Add the two variables to the `print-vars` target if they are not shown, matching
the existing echo lines there.

- [ ] **Step 5: Verify on the target that interrupts still work**

This refactor rewrote interrupt installation. The keyboard is driven entirely by
that interrupt, so a silent failure here means a dead terminal.

```bash
make smoke
```

Expected: the existing smoke test passes unchanged.

Then confirm the vector machinery itself, which the smoke test does not inspect.
With ZEsarUX running the TAP and ZRCP on port 10001, the helpers already in
`test/zesarux_smoke.py` (`cmd`, `rdbytes`) give:

- `rdbytes(s, 0xD300, 4)` must be `d4 d4 d4 d4`
- `rdbytes(s, 0xD4D4, 1)` must be `c3`
- `cmd(s, "get-registers")` must report `I=D3`

- [ ] **Step 6: Commit**

```bash
git add include/im2.h src/main.c Makefile
git commit -m "im2: put the vector table addresses in one header

The table base and trampoline were literals inside __asm blocks, invisible to
the build gate and free to drift apart. They now live in include/im2.h, which
the Makefile parses for the gate, with compile-time checks that the base is
256-byte aligned and the trampoline is 0x0101 * fill.

No address changes: this is the refactor, the move is separate."
```

---

### Task 3: Move the table above the image

**Files:**
- Modify: `include/im2.h` (the three values only)
- Modify: `docs/superpowers/specs/2026-07-26-zx-spectrum-target-design.md` (§8,
  replacing the deferred address with the chosen one)

**Interfaces:**
- Consumes: `IM2_TABLE_BASE`, `IM2_TABLE_FILL`, `IM2_TRAMPOLINE` from Task 2.
- Produces: no new interface. The image margin grows from ~1.4 KB to ~10.6 KB.

**The chosen address, and why.** The constraints from the Global Constraints
section admit few candidates. `0xF900` as the base gives `I = 0xF9`; filling with
`0xFA` puts the trampoline at `0xFAFA`, which is outside the table
(`0xF900..0xFA01`) and below the measured stack floor of `0xFD58`, leaving 603
bytes of clearance. The image may then run to `0xF900`, a margin of about 10,600
bytes against a current image ending near `0xCD58`.

- [ ] **Step 1: Prove the gate rejects the move before the values are consistent**

Confirm the compile-time checks fire, so a half-finished edit cannot build.
Temporarily set only the base in `include/im2.h`:

```c
#define IM2_TABLE_BASE 0xF900
#define IM2_TABLE_FILL 0xD4
#define IM2_TRAMPOLINE 0xD4D4
```

Run:

```bash
make tap
```

Expected: compilation fails with
`#error "IM2_TRAMPOLINE must equal 0x0101 * IM2_TABLE_FILL"` — no, it will not,
because that pair is still self-consistent. What this configuration actually
proves is the *other* check: the trampoline at `0xD4D4` now sits far below the
table at `0xF900`, and nothing forbids that, so the build succeeds and the
program breaks at runtime. **This is the case the compile-time checks cannot
catch**, which is why all three values move together in Step 2 and why Step 4
verifies `I` on the target. Revert this experiment before continuing.

- [ ] **Step 2: Move all three values together**

In `include/im2.h`:

```c
#define IM2_TABLE_BASE 0xF900
#define IM2_TABLE_FILL 0xFA
#define IM2_TRAMPOLINE 0xFAFA
```

- [ ] **Step 3: Build and read the new margin**

```bash
make tap
```

Expected: `image ends 0x…… | IM2 table 0xF900..0xFA01 | trampoline 0xFAFA | stack floor 0xFD58 | margin N bytes`, with N around 10,600 — roughly seven times
the old margin.

- [ ] **Step 4: Verify on the target**

```bash
make smoke
```

Expected: passes. Then, through ZRCP as in Task 2 Step 5:

- `rdbytes(s, 0xF900, 4)` must be `fa fa fa fa`
- `rdbytes(s, 0xFAFA, 1)` must be `c3`
- `cmd(s, "get-registers")` must report `I=F9`
- type a character through the ZRCP bridge and confirm it appears on screen —
  this is the end-to-end proof that the relocated interrupt still drives the
  keyboard

- [ ] **Step 5: Record the decision in the spec**

In `docs/superpowers/specs/2026-07-26-zx-spectrum-target-design.md`, §8, replace
the sentence beginning "The address is measured in milestone 1, but it is not
free choice" and its constraint list with the chosen values, keeping the
constraint list as the justification and adding: base `0xF900` (`I = 0xF9`),
fill `0xFA`, trampoline `0xFAFA`, stack floor `0xFD58`, clearance 603 bytes.
Update the memory map block in the same section to the new addresses.

- [ ] **Step 6: Commit**

```bash
git add include/im2.h docs/superpowers/specs/2026-07-26-zx-spectrum-target-design.md
git commit -m "im2: move the vector table to 0xF900, above the image

The table sat at 0xD300 with the image ending near 0xCD58 -- about 1.4 KB of
headroom, and the region between the trampoline and the stack floor at 0xFD58
sitting unused. Moving the table to 0xF900 with fill 0xFA (trampoline 0xFAFA)
raises the margin to roughly 10.6 KB and keeps 603 bytes clear of the stack.

Verified on the target: table contents, the JP opcode at the trampoline, I=F9,
and a keypress reaching the screen."
```

---

## Self-Review

**Spec coverage.** This plan covers milestones 1 and 2 of
`2026-07-26-zx-spectrum-target-design.md` §13, and the two hazards of §8: the
silent overwrite (Task 1) and the artificially small margin (Task 3). §8's
requirement that the gate check "the other side — that the table and trampoline
clear the stack" is Task 1 Step 3, `analyse()`. §8's requirement that the base
"becomes a single named constant shared by the assembly and the gate script" is
Task 2. The milestone-1 measurement is Task 1 Step 6. Everything else in the spec
belongs to the second plan and is deliberately absent.

**Placeholder scan.** No TBD, no "add error handling", no "similar to Task N".
Every code step carries the code. Task 3's address is a concrete value with its
derivation, not a deferred choice.

**Type consistency.** `analyse(map_path, im2_base, im2_fill)` returns a `Report`
with `.ok`, `.problems`, `.image_end`, `.image_margin`, `.stack_floor`,
`.table_end`, `.trampoline`; the test uses exactly those names, and `main()`
prints exactly those fields. `parse_map()` returns a dict keyed by the symbol
names in `REQUIRED`, which are the names the test writes into its synthetic maps.
The Makefile variables `IM2_TABLE_BASE` / `IM2_TABLE_FILL` are introduced in
Task 1, re-derived in Task 2, and consumed unchanged in Task 3.

**One honest gap.** Task 3 Step 1 is written as an experiment that *fails to
prove what it first appears to* — the compile-time checks cannot catch a
consistent-but-wrongly-placed trampoline. It is kept rather than deleted because
the reasoning is what justifies the runtime verification in Step 4. An
implementer who skips Step 1 loses nothing but that understanding.
