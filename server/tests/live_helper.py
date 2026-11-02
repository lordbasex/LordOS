#!/usr/bin/env python3
"""live_helper.py <lord-media> <h264 annex-b file with AUDs> <vp8 ivf file>: lord-media's MK_LIVE by itself, with no session.

Real decoding in the real sandbox: pictures reach the frame slots and each ring is what it says; a damaged stream asks for a
keyframe; lengths out of bounds, an unknown codec, and junk end the stream or are ignored, and never crash the helper."""
import array, mmap, os, random, select, signal, socket, struct, subprocess, sys, time

MM_JOB, MM_SIZE, MM_STOP, MM_ACK = 1, 2, 8, 9
MK_LIVE = 9
MM_INFO, MM_RING, MM_FRAME, MM_STATE, MM_ERROR = 64, 65, 66, 68, 74
LL_CODEC, LL_VIDEO, LL_OPUS, LL_WANTKEY = 1, 2, 3, 64
HDR = struct.Struct('=II6qII')
fails = []

def check(name, ok, extra=''):
    print(('ok   ' if ok else 'FAIL ') + name + (' ' + str(extra) if extra else ''))
    if not ok: fails.append(name)

class Helper:
    def __init__(self, exe):
        self.ctl, child = socket.socketpair(socket.AF_UNIX, socket.SOCK_SEQPACKET)
        self.link, far = socket.socketpair(socket.AF_UNIX, socket.SOCK_STREAM)
        fd = child.fileno()
        def pre():
            os.dup2(fd, 3)
        self.p = subprocess.Popen([exe], close_fds=False, preexec_fn=pre, env={'LANG': 'C.UTF-8'}, stderr=open('live_helper.log', 'ab'))
        child.close()
        msg = HDR.pack(MM_JOB, 0, MK_LIVE, 0, 0, 0, 0, 0, 0, 0)
        socket.send_fds(self.ctl, [msg], [far.fileno()])
        far.close()
        self.ctl.setblocking(False)
        self.ring = None
        self.frames = 0
        self.info = None
        self.link.setblocking(False)
        self.back = b''

    def send(self, ty, data=b'', pts=0, flags=0, length=None):
        b = struct.pack('<IBBHq', len(data) if length is None else length, ty, flags, 0, pts) + data
        end = time.time() + 5
        while b and time.time() < end:
            try:
                n = self.link.send(b); b = b[n:]
            except BlockingIOError:
                self.pump(0.01)
            except OSError:
                return False
        return not b

    def pump(self, t):
        r, _, _ = select.select([self.ctl], [], [], t)
        if not r: return
        while True:
            try:
                d, fds, _, _ = socket.recv_fds(self.ctl, 66000, 4)
            except BlockingIOError:
                return
            except OSError:
                return
            if not d: return
            ty, mid, *v = HDR.unpack(d[:HDR.size])[:8]
            if ty == MM_RING and fds:
                self.ring = (v[1], v[2], fds[0])
            elif ty == MM_FRAME:
                self.frames += 1
                self.last = mid
                self.ctl.send(HDR.pack(MM_ACK, mid, 0, 0, 0, 0, 0, 0, 0, 0))
            elif ty == MM_INFO:
                self.info = d[HDR.size:].decode()
            for f in fds:
                if not (self.ring and f == self.ring[2]): os.close(f)

    def wants_key(self):
        try:
            self.back += self.link.recv(4096)
        except (BlockingIOError, OSError):
            pass
        return any(self.back[i + 4] == LL_WANTKEY for i in range(0, len(self.back) - 15, 16))

    def alive(self):
        return self.p.poll() is None

    def stop(self):
        try: self.ctl.send(HDR.pack(MM_STOP, 0, 0, 0, 0, 0, 0, 0, 0, 0))
        except OSError: pass
        try: self.p.wait(3)
        except subprocess.TimeoutExpired: self.p.kill(); self.p.wait(); return None
        return self.p.returncode

def h264_units(path):
    d = open(path, 'rb').read()
    cuts = []
    i = 0
    while True:
        i = d.find(b'\0\0\1\x09', i)
        if i < 0: break
        cuts.append(i - 1 if i and d[i - 1] == 0 else i)
        i += 4
    return [d[a:b] for a, b in zip(cuts, cuts[1:] + [len(d)])]

