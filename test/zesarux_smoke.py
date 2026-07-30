"""
ZEsarUX ZRCP smoke test covering the five TAP/machine combinations from
docs/superpowers/specs/2026-07-26-zx-spectrum-target-design.md (10):

  1. term.tap    on TC2048          -- 80 columns, Timex banner, glyphs correct
  2. term-zx.tap on 48k             -- 40 columns, ZX banner,    glyphs correct
  3. term-zx.tap on TC2048          -- 40 columns, ZX banner -- THE SAFE-DEFAULT CLAIM
  4. term.tap    on 48k             -- guard message on screen, program halted
  5. term.tap    on 48k, CAPS SHIFT -- guard bypassed, program running

Row 3 is the one the whole two-TAP design argument rests on: the project tells
users the ZX TAP is the safe choice when unsure. If it does not actually run
correctly on a TC2048, that advice is wrong.

Every combination is checked by reading real Z80 memory over ZRCP
(`read-memory`), never by screenshot -- there is no display to photograph in
this harness, and reading memory directly is stricter anyway, provided it
samples the right cells (which is most of what this file is about).

Selectable via CLI flags (each also has an environment-variable default, for
backward compatibility with `make smoke`'s existing invocation):

  --tap FILE        TAP to load                    (env SMOKE_TAP)
  --machine NAME    ZEsarUX --machine value         (env SMOKE_MACHINE)
  --geom {hires,ula} display-file geometry          (env SMOKE_GEOM)
  --scenario {normal,guard,bypass,scroll}           (env SMOKE_SCENARIO)
  --port N          ZRCP port                       (env ZRCP_PORT)
  --wait SECONDS    settle time before connecting   (env SMOKE_WAIT)

scenario=normal   startup screen: box, help text, banner naming the build.
scenario=guard    the Timex build's SCLD guard must fire on a plain Spectrum.
scenario=bypass   CAPS SHIFT held through boot bypasses the guard (D22).
scenario=scroll   drives real scrolling and checks glyph content post-scroll,
                  not merely that something is present (carried forward from
                  Task 3/4/5's review: `make smoke` never exercised a scroll).
"""

import argparse
import os
import re
import socket
import subprocess
import tempfile
import time

ROOT = os.path.abspath(os.path.join(os.path.dirname(__file__), ".."))
DEFAULT_ZX = "/Applications/ZEsarUX.app/Contents/MacOS/zesarux"

# --------------------------------------------------------------------------
# ZRCP transport
# --------------------------------------------------------------------------


def recv(s, t=2.0):
    s.settimeout(t)
    buf = b""
    try:
        while True:
            d = s.recv(4096)
            if not d:
                break
            buf += d
            if buf.endswith(b"command> "):
                break
    except socket.timeout:
        pass
    return buf.decode("latin-1", "replace")


def cmd(s, c):
    s.sendall((c + "\n").encode("latin-1"))
    return recv(s)


def hexline(resp):
    """The first line of a ZRCP response that looks like a hex payload."""
    for ln in resp.splitlines():
        ln = ln.strip()
        if re.fullmatch(r"[0-9A-Fa-f]+", ln) and len(ln) >= 2:
            return ln
    return ""


def read_bytes(s, addr, n, chunk=4096):
    """Bulk-read `n` bytes at `addr`. Chunked defensively; a single
    `read-memory` call has been observed to return up to 6144 bytes cleanly
    in one line, but there is no documented upper bound, so this loops."""
    out = b""
    off = 0
    while off < n:
        want = min(chunk, n - off)
        resp = cmd(s, "read-memory %d %d" % (addr + off, want))
        out += bytes.fromhex(hexline(resp))
        off += want
    return out


def get_pc(s):
    r = cmd(s, "get-registers")
    m = re.search(r"PC=([0-9A-Fa-f]{4})", r)
    if not m:
        raise RuntimeError("could not parse PC from get-registers: %r" % r)
    return int(m.group(1), 16)


# --------------------------------------------------------------------------
# Project font tables (src/font_ascii_data.h, src/font_graph_data.h) --
# parsed from source, not re-typed, so decoding stays byte-for-byte tied to
# what font_glyph() actually returns on target.
# --------------------------------------------------------------------------

_ROW_RE = re.compile(r"\{([0-9xXa-fA-F,\s]+)\}\s*,\s*/\*\s*(0x[0-9A-Fa-f]+)")


