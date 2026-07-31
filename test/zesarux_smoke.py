import socket
import subprocess
import time
import re
import os

ROOT = os.path.abspath(os.path.join(os.path.dirname(__file__), ".."))
ZX = "/Applications/ZEsarUX.app/Contents/MacOS/zesarux"
TAP = os.environ.get("SMOKE_TAP", os.path.join(ROOT, "build", "term.tap"))
MAP = os.environ.get("SMOKE_MAP", os.path.join(ROOT, "build", "term.map"))
PORT = int(os.environ.get("ZRCP_PORT", "10001"))
SCREENSHOT = "/tmp/smoke.pbm"
if os.path.exists(SCREENSHOT):
    os.remove(SCREENSHOT)
verdict_ok = False

proc = subprocess.Popen(
    [
        ZX,
        "--noconfigfile",
        "--machine",
        "TC2048",
        "--tape",
        TAP,
        "--fastautoload",
        "--vo",
        "null",
        "--ao",
        "null",
        "--nosplash",
        "--enable-remoteprotocol",
        "--remoteprotocol-port",
        str(PORT),
        "--quickexit",
    ],
    stdout=subprocess.DEVNULL,
    stderr=subprocess.STDOUT,
)


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
    s.sendall((c + "\n").encode())
    return recv(s)


def hexline(resp):
    for ln in resp.splitlines():
        ln = ln.strip()
        if re.fullmatch(r"[0-9A-Fa-f]+", ln) and len(ln) >= 2:
            return ln
    return ""


def rdbytes(s, addr, n):
    return bytes.fromhex(hexline(cmd(s, "read-memory %d %d" % (addr, n))))


def wrbytes(s, addr, data):
    values = " ".join(str(b) for b in data)
    cmd(s, "write-memory %d %s" % (addr, values))


def map_symbol(name):
    pattern = re.compile(
        r"^_?%s\s*=\s*\$([0-9A-Fa-f]+)\b" % re.escape(name)
    )
    with open(MAP, "r", encoding="latin-1") as f:
        for line in f:
            match = pattern.match(line)
            if match:
                return int(match.group(1), 16)
    raise RuntimeError("symbol not found in map: %s" % name)


def inject(s, data, timeout=8.0):
    len_addr = map_symbol("conn_zrcp_inject_len")
    data_addr = map_symbol("conn_zrcp_inject_data")
    deadline = time.monotonic() + timeout

    while rdbytes(s, len_addr, 1)[0] != 0:
        if time.monotonic() > deadline:
            raise RuntimeError("timeout waiting for input mailbox")
        time.sleep(0.01)
    wrbytes(s, data_addr, data)
    wrbytes(s, len_addr, bytes((len(data),)))
    while rdbytes(s, len_addr, 1)[0] != 0:
        if time.monotonic() > deadline:
            raise RuntimeError("timeout waiting for injected input")
        time.sleep(0.01)


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


def wait_cell(s, row, col, want, timeout=8.0):
    deadline = time.monotonic() + timeout
    got = b""
    while time.monotonic() <= deadline:
        got = cell_bytes(s, row, col)
        if got == want:
            return got
        time.sleep(0.02)
    return got


