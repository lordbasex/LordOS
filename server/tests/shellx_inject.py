# shellx_inject.py SOCK cmd... (as root): the pointer of a headless test session, through its remote.sock as the remote
# desktop drives it (server/src/remote/proto.h; the e2e bench's tools/vm/e2e/ui.py, pointer only). For
# tests/shellx_headless.sh's tab tear-off. cmds: move:x,y down:x,y up:x,y wait:s
import socket, struct, sys, time
s = socket.socket(socket.AF_UNIX, socket.SOCK_STREAM)
s.connect(sys.argv[1])
def send(t, d=b""):
    s.sendall(struct.pack("<IB", 1 + len(d), t) + d)
for a in sys.argv[2:]:
    c, _, v = a.partition(":")
    if c == "wait":
        time.sleep(float(v))
        continue
    x, y = (int(z) for z in v.split(","))
    send(11, struct.pack("<hh", x, y))
    time.sleep(0.12)
    if c == "down": send(12, struct.pack("<BB", 0, 1)); time.sleep(0.25)
    elif c == "up": send(12, struct.pack("<BB", 0, 0)); time.sleep(0.25)
s.close()
