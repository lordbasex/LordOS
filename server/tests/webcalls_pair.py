#!/usr/bin/env python3
"""webcalls_pair.py — a call between two browsers through a signaling server of its own, on this machine (docs/WEBAPPS.md,
"Calls"), by hand: lord-web's engine (build/webcalls-check, WEBCALLS_MOCK_CAPTURE=1) on one side, headless Chrome with
its fake camera and microphone (tests/webcalls_jitsi.py's DevTools client) on the other, real UDP between two processes
and two network stacks' ICE, no third-party service.

    python3 -I tests/webcalls_pair.py serve <port> <report-dir>      the signaling server and the page
    python3 -I tests/webcalls_pair.py chrome <url> <seconds>          Chrome's side (role in the URL: ?role=callee)

URL options: bundle=<policy> (bundlePolicy), turn=<turn: URL> (a TURN server with the fixed test credentials pair /
pairpass, e.g. a local coturn), relay=1 (iceTransportPolicy relay: media only through TURN).

The page (role=caller or callee) gets the camera and microphone, the caller offers, both trickle candidates through the
server, and after 15 s each side posts what it sent and received (codecs, packets, frames, size; the kinds of the
candidate pair, no addresses) to <report-dir>/<role>.json and sets its title to "done"."""
import http.server
import json
import os
import socketserver
import sys
import threading
import time

PAGE = r"""<!doctype html><title>pair</title><script>
const role = new URLSearchParams(location.search).get('role');
const other = role === 'caller' ? 'callee' : 'caller';
const post = (path, body) => fetch(path, { method: 'POST', body: JSON.stringify(body) });
const get = async path => { for (;;) { const r = await fetch(path); if (r.status === 200) return r.json(); await new Promise(f => setTimeout(f, 200)); } };
(async () => {
  const s = await navigator.mediaDevices.getUserMedia({ audio: true, video: { width: 640, height: 480 } });
  const q = new URLSearchParams(location.search), bundle = q.get('bundle'), turn = q.get('turn');
  // turn=<turn: URL>: a TURN server with the test's fixed credentials (a local coturn); relay=1: media only through it
  const conf = {};
  if (bundle) conf.bundlePolicy = bundle;
  if (turn) conf.iceServers = [{ urls: turn, username: 'pair', credential: 'pairpass' }];
  if (q.get('relay')) conf.iceTransportPolicy = 'relay';
  const pc = new RTCPeerConnection(conf);
  s.getTracks().forEach(t => pc.addTrack(t, s));
  pc.onicecandidate = e => { if (e.candidate) post('/cand/' + role, e.candidate.toJSON()); };
  let seen = 0;
  // the other side's candidates only once its description is set (before, addIceCandidate rejects them)
  (async () => { for (;;) { if (!pc.remoteDescription) { await new Promise(f => setTimeout(f, 100)); continue; } const r = await fetch('/cands/' + other + '?from=' + seen); const l = await r.json(); for (const c of l) { seen++; try { await pc.addIceCandidate(c); } catch (e) {} } await new Promise(f => setTimeout(f, 200)); } })();
  if (role === 'caller') {
    await pc.setLocalDescription(await pc.createOffer());
    await post('/sdp/offer', pc.localDescription.toJSON());
    await pc.setRemoteDescription(await get('/sdp/answer'));
  } else {
    await pc.setRemoteDescription(await get('/sdp/offer'));
    await pc.setLocalDescription(await pc.createAnswer());
    await post('/sdp/answer', pc.localDescription.toJSON());
  }
  const t0 = performance.now();
  while (pc.connectionState !== 'connected' && performance.now() - t0 < 20000) await new Promise(f => setTimeout(f, 100));
  const r = { role, state: pc.connectionState, ice: pc.iceConnectionState, connectMs: Math.round(performance.now() - t0), in: {}, out: {} };
  await new Promise(f => setTimeout(f, 15000));
  const st = await pc.getStats(), codecs = {}, cands = {};
  st.forEach(x => { if (x.type === 'codec') codecs[x.id] = x.mimeType; if (x.type.endsWith('candidate')) cands[x.id] = x.candidateType + '/' + x.protocol; });
  st.forEach(x => {
    if (x.type === 'inbound-rtp' || x.type === 'outbound-rtp') {
      const d = x.type === 'inbound-rtp' ? r.in : r.out;
      d[x.kind] = { codec: codecs[x.codecId], packets: x.packetsReceived || x.packetsSent || 0, frames: x.framesDecoded || x.framesEncoded || 0,
                    width: x.frameWidth || 0, height: x.frameHeight || 0, bytes: x.bytesReceived || x.bytesSent || 0 };
    }
    if (x.type === 'transport' && x.dtlsState) r.dtls = x.dtlsState + (x.srtpCipher ? ' ' + x.srtpCipher : '');
    if (x.type === 'candidate-pair' && x.state === 'succeeded' && (x.nominated || x.selected)) r.pair = (cands[x.localCandidateId] || '?') + ' -> ' + (cands[x.remoteCandidateId] || '?');
  });
  await post('/report/' + role, r);
  document.title = 'done ' + pc.connectionState;
})().catch(e => { post('/report/' + role, { role, error: e.name + ': ' + e.message }); document.title = 'done error'; });
</script>"""