def _parse_glyph_table(path):
    text = open(path, "r").read()
    table = {}
    for byts, code in _ROW_RE.findall(text):
        vals = [int(x.strip(), 16) for x in byts.split(",")]
        table[int(code, 16)] = bytes(vals)
    return table


def graphics_marker(code):
    """Map a DEC-graphics selecting letter's code (e.g. 0x6C for 'l') to a
    marker character distinguishable from the plain ASCII glyph of the same
    letter. Before this, both load_project_font()'s ascii_table AND
    graph_table entries for 'l' collapsed to the same Python character
    (chr(0x6C) == 'l') even though their PIXEL patterns differ (a box corner
    vs. the letter) -- so every box assertion in scenario_normal() ("row0[0]
    == 'l'", the run of 'q', etc.) passed whether the screen showed
    line-drawing glyphs or the literal letters. That is not a hypothetical:
    under the exact G0-graphics charset bug fixed elsewhere in this project's
    history, "Bridge" rendered with line-drawing glyphs in place of its
    lowercase letters, and "Bridge" in body_text still decoded true, because
    the two code paths produced the same character value. Setting the high
    bit here keeps the marker one Python character (so string comparisons
    and slicing still work) while making it impossible for a graphics glyph
    and its same-named ASCII letter to compare equal."""
    return chr(code | 0x80)


def load_project_font():
    """bytes(8) -> display char, for both the ASCII range (0x20-0x7E) and the
    DEC graphics range (cell codes 0xDF-0xFE, i.e. 0x5F-0x7E | 0x80).
    Graphics glyphs map to graphics_marker(code) -- NOT chr(code) -- so a
    graphics pixel pattern can never decode to the same character as the
    plain ASCII letter that happens to share its selecting code. See
    graphics_marker()'s docstring for why this distinction is load-bearing,
    not cosmetic."""
    ascii_table = _parse_glyph_table(os.path.join(ROOT, "src", "font_ascii_data.h"))
    graph_table = _parse_glyph_table(os.path.join(ROOT, "src", "font_graph_data.h"))
    rev = {}
    for code, g in ascii_table.items():
        rev[g] = chr(code)
    for code, g in graph_table.items():  # code is 0x5F-0x7E here
        rev.setdefault(g, graphics_marker(code))
    return rev


def load_rom_font(s):
    """bytes(8) -> chr, read LIVE from the 48K ROM's own character set
    (0x3D00..0x3FFF, 96 chars x 8 bytes, codes 32-127) -- this is what the
    guard's plain RST 0x10 print uses, not this project's font.c. Reading it
    from the running machine rather than hardcoding it means the decode is
    tied to whatever ROM ZEsarUX actually loaded, not to a copy-pasted table.
    """
    rom = read_bytes(s, 0x3D00, 768)
    rev = {}
    for code in range(32, 128):
        g = rom[(code - 32) * 8 : (code - 32) * 8 + 8]
        rev[g] = chr(code)
    return rev


# --------------------------------------------------------------------------
# Display-file geometry: hi-res (two files, 16px margin) and ULA (one file,
# 8px margin). Both use the same "thirds" scanline-offset formula; they
# differ only in file count and left margin. Mirrors render_cell_span() /
# hires_addr() / ula_addr() -- see src/render_hires.c, src/render_ula.c,
# src/hires.c, src/ula.c.
# --------------------------------------------------------------------------

GEOMETRIES = {
    "hires": {"left_margin_px": 16, "cell_px": 6, "two_files": True, "cols": 80},
    "ula": {"left_margin_px": 8, "cell_px": 6, "two_files": False, "cols": 40},
}


def cell_span(geom, col):
    px = geom["left_margin_px"] + geom["cell_px"] * col
    sh = px & 7
    mask0 = 0xFC >> sh
    mask1 = (0xFC << (8 - sh)) & 0xFF if sh > 2 else 0
    return px >> 3, sh, mask0, mask1


def thirds_offset(prow):
    return ((prow & 0xC0) << 5) | ((prow & 0x07) << 8) | ((prow & 0x38) << 2)


