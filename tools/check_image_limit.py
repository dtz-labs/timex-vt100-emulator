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

    if rep.image_end == im2_base:
        rep.fail(
            "image ends exactly at the IM2 table base 0x%04X, leaving no "
            "margin: the gate refuses a zero-byte margin"
            % im2_base
        )
    elif rep.image_end > im2_base:
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
