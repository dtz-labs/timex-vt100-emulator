import socket, subprocess, time, re, os

ZX = "/Applications/ZEsarUX.app/Contents/MacOS/zesarux"
TAP = "/Users/mpasternak/Programowanie/tc-2068-vt100/build/term.tap"
PORT = 10000
PNG = "/tmp/smoke.png"
if os.path.exists(PNG): os.remove(PNG)

proc = subprocess.Popen(
    [ZX, "--machine","TC2048","--tape",TAP,"--fastautoload",
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
def glyph_addrs(col):
    base = 0x6000 if (col&1) else 0x4000
    bx = col>>1
    out=[]
    for prow in range(8):
        off=((prow&0xC0)<<5)|((prow&0x07)<<8)|((prow&0x38)<<2)
        out.append(base+off+bx)
    return out

try:
    time.sleep(8)
    s=socket.create_connection(("127.0.0.1",PORT),timeout=5)
    recv(s)
    print("help save-screen:", cmd(s,"help save-screen").strip()[:300])
    print()
    ok=True
    for col,ch in ((0,ord('T')),(1,ord('C'))):
        rom_addr = 0x3D00 + (ch-0x20)*8
        rom = rdbytes(s, rom_addr, 8)
        scr = bytes(rdbytes(s,a,1)[0] for a in glyph_addrs(col))
        match = (rom==scr)
        ok = ok and match
        print("cell(col=%d,'%c') ROM@%04X=%s  SCREEN=%s  %s" %
              (col,ch,rom_addr,rom.hex(),scr.hex(),"MATCH" if match else "MISMATCH"))
    print()
    print("VERDICT:", "PASS - hires.c address math + glyph blit correct on TC2048" if ok else "FAIL")
    # screenshot artifact
    print("save-screen:", cmd(s,"save-screen %s"%PNG).strip()[:200])
    s.close()
finally:
    proc.terminate()
    try: proc.wait(timeout=3)
    except Exception: proc.kill()

time.sleep(0.5)
print("PNG exists:", os.path.exists(PNG), os.path.getsize(PNG) if os.path.exists(PNG) else "")
