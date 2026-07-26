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