def ivf_units(path):
    d = open(path, 'rb').read()
    o = struct.unpack('<H', d[6:8])[0]
    out = []
    while o + 12 <= len(d):
        n = struct.unpack('<I', d[o:o + 4])[0]
        out.append(d[o + 12:o + 12 + n])
        o += 12 + n
    return out

def stream(h, codec, units, name):
    h.send(LL_CODEC, codec)
    for i, u in enumerate(units):
        h.send(LL_VIDEO, u, pts=i * 33, flags=1 if i == 0 else 0)
        h.pump(0.002)
    end = time.time() + 3
    while time.time() < end and h.frames < len(units) // 2:
        h.pump(0.05)
    check(name + ': pictures reach the slots', h.frames >= len(units) // 2, f'{h.frames}/{len(units)}')
    check(name + ': a ring came', h.ring is not None)
    if h.ring:
        w, hg, fd = h.ring
        m = mmap.mmap(fd, w * hg * 4 * 3, prot=mmap.PROT_READ)
        slot = (h.last & 0xff) * w * hg * 4
        px = m[slot:slot + w * hg * 4]
        check(name + ': the slot holds a picture', len(set(px[::1021])) > 8, f'{w}x{hg}')
        m.close()
    check(name + ': its size was told', h.info is not None and '"width":320' in h.info, h.info)

exe, h264, ivf = sys.argv[1:4]
a = h264_units(h264)
v = ivf_units(ivf)
print(len(a), 'H.264 units,', len(v), 'VP8 units')

h = Helper(exe)
stream(h, b'h264', a, 'H.264')
rc = h.stop()
check('H.264: ends cleanly', rc == 0, rc)

h = Helper(exe)
stream(h, b'vp8', v, 'VP8')
check('VP8: ends cleanly', h.stop() == 0)

h = Helper(exe)
h.send(LL_CODEC, b'h264')
for u in a[5:15]:                       # no keyframe yet, and damaged
    b = bytearray(u)
    for k in range(30, len(b), 50): b[k] ^= 0xff
    h.send(LL_VIDEO, bytes(b))
    h.pump(0.002)
end = time.time() + 2
while time.time() < end and not h.wants_key(): h.pump(0.05)
check('a lost picture asks for a keyframe', h.wants_key())
check('and the helper lives', h.alive())
h.stop()

h = Helper(exe)
h.send(LL_CODEC, b'mp4v-evil')
h.send(LL_CODEC, b'nope')
h.send(LL_VIDEO, a[0])
h.pump(0.2)
check('an unknown codec decodes nothing', h.frames == 0 and h.alive())
h.send(99, b'x' * 100)
h.pump(0.1)
check('an unknown message is ignored', h.alive())
h.send(LL_CODEC, b'h264')
for u in a[:6]: h.send(LL_VIDEO, u)
end = time.time() + 2
while time.time() < end and not h.frames: h.pump(0.05)
check('and the stream goes on', h.frames > 0)
h.stop()

h = Helper(exe)
h.send(LL_VIDEO, b'', length=(8 << 20) + 1)
h.p.wait(3) if False else None
end = time.time() + 3
while time.time() < end and h.alive(): h.pump(0.05)
check('a length out of bounds ends the helper, with no crash', not h.alive() and h.p.returncode == 0, h.p.returncode)

rnd = random.Random(1)
crashed = 0
for codec, units in ((b'h264', a), (b'vp8', v)):
    for round_ in range(20):
        h = Helper(exe)
        h.send(LL_CODEC, codec)
        for u in units[:12]:
            b = bytearray(u)
            for _ in range(1 + len(b) // 200): b[rnd.randrange(len(b))] = rnd.randrange(256)
            if not h.send(LL_VIDEO, bytes(b)): break
        h.pump(0.05)
        rc = h.stop()
        if rc is None or rc < 0:
            crashed += 1
            print('  crash', codec, round_, rc)
check('40 damaged streams: no crash', crashed == 0, crashed)

print('FAILED: ' + ', '.join(fails) if fails else 'all passed')
sys.exit(1 if fails else 0)
