#!/usr/bin/env python3
"""webcalls_jitsi.py — a real call through a public Jitsi Meet server (docs/WEBAPPS.md, "Calls"), by hand: it reaches a
service on the Internet, so it is never part of `make test`.

    python3 -I tests/webcalls_jitsi.py chrome  <room-url> <seconds> <report.json>
    python3 -I tests/webcalls_jitsi.py summary <report.json>

`chrome` joins the room with headless Chrome (the machine's own google-chrome, a throwaway profile, Chrome's fake camera
and microphone, no prompt) and, after <seconds>, writes what each of its RTCPeerConnections received and sent (codecs,
packets, frames, video size, fps, the ICE candidate kinds in use; no addresses) to <report.json>. The other side is
lord-web's engine: `build/webcalls-check <room-url> <seconds>` with WEBCALLS_MOCK_CAPTURE=1 (WebKit's mock camera and
microphone), started at the same time, whose `webcall:` lines say what it sent and received.

The room URL carries Jitsi's own URL options so neither side waits on a screen: no prejoin page, a display name, camera
and microphone on, and `config.p2p.enabled=false` to go through the server's video bridge (the Internet path), or true
for a direct path between the two."""
import base64
import json
import os
import shutil
import socket
import struct
import subprocess
import sys
import tempfile
import time
import urllib.request

# kept per page from its first script on: every RTCPeerConnection, so their stats can be read at the end
HOOK = """(() => {
  const PC = window.RTCPeerConnection; if (!PC) return;
  window.__lordosPcs = [];
  window.RTCPeerConnection = new Proxy(PC, { construct(t, a, n) { const pc = Reflect.construct(t, a, n); window.__lordosPcs.push(pc); return pc; } });
})();"""

STATS = r"""(async () => {
  const out = [];
  for (const pc of (window.__lordosPcs || [])) {
    const r = { state: pc.connectionState, ice: pc.iceConnectionState, in: {}, out: {}, pair: null };
    const s = await pc.getStats(), codecs = {}, cands = {};
    s.forEach(x => { if (x.type === 'codec') codecs[x.id] = x.mimeType; if (x.type.endsWith('candidate')) cands[x.id] = x.candidateType + '/' + x.protocol; });
    s.forEach(x => {
      if (x.type === 'inbound-rtp' || x.type === 'outbound-rtp') {
        const d = x.type === 'inbound-rtp' ? r.in : r.out, k = x.kind;
        const e = d[k] || (d[k] = { codec: codecs[x.codecId], packets: 0, frames: 0, width: 0, height: 0, fps: 0, streams: 0 });
        e.streams++; e.packets += x.packetsReceived || x.packetsSent || 0;
        e.frames += x.framesDecoded || x.framesEncoded || 0;
        e.width = Math.max(e.width, x.frameWidth || 0); e.height = Math.max(e.height, x.frameHeight || 0);
        e.fps = Math.max(e.fps, Math.round(x.framesPerSecond || 0)); if (!e.codec) e.codec = codecs[x.codecId];
      }
      if (x.type === 'candidate-pair' && x.state === 'succeeded' && (x.nominated || x.selected))
        r.pair = (cands[x.localCandidateId] || '?') + ' -> ' + (cands[x.remoteCandidateId] || '?');
    });
    out.push(r);
  }
  return JSON.stringify({ title: document.title, text: (document.body ? document.body.innerText : '').replace(/\s+/g, ' ').slice(0, 300), pcs: out });
})()"""