def read_display(s, geom):
    """Bulk-read the whole display file(s) once. Returns {0x4000: bytes,
    0x6000: bytes-or-None}. Decoding many cells (banner rows, a full-screen
    scroll check) from one bulk read is both far fewer ZRCP round trips than
    reading cell-by-cell, and immune to the display changing mid-decode."""
    d = {0x4000: read_bytes(s, 0x4000, 0x2000)}
    if geom["two_files"]:
        d[0x6000] = read_bytes(s, 0x6000, 0x2000)
    else:
        d[0x6000] = None
    return d


def byte_at(display, geom, byte_idx, prow):
    if geom["two_files"]:
        base = 0x6000 if (byte_idx & 1) else 0x4000
        off = thirds_offset(prow) + (byte_idx >> 1)
    else:
        base = 0x4000
        off = thirds_offset(prow) + byte_idx
    return display[base][off]


def cell_bytes(display, geom, row, col):
    byte_idx, sh, mask0, mask1 = cell_span(geom, col)
    out = []
    for i in range(8):
        prow = row * 8 + i
        g = ((byte_at(display, geom, byte_idx, prow) & mask0) << sh) & 0xFF
        if mask1:
            g |= (byte_at(display, geom, byte_idx + 1, prow) & mask1) >> (8 - sh)
        out.append(g)
    return bytes(out)


def decode_row(display, geom, font, row, col0, col1):
    return "".join(
        font.get(cell_bytes(display, geom, row, c), "?") for c in range(col0, col1)
    )


# --------------------------------------------------------------------------
# ZEsarUX process lifecycle
# --------------------------------------------------------------------------


def launch(zx, tap, machine, port, fastautoload=True, spool=None, keylength=None):
    args = [
        zx,
        "--noconfigfile",
        "--machine",
        machine,
        "--tape",
        tap,
        "--vo",
        "null",
        "--ao",
        "null",
        "--nosplash",
        "--enable-remoteprotocol",
        "--remoteprotocol-port",
        str(port),
        "--quickexit",
    ]
    if fastautoload:
        args.append("--fastautoload")
    if spool:
        args += [
            "--sendtextkeystrokes-file",
            spool,
            "--sendtextkeystrokes-play",
            "--sendtextkeystrokes-keylength",
            str(keylength or 100),
            "--sendtextkeystrokes-nodelay",
        ]
    return subprocess.Popen(args, stdout=subprocess.DEVNULL, stderr=subprocess.STDOUT)


def connect(port, settle, timeout=5):
    time.sleep(settle)
    s = socket.create_connection(("127.0.0.1", port), timeout=timeout)
    recv(s)
    return s


def make_caps_shift_spool(n=50):
    """A `--sendtextkeystrokes-file` spool of `n` repeated CAPS-SHIFT-only
    presses (byte value 128; see the ZEsarUX binary's own --joystickkeybt
    help string: "to simulate Caps shift, use key value 128"). Empirically
    confirmed (not merely per that doc note) to hold row 0xFEFE bit 0 down
    continuously across many seconds when keylength=100 (2000 ms/press) and
    --sendtextkeystrokes-nodelay chains presses back-to-back with no gap --
    see the task report for the register trace that established this.
    """
    path = os.path.join(tempfile.gettempdir(), "smoke_capshold.spool")
    with open(path, "wb") as f:
        f.write(bytes([128]) * n)
    return path


# --------------------------------------------------------------------------
# Map-file / header-constant lookups (kept dynamic, never hardcoded, so a
# future code change cannot silently make these stale).
# --------------------------------------------------------------------------


def parse_map_symbol(map_path, name):
    pattern = re.compile(r"^_?%s\s*=\s*\$([0-9A-Fa-f]+)\b" % re.escape(name), re.M)
    text = open(map_path, "r", encoding="latin-1").read()
    m = pattern.search(text)
    if not m:
        raise RuntimeError("symbol %r not found in %s" % (name, map_path))
    return int(m.group(1), 16)


def parse_header_define(header_path, name):
    pattern = re.compile(r"^\s*#define\s+%s\s+(0x[0-9A-Fa-f]+|\d+)" % re.escape(name), re.M)
    text = open(header_path, "r").read()
    m = pattern.search(text)
    if not m:
        raise RuntimeError("#define %r not found in %s" % (name, header_path))
    return int(m.group(1), 0)


# --------------------------------------------------------------------------
# conn_zrcp_inject mailbox (src/conn.c) -- lets a scenario feed arbitrary
# bytes into the running program's RX stream without going through keyboard
# emulation. Same mechanism as tools/zesarux_pipe_inject.py.
# --------------------------------------------------------------------------


