# ui.py SOCK cmd... (run as root): drives a headless session over its remote.sock, as the remote desktop does
# (server/src/remote/proto.h), streaming frames in the background so several grabs work in one connection.
# cmds: click:x,y dbl:x,y rclick:x,y move:x,y wheel:x,y,steps combo:a+b key:code type:text wait:s grab:out.png
#       keyring:PASSWORD   (RP_KEYRING, as lord-sessiond after a login: opens or creates the session's keyring)
# From ~/cloud-drv.py on VM 116 (the Cloud UI agent's), plus keyring and frames only when a grab is asked.
import socket, struct, sys, time, threading, zlib
s = socket.socket(socket.AF_UNIX, socket.SOCK_STREAM); s.connect(sys.argv[1])
lock = threading.Lock()
def send(t, d=b""):
    with lock: s.sendall(struct.pack("<IB", 1 + len(d), t) + d)
KEYS = {c: k for c, k in zip("qwertyuiop", range(16, 26))}
KEYS.update({c: k for c, k in zip("asdfghjkl", range(30, 39))}); KEYS.update({c: k for c, k in zip("zxcvbnm", range(44, 51))})
KEYS.update({c: k for c, k in zip("1234567890", range(2, 12))}); KEYS.update({" ": 57, "-": 12, ".": 52, "/": 53, ":": 39, "_": 12})
SHIFTED = {":": 39, "_": 12}
FB = {"w": 0, "h": 0, "px": None}
def recvn(n):
    b = b""
    while len(b) < n:
        c = s.recv(n - len(b))
        if not c: raise OSError("closed")
        b += c
    return b
def drain():
    try:
        while True:
            n, = struct.unpack("<I", recvn(4)); body = recvn(n); t = body[0]; p = body[1:]
            if t == 1: W, H = struct.unpack("<HH", p[:4]); FB.update(w=W, h=H, px=bytearray(W * H * 4))
            elif t == 2 and FB["px"] is not None:
                x, y, w, h = struct.unpack("<HHHH", p[:8]); d = p[8:]; W = FB["w"]
                for j in range(h):
                    o = ((y + j) * W + x) * 4; FB["px"][o:o + w * 4] = d[j * w * 4:(j + 1) * w * 4]
            elif t == 3: send(10)
    except OSError: pass
cmds = sys.argv[2:]
video = any(not a.startswith("keyring:") for a in cmds)
if video:
    threading.Thread(target=drain, daemon=True).start()
    send(32, struct.pack("<BBHI", 1, 0, 0, 8000)); send(35, b"\x01"); send(10)
def key(c, v): send(14, struct.pack("<HB", c, v)); time.sleep(0.04)
def save(out):
    send(10); time.sleep(0.8)
    W, H, px = FB["w"], FB["h"], bytes(FB["px"] or b"")
    if not px: print(out, "no frame"); return
    raw = bytearray()
    for r in range(H):
        raw.append(0); row = px[r * W * 4:(r + 1) * W * 4]
        for i in range(0, len(row), 4): raw += bytes((row[i + 2], row[i + 1], row[i]))
    ch = lambda t, d: struct.pack(">I", len(d)) + t + d + struct.pack(">I", zlib.crc32(t + d) & 0xffffffff)
    open(out, "wb").write(b"\x89PNG\r\n\x1a\n" + ch(b"IHDR", struct.pack(">IIBBBBB", W, H, 8, 2, 0, 0, 0)) + ch(b"IDAT", zlib.compress(bytes(raw), 6)) + ch(b"IEND", b""))
    print(out, W, H, flush=True)
for a in cmds:
    c, _, v = a.partition(":")
    if c in ("click", "dbl"):
        x, y = (int(z) for z in v.split(",")); send(11, struct.pack("<hh", x, y)); time.sleep(0.15)
        for _ in range(2 if c == "dbl" else 1): send(12, struct.pack("<BB", 0, 1)); time.sleep(0.06); send(12, struct.pack("<BB", 0, 0)); time.sleep(0.06)
    elif c == "rclick":
        x, y = (int(z) for z in v.split(",")); send(11, struct.pack("<hh", x, y)); time.sleep(0.15)
        send(12, struct.pack("<BB", 2, 1)); time.sleep(0.06); send(12, struct.pack("<BB", 2, 0)); time.sleep(0.06)
    elif c == "move": x, y = (int(z) for z in v.split(",")); send(11, struct.pack("<hh", x, y)); time.sleep(0.05)
    elif c == "wheel":
        x, y, st = (int(z) for z in v.split(",")); send(11, struct.pack("<hh", x, y)); time.sleep(0.1)
        for _ in range(abs(st)): send(13, struct.pack("<b", -1 if st < 0 else 1)); time.sleep(0.05)
    elif c == "combo":
        ks = [int(k) for k in v.split("+")]
        for k in ks: key(k, 1)
        for k in reversed(ks): key(k, 0)
    elif c == "key": key(int(v), 1); key(int(v), 0)
    elif c == "type":
        for chh in v:
            if chh.isupper() or chh in SHIFTED:
                k = SHIFTED.get(chh) or KEYS[chh.lower()]; key(42, 1); key(k, 1); key(k, 0); key(42, 0)
            else: key(KEYS[chh], 1); key(KEYS[chh], 0)
    elif c == "wait":
        end = time.time() + float(v)
        while time.time() < end: send(10 if video else 10); time.sleep(0.2)
    elif c == "grab": save(v)
    elif c == "keyring": send(24, v.encode()); time.sleep(0.3)
    time.sleep(0.1)
if video: send(32, struct.pack("<BBHI", 0, 0, 0, 0))
