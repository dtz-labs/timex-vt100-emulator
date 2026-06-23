import socket, subprocess, time, re, os

ZX = "/Applications/ZEsarUX.app/Contents/MacOS/zesarux"
TAP = "/Users/mpasternak/Programowanie/tc-2068-vt100/build/term.tap"
PORT = int(os.environ.get("ZRCP_PORT", "10001"))
SCREENSHOT = "/tmp/smoke.pbm"
if os.path.exists(SCREENSHOT): os.remove(SCREENSHOT)

proc = subprocess.Popen(
    [ZX, "--noconfigfile", "--machine","TC2048","--tape",TAP,"--fastautoload",
     "--vo","null","--ao","null","--nosplash",
     "--enable-remoteprotocol","--remoteprotocol-port",str(PORT),"--quickexit"],
    stdout=subprocess.DEVNULL, stderr=subprocess.STDOUT)

def recv(s,t=2.0):
    s.settimeout(t); buf=b""
    try:
        while True:
            d=s.recv(4096)
            if not d: break
            buf+=d
            if buf.endswith(b"command> "): break
    except socket.timeout: pass
    return buf.decode("latin-1","replace")
def cmd(s,c):
    s.sendall((c+"\n").encode()); return recv(s)
def hexline(resp):
    for ln in resp.splitlines():
        ln=ln.strip()
        if re.fullmatch(r"[0-9A-Fa-f]+", ln) and len(ln)>=2:
            return ln
    return ""
def rdbytes(s,addr,n):
    return bytes.fromhex(hexline(cmd(s,"read-memory %d %d"%(addr,n))))
def glyph_addrs(row, col):
    base = 0x6000 if (col&1) else 0x4000
    bx = col>>1
    out=[]
    for i in range(8):
        prow = row * 8 + i
        off=((prow&0xC0)<<5)|((prow&0x07)<<8)|((prow&0x38)<<2)
        out.append(base+off+bx)
    return out
def cell_bytes(s, row, col):
    return bytes(rdbytes(s,a,1)[0] for a in glyph_addrs(row, col))

try:
    time.sleep(float(os.environ.get("SMOKE_WAIT", "8")))
    s=socket.create_connection(("127.0.0.1",PORT),timeout=5)
    recv(s)
    print("help save-screen:", cmd(s,"help save-screen").strip()[:300])
    print()
    ok=True
    checks = (
        ("upper-left l", 0, 0, bytes([0x00,0x00,0x00,0x1F,0x10,0x10,0x10,0x10])),
        ("horizontal q", 0, 1, bytes([0x00,0x00,0x00,0xFF,0x00,0x00,0x00,0x00])),
        ("upper-right k", 0, 26, bytes([0x00,0x00,0x00,0xF0,0x10,0x10,0x10,0x10])),
        ("scroll top 2", 6, 7, bytes([0x70,0x88,0x08,0x30,0x40,0x80,0xF8,0x00])),
        ("scroll top 3", 6, 8, bytes([0xF0,0x08,0x08,0x70,0x08,0x08,0xF0,0x00])),
        ("scroll bottom 4", 23, 7, bytes([0x10,0x30,0x50,0x90,0xF8,0x10,0x10,0x00])),
        ("scroll bottom 0", 23, 8, bytes([0x70,0x88,0x98,0xA8,0xC8,0x88,0x70,0x00])),
    )
    for name,row,col,want in checks:
        scr = cell_bytes(s,row,col)
        match = (scr==want)
        ok = ok and match
        print("cell(row=%d,col=%d,%s) WANT=%s  SCREEN=%s  %s" %
              (row,col,name,want.hex(),scr.hex(),"MATCH" if match else "MISMATCH"))
    print()
    print("VERDICT:", "PASS - line drawing + scroll rendered on TC2048" if ok else "FAIL")
    # screenshot artifact
    print("save-screen:", cmd(s,"save-screen %s"%SCREENSHOT).strip()[:200])
    s.close()
finally:
    proc.terminate()
    try: proc.wait(timeout=3)
    except Exception: proc.kill()

time.sleep(0.5)
print("screenshot exists:", os.path.exists(SCREENSHOT), os.path.getsize(SCREENSHOT) if os.path.exists(SCREENSHOT) else "")