def mailbox_addrs(map_path):
    return (
        parse_map_symbol(map_path, "conn_zrcp_inject_len"),
        parse_map_symbol(map_path, "conn_zrcp_inject_data"),
    )


def mailbox_wait_empty(s, len_addr, timeout=3.0):
    deadline = time.time() + timeout
    while True:
        if read_bytes(s, len_addr, 1)[0] == 0:
            return True
        if time.time() > deadline:
            return False
        time.sleep(0.02)


def mailbox_inject(s, len_addr, data_addr, chunk):
    if not mailbox_wait_empty(s, len_addr):
        raise RuntimeError("conn_zrcp_inject mailbox never emptied")
    values = " ".join(str(b) for b in chunk)
    cmd(s, "write-memory %d %s" % (data_addr, values))
    cmd(s, "write-memory %d %d" % (len_addr, len(chunk)))
    if not mailbox_wait_empty(s, len_addr):
        raise RuntimeError("conn_zrcp_inject mailbox never drained")


def mailbox_send(s, len_addr, data_addr, payload, chunk_size=32):
    for i in range(0, len(payload), chunk_size):
        mailbox_inject(s, len_addr, data_addr, payload[i : i + chunk_size])


# --------------------------------------------------------------------------
# Scenarios
# --------------------------------------------------------------------------


def check(checks, name, ok, detail=""):
    checks.append((name, ok, detail))
    print(
        "%-40s %-4s %s"
        % (name, "PASS" if ok else "FAIL", detail)
    )


def scenario_normal(s, geom_name, cols, banner_contains, checks):
    """The startup screen: box rules, help text, and (new in Task 11) the
    banner line that names the build -- the check that gives rows 2 and 3
    of the combination matrix (and row 1) actual teeth, not just "some box
    is on screen"."""
    geom = GEOMETRIES[geom_name]
    font = load_project_font()
    display = read_display(s, geom)

    # Graphics markers (see graphics_marker()) are the high-bit-set forms of
    # the DEC special-graphics selecting letters feed_hrule()/feed_row() use
    # for the box corners, rule, and vertical bars. Asserting against these
    # markers -- rather than the plain letters -- is what makes this check
    # able to fail: with the pre-fix font map, both the correct rendering and
    # the G0-graphics charset bug (line-drawing glyphs bleeding into plain
    # text) decoded to the exact same characters, so no assertion here could
    # ever tell them apart.
    gfx_l = graphics_marker(ord("l"))
    gfx_k = graphics_marker(ord("k"))
    gfx_q = graphics_marker(ord("q"))
    gfx_x = graphics_marker(ord("x"))

    row0 = decode_row(display, geom, font, 0, 0, cols)
    check(checks, "row0 left corner is graphics 'l'", row0[0] == gfx_l, repr(row0[0]))
    check(checks, "row0 right corner is graphics 'k'", row0[-1] == gfx_k, repr(row0[-1]))
    check(
        checks,
        "row0 interior is graphics horizontal rule 'q'",
        row0[1:-1] == gfx_q * (cols - 2),
        "%r" % (row0,),
    )

    row1 = decode_row(display, geom, font, 1, 0, cols)
    check(
        checks,
        "row1 contains title text",
        "VT-102 TERMINAL EMULATOR" in row1,
        repr(row1),
    )
    check(
        checks,
        "row1 left/right bars are graphics 'x'",
        row1[0] == gfx_x and row1[-1] == gfx_x,
        repr((row1[0], row1[-1])),
    )
    # The interior text must be plain ASCII, not graphics markers -- this is
    # the other half of the collision this test used to miss: a charset bug
    # that leaks G1/graphics into ordinary text (as the historical bug did)
    # would show up here as a graphics marker byte inside plain title text.
    check(
        checks,
        "row1 interior title text is plain ASCII (no graphics markers)",
        all(ord(c) < 0x80 for c in row1[1:-1].rstrip()),
        repr(row1[1:-1]),
    )

    # The free-text help/version/banner lines below the box (rows 2..23) are
    # plain literal text, not COLS-aware like feed_hrule()/feed_row() -- at
    # 40 columns some of them are long enough to wrap, shifting every row
    # after the wrap down by one relative to the 80-column build. Rather
    # than hardcode per-geometry row numbers (fragile, and exactly the kind
    # of width-dependent literal this whole task line of work removed from
    # main.c), search the WHOLE free-text area for these substrings, joined
    # with no row separator so a mid-word wrap cannot split a match. This is
    # still an exact byte-for-byte glyph comparison per cell (via the font
    # table) -- only the row bookkeeping is relaxed, not the check itself.
    body_rows = [decode_row(display, geom, font, r, 0, cols) for r in range(2, 24)]
    body_text = "".join(body_rows)
    body_text_nl = "\n".join(body_rows)

    check(checks, "help text contains 'Bridge'", "Bridge" in body_text, repr(body_text_nl))
    check(checks, "help text contains 'ENTER sends CR'", "ENTER sends CR" in body_text, "")
    check(checks, "help text contains 'Ready.'", "Ready." in body_text, "")
    check(
        checks,
        "banner names the build (%r)" % banner_contains,
        ("HW: " + banner_contains) in body_text,
        "",
    )