def serve(port, outdir):
    state = {"sdp": {}, "cands": {"caller": [], "callee": []}}
    lock = threading.Lock()

    class H(http.server.BaseHTTPRequestHandler):
        def log_message(self, *a):
            pass

        def reply(self, code, body=b"", ctype="application/json"):
            self.send_response(code)
            self.send_header("Content-Type", ctype)
            self.send_header("Cache-Control", "no-store")
            self.end_headers()
            self.wfile.write(body)

        def do_GET(self):
            path, _, q = self.path.partition("?")
            with lock:
                if path == "/":
                    return self.reply(200, PAGE.encode(), "text/html")
                if path.startswith("/sdp/"):
                    v = state["sdp"].get(path[5:])
                    return self.reply(200, json.dumps(v).encode()) if v else self.reply(204)
                if path.startswith("/cands/"):
                    n = int(q.split("=")[1]) if q.startswith("from=") else 0
                    return self.reply(200, json.dumps(state["cands"].get(path[7:], [])[n:]).encode())
            self.reply(404)

        def do_POST(self):
            body = json.loads(self.rfile.read(int(self.headers.get("Content-Length", "0"))) or b"null")
            with lock:
                if self.path.startswith("/sdp/"):
                    state["sdp"][self.path[5:]] = body
                    # kept for a look when something fails (a local test between two local browsers: nothing private)
                    open(os.path.join(outdir, self.path[5:] + ".sdp"), "w").write(body.get("sdp", ""))
                elif self.path.startswith("/cand/"):
                    state["cands"].setdefault(self.path[6:], []).append(body)
                elif self.path.startswith("/report/"):
                    json.dump(body, open(os.path.join(outdir, self.path[8:] + ".json"), "w"))
            self.reply(204)

    class S(socketserver.ThreadingMixIn, http.server.HTTPServer):
        daemon_threads = True

    S(("127.0.0.1", port), H).serve_forever()


if __name__ == "__main__":
    if len(sys.argv) >= 4 and sys.argv[1] == "serve":
        serve(int(sys.argv[2]), sys.argv[3])
    elif len(sys.argv) >= 4 and sys.argv[1] == "chrome":
        import importlib.util
        spec = importlib.util.spec_from_file_location("wj", os.path.join(os.path.dirname(os.path.abspath(__file__)), "webcalls_jitsi.py"))
        wj = importlib.util.module_from_spec(spec)
        spec.loader.exec_module(wj)
        wj.chrome(sys.argv[2], int(sys.argv[3]), "/dev/null")
    else:
        sys.exit(__doc__)
