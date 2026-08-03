#!/usr/bin/env python3
"""Unit tests for tools/check_image_limit.py. Plain asserts, no pytest."""
import os
import re
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
        check(
            not any("overwrite" in p for p in rep.problems),
            "a zero-byte margin is not described as an overwrite",
        )


def test_fails_when_image_overlaps_the_table():
    with tempfile.TemporaryDirectory() as d:
        # Image ends 0x10 bytes past the table base: a real overlap, not a touch.
        rep = cil.analyse(write_map(d, "$D310"), 0xD300, 0xD4)
        check(not rep.ok, "an image overlapping the table fails")
        check(
            any("overwrite 16 bytes" in p for p in rep.problems),
            "the problem reports the correct overwritten byte count",
        )


def test_fails_when_table_hits_the_stack():
    with tempfile.TemporaryDirectory() as d:
        # Table at 0xFD00 spans 0xFD00..0xFE01, past the 0xFD58 stack floor.
        rep = cil.analyse(write_map(d, "$CD58"), 0xFD00, 0xFE)
        check(not rep.ok, "a table overlapping the stack fails")
        check(any("stack" in p for p in rep.problems), "the problem names the stack")


def test_fails_when_trampoline_touches_the_stack_floor_exactly():
    with tempfile.TemporaryDirectory() as d:
        # Table 0xD300..0xD401, trampoline 0xD4D4..0xD4D6 (0xD4D7 exclusive).
        # SP 0xD6D7 minus 0x0200 of stack puts the floor at exactly 0xD4D7:
        # the highest IM2 byte and the stack floor coincide -- a genuine
        # zero-byte margin, not an overlap.
        rep = cil.analyse(
            write_map(d, "$CD58", register_sp="$D6D7", stack_size="$0200"),
            0xD300,
            0xD4,
        )
        check(not rep.ok, "IM2 data touching the stack floor exactly fails")
        check(any("stack" in p for p in rep.problems), "the problem names the stack")
        check(
            any("zero-byte margin" in p for p in rep.problems),
            "the problem explains the zero margin rather than claiming an overlap",
        )
        check(
            not any("past the stack floor" in p for p in rep.problems),
            "a zero-byte margin is not described as past the floor",
        )


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


def test_table_and_trampoline_overlap_is_caught_by_its_tail_too():
    with tempfile.TemporaryDirectory() as d:
        # Fill 0xFE puts the trampoline at 0xFEFE, which *starts* below the
        # table base 0xFF00 -- a first-byte-only check would miss it -- but its
        # 3-byte span (0xFEFE..0xFF00) still reaches into the table's first byte.
        rep = cil.analyse(write_map(d, "$CD58"), 0xFF00, 0xFE)
        check(not rep.ok, "a trampoline whose tail reaches into the table fails")
        check(any("trampoline" in p for p in rep.problems), "the problem names the trampoline")


def test_fails_when_trampoline_lands_inside_the_image():
    with tempfile.TemporaryDirectory() as d:
        # Reviewer's first probe: fill 0x90 forces the trampoline to 0x9090,
        # inside the image body (well below the real image end 0xCF1C).
        rep = cil.analyse(write_map(d, "$CF1C"), 0xF900, 0x90)
        check(not rep.ok, "a trampoline landing inside the image fails")
        check(
            any("trampoline" in p and "image" in p for p in rep.problems),
            "the problem names both the trampoline and the image",
        )


def test_fails_when_trampoline_lands_in_the_display_file():
    with tempfile.TemporaryDirectory() as d:
        # Reviewer's second probe: fill 0x50 forces the trampoline to 0x5050,
        # in the display file -- below the image, but not literally "the image".
        rep = cil.analyse(write_map(d, "$CF1C"), 0xF900, 0x50)
        check(not rep.ok, "a trampoline landing in the display file fails")
        check(
            any("trampoline" in p and "image" in p for p in rep.problems),
            "the problem names the trampoline relative to the image end",
        )


def test_fails_when_trampoline_lands_in_rom():
    with tempfile.TemporaryDirectory() as d:
        # Reviewer's third probe: fill 0x10 forces the trampoline to 0x1010, in
        # ROM, where the installer's JP write is simply a no-op.
        rep = cil.analyse(write_map(d, "$CF1C"), 0xF900, 0x10)
        check(not rep.ok, "a trampoline landing in ROM fails")
        check(
            any("ROM" in p for p in rep.problems),
            "the problem names ROM",
        )


def test_shipped_im2_header_constants_pass_the_gate():
    """Pins the constants tools/check_image_limit.py's docstring warns about
    drifting: extracts IM2_TABLE_BASE / IM2_TABLE_FILL from include/im2.h with
    the same sed-style pattern the Makefile uses, so a header reformat that
    breaks that extraction fails a host test instead of only failing `make tap`
    (which needs z88dk and so never runs in the host-tests CI job)."""
    header_path = os.path.join(os.path.dirname(__file__), "..", "include", "im2.h")
    base_re = re.compile(r"^#define\s+IM2_TABLE_BASE\s+(0x[0-9A-Fa-f]+)")
    fill_re = re.compile(r"^#define\s+IM2_TABLE_FILL\s+(0x[0-9A-Fa-f]+)")
    base_text = ""
    fill_text = ""
    with open(header_path, "r") as fh:
        for line in fh:
            m = base_re.match(line)
            if m:
                base_text = m.group(1)
            m = fill_re.match(line)
            if m:
                fill_text = m.group(1)

    check(base_text != "", "IM2_TABLE_BASE was extracted from the header")
    check(fill_text != "", "IM2_TABLE_FILL was extracted from the header")

    base = int(base_text, 0)
    fill = int(fill_text, 0)
    # Deliberately NOT asserting literal addresses. Repeating them here would
    # make this file a second place to edit whenever the memory map moves, and
    # a test that only says "the header still says what the header says"
    # catches nothing. What matters is that the constants remain internally
    # consistent -- the same invariants include/im2.h #errors on in C, checked
    # here against the values the Makefile actually extracts.
    check(base % 0x100 == 0, "IM2_TABLE_BASE is 256-aligned")
    check(0x00 < fill <= 0xFF, "IM2_TABLE_FILL is a single byte")
    check(fill * 0x0101 >= base + 0x101,
          "the trampoline lies past the end of the 257-byte table")
    check(fill * 0x0101 + 3 < 0x10000,
          "the trampoline's three bytes fit below the top of memory")

    with tempfile.TemporaryDirectory() as d:
        rep = cil.analyse(write_map(d, "$DE62"), base, fill)
        check(rep.ok, "the gate accepts the shipped constants against a synthetic map")


def main():
    test_parses_symbols()
    test_ok_when_image_fits()
    test_fails_when_image_reaches_the_table()
    test_fails_when_image_overlaps_the_table()
    test_fails_when_table_hits_the_stack()
    test_fails_when_trampoline_touches_the_stack_floor_exactly()
    test_rejects_a_misaligned_table_base()
    test_table_and_trampoline_must_not_overlap()
    test_table_and_trampoline_overlap_is_caught_by_its_tail_too()
    test_fails_when_trampoline_lands_inside_the_image()
    test_fails_when_trampoline_lands_in_the_display_file()
    test_fails_when_trampoline_lands_in_rom()
    test_shipped_im2_header_constants_pass_the_gate()
    print("check_image_limit: %d checks passed" % CHECKS)


if __name__ == "__main__":
    main()