def scenario_guard(s, checks, map_path):
    """The Timex build's SCLD guard on a plain Spectrum: PC must be frozen
    inside guard_halt's 2-byte halt/jr loop (parsed from the TAP's own .map,
    never hardcoded), and the guard message must be decodable from the ROM's
    OWN font, read live -- this is a plain RST 0x10 ROM print, not this
    project's font.c, so the project font table would not match it."""
    guard_halt = parse_map_symbol(map_path, "guard_halt")

    pc1 = get_pc(s)
    time.sleep(0.5)
    pc2 = get_pc(s)
    check(
        checks,
        "PC frozen inside guard_halt (halt with DI never resumes)",
        pc1 == pc2 and guard_halt <= pc1 <= guard_halt + 1,
        "guard_halt=$%04X pc1=$%04X pc2=$%04X" % (guard_halt, pc1, pc2),
    )

    rom_font = load_rom_font(s)
    display_bytes = read_bytes(s, 0x4000, 6144)

    def rom_cell(row, col):
        out = []
        for sc in range(8):
            prow = row * 8 + sc
            off = thirds_offset(prow) + col
            out.append(display_bytes[off])
        return bytes(out)

    lines = []
    for row in range(24):
        line = "".join(rom_font.get(rom_cell(row, c), "?") for c in range(32))
        if line.strip(" ?"):
            lines.append(line)
    screen_text = "\n".join(lines)
    check(
        checks,
        "guard message visible (decoded via live ROM font)",
        "THIS BUILD NEEDS A TIMEX" in screen_text,
        repr(screen_text),
    )


def scenario_bypass(s, geom_name, cols, banner_contains, checks, map_path):
    """CAPS SHIFT held through boot (D22): the guard must be bypassed and
    the program must be genuinely running -- not merely "not halted". PC
    landing repeatedly on IM2_TRAMPOLINE (parsed from include/im2.h) is the
    same strong "really running" signal Task 6 established (R alone is weak:
    it increments even during a genuine HALT). On top of that, this scenario
    re-runs the full `normal` content checks against the SAME TAP, proving
    the bypassed run renders correctly, not just that the CPU moved."""
    im2_trampoline = parse_header_define(
        os.path.join(ROOT, "include", "im2.h"), "IM2_TRAMPOLINE"
    )
    guard_halt = parse_map_symbol(map_path, "guard_halt")
    pcs = []
    for _ in range(5):
        pcs.append(get_pc(s))
        time.sleep(0.3)
    # A frozen PC (equal on every sample, or parked in guard_halt) would mean
    # the guard fired after all. A live system's PC instead visits both
    # ordinary loop code AND the IM2 trampoline across a handful of samples
    # 300ms apart (50 Hz interrupts land it there often, but not on every
    # single ZRCP register poll) -- seeing both is stronger evidence of
    # genuine execution than PC merely differing between polls would be, and
    # far stronger than R alone (R increments even during a real HALT).
    never_halted = all(pc != guard_halt and pc != guard_halt + 1 for pc in pcs)
    saw_trampoline = im2_trampoline in pcs
    check(
        checks,
        "PC never parked in guard_halt, and visits the IM2 trampoline (running)",
        never_halted and saw_trampoline,
        "guard_halt=$%04X im2_trampoline=$%04X pcs=%s"
        % (guard_halt, im2_trampoline, ["$%04X" % p for p in pcs]),
    )
    scenario_normal(s, geom_name, cols, banner_contains, checks)


