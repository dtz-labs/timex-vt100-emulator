import socket
import subprocess
import time
import re
import os

ROOT = os.path.abspath(os.path.join(os.path.dirname(__file__), ".."))
ZX = "/Applications/ZEsarUX.app/Contents/MacOS/zesarux"
TAP = os.environ.get("SMOKE_TAP", os.path.join(ROOT, "build", "term.tap"))
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