try:
    time.sleep(float(os.environ.get("SMOKE_WAIT", "8")))
    s = socket.create_connection(("127.0.0.1", PORT), timeout=5)
    recv(s)
    print("help save-screen:", cmd(s, "help save-screen").strip()[:300])
    print()
    ok = True
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
    for name, row, col, want in checks:
        scr = cell_bytes(s, row, col)
        match = scr == want
        ok = ok and match
        print(
            "cell(row=%d,col=%d,%s) WANT=%s  SCREEN=%s  %s"
            % (row, col, name, want.hex(), scr.hex(), "MATCH" if match else "MISMATCH")
        )

    # Exercise the single-cell repaint path at a byte-straddling column. Hide
    # the cursor so the neighbouring-cell preservation checks read raw glyphs.
    a_glyph = bytes([0x00, 0x70, 0x88, 0x88, 0xF8, 0x88, 0x88, 0x00])
    blank = bytes(8)
    inject(s, b"\x1b[?25l\x1b[17;11HA")
    got = wait_cell(s, 16, 10, a_glyph)
    left = cell_bytes(s, 16, 9)
    right = cell_bytes(s, 16, 11)
    partial_match = got == a_glyph and left == blank and right == blank
    ok = ok and partial_match
    print(
        "partial-cell repaint WANT=%s SCREEN=%s neighbors=%s/%s  %s"
        % (
            a_glyph.hex(),
            got.hex(),
            left.hex(),
            right.hex(),
            "MATCH" if partial_match else "MISMATCH",
        )
    )

    # Exercise a full upward scroll, including both discontinuities between
    # the three ZX display-memory thirds (rows 8->7 and 16->15).
    i_glyph = bytes([0x00, 0x70, 0x20, 0x20, 0x20, 0x20, 0x70, 0x00])
    inject(s, b"\x1b[2J\x1b[9;1HI")
    before_scroll = wait_cell(s, 8, 0, i_glyph)
    before_match = before_scroll == i_glyph
    ok = ok and before_match
    print(
        "scroll setup row=8 WANT=%s SCREEN=%s  %s"
        % (
            i_glyph.hex(),
            before_scroll.hex(),
            "MATCH" if before_match else "MISMATCH",
        )
    )
    inject(s, b"\x1b[17;1HQ\x1b[24;1HX\r\n")
    scroll_checks = (
        ("third 1->0", 7, 8, i_glyph),
        (
            "third 2->1",
            15,
            16,
            bytes([0x00, 0x70, 0x88, 0x88, 0xA8, 0x98, 0x70, 0x00]),
        ),
        (
            "last row up",
            22,
            23,
            bytes([0x00, 0x88, 0x50, 0x20, 0x20, 0x50, 0x88, 0x00]),
        ),
    )
    for name, row, source_row, expected in scroll_checks:
        # Font shapes are explicit so the check is independent of stale pixels
        # at either the source or destination address.
        got = wait_cell(s, row, 0, expected)
        match = got == expected
        ok = ok and match
        print(
            "scroll(row=%d,%s,from=%d) WANT=%s SCREEN=%s  %s"
            % (
                row,
                name,
                source_row,
                expected.hex(),
                got.hex(),
                "MATCH" if match else "MISMATCH",
            )
        )
    bottom = cell_bytes(s, 23, 0)
    bottom_match = bottom == blank
    ok = ok and bottom_match
    print(
        "scroll blank bottom WANT=%s SCREEN=%s  %s"
        % (blank.hex(), bottom.hex(), "MATCH" if bottom_match else "MISMATCH")
    )

    # And the reverse direction: RI at the top margin scrolls down. Again put
    # glyphs on both display-third boundaries before moving them.
    p_glyph = bytes([0x00, 0xF0, 0x88, 0x88, 0xF0, 0x80, 0x80, 0x00])
    inject(s, b"\x1b[2J\x1b[8;1HI\x1b[16;1HP\x1b[1;1HA")
    setup = wait_cell(s, 15, 0, p_glyph)
    setup_match = setup == p_glyph
    ok = ok and setup_match
    print(
        "reverse-scroll setup row=15 WANT=%s SCREEN=%s  %s"
        % (p_glyph.hex(), setup.hex(), "MATCH" if setup_match else "MISMATCH")
    )
    inject(s, b"\x1b[H\x1bM")
    reverse_checks = (
        ("first row down", 1, a_glyph),
        ("third 0->1", 8, i_glyph),
        ("third 1->2", 16, p_glyph),
    )
    for name, row, expected in reverse_checks:
        got = wait_cell(s, row, 0, expected)
        match = got == expected
        ok = ok and match
        print(
            "reverse-scroll(row=%d,%s) WANT=%s SCREEN=%s  %s"
            % (
                row,
                name,
                expected.hex(),
                got.hex(),
                "MATCH" if match else "MISMATCH",
            )
        )
    top = cell_bytes(s, 0, 0)
    top_match = top == blank
    ok = ok and top_match
    print(
        "reverse-scroll blank top WANT=%s SCREEN=%s  %s"
        % (blank.hex(), top.hex(), "MATCH" if top_match else "MISMATCH")
    )
    print()
    print("VERDICT:", "PASS - startup help rendered on TC2048" if ok else "FAIL")
    verdict_ok = ok
    # screenshot artifact
    print("save-screen:", cmd(s, "save-screen %s" % SCREENSHOT).strip()[:200])
    s.close()
finally:
    proc.terminate()
    try:
        proc.wait(timeout=3)
    except Exception:
        proc.kill()

time.sleep(0.5)
print(
    "screenshot exists:",
    os.path.exists(SCREENSHOT),
    os.path.getsize(SCREENSHOT) if os.path.exists(SCREENSHOT) else "",
)
if not verdict_ok:
    raise SystemExit(1)