class Cdp:
    """the smallest DevTools Protocol client: one WebSocket, text frames"""

    def __init__(self, ws_url):
        host, rest = ws_url[len("ws://"):].split("/", 1)
        h, p = host.split(":")
        self.s = socket.create_connection((h, int(p)), timeout=60)
        key = base64.b64encode(os.urandom(16)).decode()
        self.s.sendall(("GET /%s HTTP/1.1\r\nHost: %s\r\nUpgrade: websocket\r\nConnection: Upgrade\r\n"
                        "Sec-WebSocket-Key: %s\r\nSec-WebSocket-Version: 13\r\n\r\n" % (rest, host, key)).encode())
        head = b""
        while b"\r\n\r\n" not in head:
            head += self.s.recv(1)
        if b" 101 " not in head.split(b"\r\n")[0]:
            raise RuntimeError("no WebSocket: %r" % head[:80])
        self.n = 0

    def _send(self, text):
        data = text.encode()
        mask = os.urandom(4)
        hdr = bytes([0x81])
        n = len(data)
        hdr += bytes([0x80 | n]) if n < 126 else bytes([0x80 | 126]) + struct.pack(">H", n) if n < 65536 else bytes([0x80 | 127]) + struct.pack(">Q", n)
        self.s.sendall(hdr + mask + bytes(b ^ mask[i % 4] for i, b in enumerate(data)))

    def _exact(self, n):
        b = b""
        while len(b) < n:
            c = self.s.recv(n - len(b))
            if not c:
                raise RuntimeError("DevTools closed")
            b += c
        return b

    def _recv(self):
        msg = b""
        while True:
            a, b = self._exact(2)
            n = b & 0x7F
            if n == 126:
                n = struct.unpack(">H", self._exact(2))[0]
            elif n == 127:
                n = struct.unpack(">Q", self._exact(8))[0]
            msg += self._exact(n)
            if a & 0x80:
                return msg.decode(errors="replace")

    def call(self, method, **params):
        self.n += 1
        self._send(json.dumps({"id": self.n, "method": method, "params": params}))
        while True:
            m = json.loads(self._recv())
            if m.get("id") == self.n:
                if "error" in m:
                    raise RuntimeError("%s: %s" % (method, m["error"]))
                return m.get("result", {})


def chrome(url, secs, report):
    prof = tempfile.mkdtemp(prefix="webcalls-chrome-")
    port = 9333
    p = subprocess.Popen(["google-chrome", "--headless=new", "--user-data-dir=" + prof, "--no-first-run",
                          "--use-fake-device-for-media-stream", "--use-fake-ui-for-media-stream",
                          "--autoplay-policy=no-user-gesture-required", "--remote-debugging-port=%d" % port, "about:blank"],
                         stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)
    try:
        for _ in range(100):
            try:
                tabs = json.load(urllib.request.urlopen("http://127.0.0.1:%d/json" % port, timeout=2))
                page = next(t for t in tabs if t.get("type") == "page")
                break
            except Exception:
                time.sleep(0.2)
        else:
            raise RuntimeError("Chrome's DevTools didn't answer")
        c = Cdp(page["webSocketDebuggerUrl"])
        c.call("Page.enable")
        c.call("Page.addScriptToEvaluateOnNewDocument", source=HOOK)
        c.call("Page.navigate", url=url)
        t0 = time.time()
        samples = []
        while time.time() - t0 < secs:
            time.sleep(10)
            r = c.call("Runtime.evaluate", expression=STATS, awaitPromise=True, returnByValue=True)
            v = json.loads(r.get("result", {}).get("value") or "{}")
            v["t"] = round(time.time() - t0)
            samples.append(v)
            print("chrome t=%ds: %s %s" % (v["t"], json.dumps(v.get("pcs")), "" if v.get("pcs") else repr(v.get("text", ""))[:300]), flush=True)
        json.dump(samples, open(report, "w"), indent=1)
    finally:
        p.terminate()
        try:
            p.wait(10)
        except subprocess.TimeoutExpired:
            p.kill()
        shutil.rmtree(prof, ignore_errors=True)


if __name__ == "__main__":
    if len(sys.argv) >= 5 and sys.argv[1] == "chrome":
        chrome(sys.argv[2], int(sys.argv[3]), sys.argv[4])
    elif len(sys.argv) >= 3 and sys.argv[1] == "summary":
        for s in json.load(open(sys.argv[2])):
            print(s["t"], json.dumps(s.get("pcs")))
    else:
        sys.exit(__doc__)