def scenario_scroll(s, geom_name, cols, checks, map_path, n_lines):
    """Carried forward from Task 3/4/5's review notes: `make smoke` never
    exercised a scroll, so passing it never confirmed the overlapping
    memmove()s in screen_scroll()/blit_scroll_region() on real Z80 memory,
    nor (after Task 5 made the blitter render only dirty groups) that a
    dropped dirty mark does not leave a stale/blank cell. This drives
    `n_lines` distinct lines through the conn_zrcp_inject mailbox (each
    `LNNN\\r\\n`), which forces real scrolling in the running program, then
    decodes the resulting rows against the font table and checks:
      - the last `n_lines` rows show exactly the expected trailing content,
        in order (byte-exact against font_ascii_data.h, not "non-blank"),
      - columns past each line's own text are genuinely blank -- the exact
        regression a dropped dirty mark would produce (stale glyph content
        bleeding through from whatever used to occupy that cell)."""
    geom = GEOMETRIES[geom_name]
    font = load_project_font()
    len_addr, data_addr = mailbox_addrs(map_path)

    payload = b"".join(("L%03d\r\n" % i).encode() for i in range(n_lines))
    mailbox_send(s, len_addr, data_addr, payload)
    # The mailbox drain confirms conn_rx_read() has consumed every byte, but
    # blit_flush() only runs once per pump_conn() batch/main-loop iteration
    # after that -- give the render a couple of extra main-loop iterations
    # to land before reading display memory, or the read can race the last
    # rows' repaint (observed: a byte-exact match on 46/48 rows plus a
    # transiently torn row without this margin).
    time.sleep(2.0)

    display = read_display(s, geom)

    # Row bookkeeping, worked out empirically and confirmed against a hand
    # trace of screen_lf()/screen_cr(): each "LNNN\r\n" line's OWN trailing
    # LF is what scrolls it, because by the time that LF arrives the cursor
    # is already sitting at row 23 (the bottom of the scroll region) from
    # the PREVIOUS line's LF. So a line is written to row 23, then its own
    # LF immediately scrolls it up to row 22 -- meaning the true bottom row
    # (23) is NEVER left holding a line's text; it is always the fresh,
    # fully-blanked row a scroll just exposed, with the cursor now sitting
    # on it. The last 23 lines therefore land on rows 0..22 (oldest at the
    # top), and row 23 is checked separately as the blank/cursor row.
    # Assumes n_lines is large enough that the startup banner has been
    # completely scrolled off screen (n_lines comfortably greater than 24 is
    # safest; the default of 30 satisfies this and is what `make smoke` uses).
    rows_with_content = min(23, n_lines)
    first_line = n_lines - rows_with_content
    expected_lines = ["L%03d" % i for i in range(first_line, n_lines)]
    for row, want in enumerate(expected_lines):
        got = decode_row(display, geom, font, row, 0, len(want))
        check(checks, "post-scroll row %d == %r" % (row, want), got == want, "got=%r" % got)
        # Columns past the line's own text must be blank -- exactly the
        # "dropped dirty mark" regression named in the Task 3/4/5 reviews.
        # Decoded across the FULL row width (not just the first ~15 columns):
        # dirty groups are 4 cells wide, so a narrower check only exercises
        # groups 0-3 and would never read groups 4..19 (Timex) / 4..9 (ZX) --
        # exactly where a dropped dirty mark elsewhere on the row would be
        # invisible. This is one bulk read either way (read_display() already
        # pulled the whole display file up front), so decoding the rest costs
        # nothing extra.
        tail_col = cols
        tail = decode_row(display, geom, font, row, len(want), tail_col)
        check(
            checks,
            "post-scroll row %d tail (cols %d..%d) blank" % (row, len(want), tail_col),
            tail == " " * len(tail),
            "got=%r" % tail,
        )

    # Row 23: the cursor's row, freshly scrolled in and blanked, so every
    # cell except the cursor's own (col 0, XORed by blit_cursor_toggle() and
    # therefore not expected to match the plain font table) must be blank --
    # the same "no stale content" check as the tail check above, just
    # covering the one row that check skips.
    cursor_row = decode_row(display, geom, font, 23, 1, cols)
    check(
        checks,
        "post-scroll row 23 (cursor row) blank past col 0",
        cursor_row == " " * len(cursor_row),
        "got=%r" % cursor_row,
    )


# --------------------------------------------------------------------------
# main
# --------------------------------------------------------------------------


def main():
    ap = argparse.ArgumentParser(description=__doc__)
    ap.add_argument(
        "--tap", default=os.environ.get("SMOKE_TAP", os.path.join(ROOT, "build", "term.tap"))
    )
    ap.add_argument("--machine", default=os.environ.get("SMOKE_MACHINE", "TC2048"))
    ap.add_argument(
        "--geom", choices=list(GEOMETRIES), default=os.environ.get("SMOKE_GEOM", "hires")
    )
    ap.add_argument(
        "--scenario",
        choices=["normal", "guard", "bypass", "scroll"],
        default=os.environ.get("SMOKE_SCENARIO", "normal"),
    )
    ap.add_argument("--port", type=int, default=int(os.environ.get("ZRCP_PORT", "10001")))
    ap.add_argument("--wait", type=float, default=None)
    ap.add_argument("--cols", type=int, default=None)
    ap.add_argument("--map", default=None)
    ap.add_argument("--banner-contains", default=None)
    ap.add_argument("--scroll-lines", type=int, default=30)
    ap.add_argument("--zx", default=os.environ.get("ZX", DEFAULT_ZX))
    args = ap.parse_args()

    # ZEsarUX silently fails to load a relative --tape path (observed: it logs
    # "Unable to open tap input file <path>" to its own stdout/stderr, which
    # this script does not capture, and then just sits at the BASIC prompt --
    # every check then fails with a blank screen, with no obvious cause).
    # Resolve to absolute paths up front so callers (and `make smoke`, which
    # runs from the repo root) cannot trip over this by passing a relative
    # --tap.
    args.tap = os.path.abspath(args.tap)

    geom = GEOMETRIES[args.geom]
    cols = args.cols or geom["cols"]
    map_path = os.path.abspath(args.map or (os.path.splitext(args.tap)[0] + ".map"))
    banner_contains = args.banner_contains or (
        "Timex (SCLD)" if args.geom == "hires" else "ZX Spectrum"
    )

    default_wait = {"normal": 8.0, "guard": 9.0, "bypass": 14.0, "scroll": 8.0}[args.scenario]
    wait = args.wait if args.wait is not None else float(os.environ.get("SMOKE_WAIT", default_wait))

    print(
        "=== scenario=%s tap=%s machine=%s geom=%s cols=%d wait=%.1fs ==="
        % (args.scenario, args.tap, args.machine, args.geom, cols, wait)
    )

    spool = None
    keylength = None
    fastautoload = True
    if args.scenario == "bypass":
        fastautoload = False  # --fastautoload's own auto-typing hijacks the spool player
        spool = make_caps_shift_spool()
        keylength = 100

    screenshot = os.path.join(tempfile.gettempdir(), "smoke.pbm")
    if os.path.exists(screenshot):
        os.remove(screenshot)

    proc = launch(
        args.zx,
        args.tap,
        args.machine,
        args.port,
        fastautoload=fastautoload,
        spool=spool,
        keylength=keylength,
    )

    checks = []
    try:
        s = connect(args.port, wait)
        try:
            if args.scenario == "normal":
                scenario_normal(s, args.geom, cols, banner_contains, checks)
            elif args.scenario == "guard":
                scenario_guard(s, checks, map_path)
            elif args.scenario == "bypass":
                scenario_bypass(s, args.geom, cols, banner_contains, checks, map_path)
            elif args.scenario == "scroll":
                scenario_scroll(s, args.geom, cols, checks, map_path, args.scroll_lines)
            print("save-screen:", cmd(s, "save-screen %s" % screenshot).strip()[:200])
        finally:
            s.close()
    finally:
        proc.terminate()
        try:
            proc.wait(timeout=3)
        except Exception:
            proc.kill()

    ok = all(c[1] for c in checks)
    print()
    print(
        "VERDICT: %s -- scenario=%s (%d/%d checks passed)"
        % ("PASS" if ok else "FAIL", args.scenario, sum(1 for c in checks if c[1]), len(checks))
    )
    return 0 if ok else 1


if __name__ == "__main__":
    raise SystemExit(main())
