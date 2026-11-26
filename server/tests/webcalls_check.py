#!/usr/bin/env python3
"""webcalls_check.py — web apps' calls and Chrome identity on the real engine (docs/WEBAPPS.md, "Calls").

    python3 -I tests/webcalls_check.py build/webcalls-check [--webrtc] [--dip] [--swnotify]

Serves pages on 127.0.0.1 (a secure context, as Chrome and WebKit treat loopback) and loads each in
build/webcalls-check (tests/webcalls_check.c: a WebKit view set up as lord-web sets up a Chrome profile):

  identity   the request headers and the page's navigator values against what Chrome 158 on Linux sent and answered
             (captured 2026-11-25): Sec-CH-UA, -Mobile and -Platform on the first request, the high-entropy hints
             only after the site's Accept-CH, navigator.userAgentData (toJSON, getHighEntropyValues), navigator.vendor;
             the functions read as native code; getUserMedia's failure (a device that can't exist: no real microphone
             is opened) in the call log by its name
  isolation  SharedArrayBuffer: never on a page that isn't cross-origin isolated; with --webrtc (an engine built with
             LordOS's patch) present on one served with COOP same-origin and COEP require-corp, in the same run
  DIP        a page served with exactly a messaging web app's headers (COEP require-corp, COOP same-origin-allow-popups,
             Document-Isolation-Policy isolate-and-require-corp, Origin-Agent-Cluster ?1): with --dip (an engine with
             lordos-document-isolation-policy.patch, +4.1) cross-origin isolated, SharedArrayBuffer in the page and in a
             dedicated worker, a SharedArrayBuffer shared with that worker, "DIP isolate-and-require-corp" in the call
             log's page line; on any engine: a cross-origin script without CORP still blocked there (one with CORP
             loads), and COOP same-origin-allow-popups with COEP, or DIP without COEP, still not isolated
  loopback   (--webrtc) two RTCPeerConnections in one page exchange audio (a WebAudio oscillator) and video (an
             animated canvas): both connect, frames and audio arrive, and the codecs are Opus and VP8, then VP9, then
             H.264 (the receiver's codec preferences); the received video plays in a <video> element, as a call shows
             it, and its frames are counted there too (requestVideoFrameCallback, videoWidth). The element matters:
             where GStreamer has a hardware decoder for the codec (vah264dec on an AMD or Intel GPU), WebKit leaves the
             received track encoded and decodes it only in the pipeline of the element that plays it, so a track no
             element plays decodes nothing and its inbound-rtp stats have no framesDecoded. The call log shows the
             connection, its states and codecs

  swnotify   (--swnotify: an engine with lordos-sw-notifications.patch, +4.2; needs dbus-run-session) a page's service
             worker calls registration.showNotification, under a session bus of its own with webcalls-check's own
             notification server on it. With the page open: two notifications with one tag reach the view's
             show-notification with their title, body and tag, the first closed when the second replaces it, none on
             D-Bus; webcalls-check clicks the second (webkit_notification_clicked, as lord-web does) and the worker's
             notificationclick gets it, its clients.focus() resolves and the view's lordos-focus-from-service-worker
             fires. With no view of the worker's origin (the page gone to another origin first): the notification goes
             to D-Bus as before (Notify, the app name set), and the server's click (ActionInvoked) reaches the worker's
             notificationclick

Without --webrtc (an engine without WebRTC, Debian's or LordOS's build before +3) the WebRTC checks are reported as
"not built" and the rest must pass. Prints one line per check and exits 1 on the first failure."""
import http.server
import json
import os
import socketserver
import subprocess
import sys
import tempfile
import threading

CHECK = sys.argv[1] if len(sys.argv) > 1 else "build/webcalls-check"
WEBRTC = "--webrtc" in sys.argv
# Known on LordOS's +3 build (docs/WEBAPPS.md "Calls", "The network"): WebKit's web process has no network of its own
# (its sandbox's --unshare-net) and, without librice, ICE runs there, so libnice finds only the loopback interface,
# which it ignores, and gathers no candidate. With this flag a loopback that gathered no candidate at all is reported
# as that known block, not a failure; anything else (a candidate gathered and still no connection, no frames, a
# wrong codec) still fails.
NO_NETWORK_KNOWN = "--no-network-known" in sys.argv
DIP = "--dip" in sys.argv
SWNOTIFY = "--swnotify" in sys.argv
MAJOR = None

ACCEPT_CH = ("Sec-CH-UA-Full-Version-List, Sec-CH-UA-Platform-Version, Sec-CH-UA-Arch, Sec-CH-UA-Bitness, "
             "Sec-CH-UA-Model, Sec-CH-UA-WoW64, Sec-CH-UA-Form-Factors")

UA_PAGE = b"""<!doctype html><title>ua</title><script>
(async () => {
  const d = navigator.userAgentData;
  const o = { ua: navigator.userAgent, vendor: navigator.vendor, platform: navigator.platform, secure: isSecureContext,
    uad: d ? d.toJSON() : null,
    he: d ? await d.getHighEntropyValues(['architecture','bitness','model','platformVersion','fullVersionList','uaFullVersion','wow64','formFactors']) : null,
    heSome: d ? await d.getHighEntropyValues(['bitness']) : null,
    heNone: d ? await d.getHighEntropyValues([]) : null,
    noArg: d ? await d.getHighEntropyValues().then(() => 'resolved', e => e.name) : null,
    tag: d ? Object.prototype.toString.call(d) : null,
    native: d ? [String(Object.getOwnPropertyDescriptor(Navigator.prototype, 'userAgentData').get), String(d.getHighEntropyValues),
                 String(Function.prototype.toString), String(window.RTCPeerConnection || '')] : null,
    sameBrands: d ? d.brands === d.brands && Object.isFrozen(d.brands) : null,
    construct: (() => { try { new NavigatorUAData(); return 'constructed'; } catch (e) { return e.name; } })(),
    api: { RTCPeerConnection: typeof RTCPeerConnection, SharedArrayBuffer: typeof SharedArrayBuffer, Atomics: typeof Atomics,
           crossOriginIsolated: self.crossOriginIsolated, RTCRtpScriptTransform: typeof RTCRtpScriptTransform,
           MediaStreamTrackProcessor: typeof MediaStreamTrackProcessor, MediaStreamTrackGenerator: typeof MediaStreamTrackGenerator,
           VideoEncoder: typeof VideoEncoder, AudioEncoder: typeof AudioEncoder, AudioWorkletNode: typeof AudioWorkletNode,
           WebTransport: typeof WebTransport, getDisplayMedia: typeof (navigator.mediaDevices && navigator.mediaDevices.getDisplayMedia),
           setSinkId: typeof HTMLMediaElement.prototype.setSinkId,
           insertableStreams: typeof RTCRtpSender !== 'undefined' && 'createEncodedStreams' in RTCRtpSender.prototype,
           codecsAudio: window.RTCRtpSender ? RTCRtpSender.getCapabilities('audio').codecs.map(c => c.mimeType) : null,
           codecsVideo: window.RTCRtpSender ? [...new Set(RTCRtpSender.getCapabilities('video').codecs.map(c => c.mimeType))] : null,
           // a TURN server as sites give it (RFC 7065's transport query): accepted, and a bad query refused
           turnQuery: (() => { if (!window.RTCPeerConnection) return null; const t = q => { try { new RTCPeerConnection({ iceServers: [{ urls: 'turn:127.0.0.1:3478' + q, username: 'u', credential: 'p' }] }).close(); return 'ok'; } catch (e) { return e.name; } };
             return [t('?transport=tcp'), t('?transport=udp'), t('?foo=bar')].join(' '); })() } };
  // a device that can't exist: the request fails by its name and no real microphone is opened
  try { await navigator.mediaDevices.getUserMedia({ audio: { deviceId: { exact: 'lordos-no-such-device' } } }); } catch (e) { o.gum = e.name; }
  await fetch('/report/ua', { method: 'POST', body: JSON.stringify(o) });
  document.title = 'done';
})();
</script>"""

SAB_PAGE = b"""<!doctype html><title>sab</title><script>
fetch(location.pathname.replace('/page/', '/report/'), { method: 'POST', body: JSON.stringify({
  sab: typeof SharedArrayBuffer, coi: self.crossOriginIsolated, atomics: typeof Atomics })
}).then(() => { document.title = 'done'; });
</script>"""

# a dedicated worker (served with COEP require-corp, as a site that is isolated serves its workers), a
# SharedArrayBuffer handed to it and written there, and two cross-origin scripts (another port: another origin), one
# without Cross-Origin-Resource-Policy (COEP must block it) and one with CORP cross-origin (it loads)
DIP_PAGE = b"""<!doctype html><title>dip</title><script>
(async () => {
  const name = location.pathname.replace('/page/', '');
  const o = { sab: typeof SharedArrayBuffer, coi: self.crossOriginIsolated, atomics: typeof Atomics };
  try {
    const w = new Worker('/worker.js');
    o.worker = await new Promise(res => {
      w.onmessage = e => res(e.data);
      w.onerror = e => res({ error: 'worker error' });
      if (typeof SharedArrayBuffer === 'function') {
        const sab = new SharedArrayBuffer(16);
        o.shared = 'sent';
        w.postMessage(sab);
        setTimeout(() => { o.sharedValue = new Int32Array(sab)[0]; }, 500);
      } else w.postMessage(null);
      setTimeout(() => res({ error: 'timeout' }), 5000);
    });
    await new Promise(f => setTimeout(f, 700));
  } catch (e) { o.worker = { error: e.name }; }
  const load = src => new Promise(res => { const s = document.createElement('script'); s.src = src;
    s.onload = () => res('loaded'); s.onerror = () => res('blocked'); document.head.appendChild(s); setTimeout(() => res('timeout'), 5000); });
  o.noCorp = await load(XORIGIN + '/res/nocorp.js');
  o.withCorp = await load(XORIGIN + '/res/corp.js');
  await fetch('/report/' + name, { method: 'POST', body: JSON.stringify(o) });
  document.title = 'done';
})();
</script>"""

WORKER_JS = b"""self.onmessage = e => {
  let wrote = false;
  if (e.data && typeof SharedArrayBuffer === 'function' && e.data instanceof SharedArrayBuffer) { Atomics.store(new Int32Array(e.data), 0, 42); wrote = true; }
  postMessage({ sab: typeof SharedArrayBuffer, coi: self.crossOriginIsolated, wrote });
};"""

# the headers a messaging web app's document is served with (captured from Chrome, 2026-11-26)
DIP_HEADERS = [("Cross-Origin-Embedder-Policy", "require-corp"), ("Cross-Origin-Opener-Policy", "same-origin-allow-popups"),
               ("Document-Isolation-Policy", "isolate-and-require-corp"), ("Origin-Agent-Cluster", "?1")]

LOOPBACK_PAGE = b"""<!doctype html><title>loopback</title><canvas id=c width=320 height=240></canvas><script>
async function call(prefer) {
  const r = { prefer };
  // audio from WebKit's mock microphone (WEBCALLS_MOCK_CAPTURE=1: a tone, no device; WebAudio can't run without a sound
  // server, as in the build's test chroot)
  const mic = await navigator.mediaDevices.getUserMedia({ audio: true });
  const c = document.getElementById('c'), g = c.getContext('2d');
  let n = 0;
  const tick = setInterval(() => { g.fillStyle = `hsl(${(n++ * 7) % 360} 80% 50%)`; g.fillRect(0, 0, 320, 240); }, 33);
  const vs = c.captureStream(30);
  const a = new RTCPeerConnection(), b = new RTCPeerConnection();
  // candidates handed across, counted by kind (never their addresses)
  r.candidates = {};
  const count = c => { const k = (c.type || '?') + '/' + (c.protocol || '?'); r.candidates[k] = (r.candidates[k] || 0) + 1; };
  a.onicecandidate = e => { if (e.candidate) { count(e.candidate); b.addIceCandidate(e.candidate); } };
  b.onicecandidate = e => { if (e.candidate) { count(e.candidate); a.addIceCandidate(e.candidate); } };
  const got = { audio: false, video: false };
  // the received video on screen, as a call shows it: frames counted as the element presents them
  const v = document.createElement('video'); v.muted = true; v.autoplay = true; v.playsInline = true; v.width = 320; v.height = 240;
  document.body.appendChild(v);
  let shown = 0;
  const onFrame = () => { shown++; v.requestVideoFrameCallback(onFrame); };
  if (v.requestVideoFrameCallback) v.requestVideoFrameCallback(onFrame);
  b.ontrack = e => { got[e.track.kind] = true; if (e.track.kind === 'video') { v.srcObject = new MediaStream([e.track]); v.play().catch(() => {}); } };
  a.addTrack(mic.getAudioTracks()[0], mic);
  a.addTrack(vs.getVideoTracks()[0], vs);
  // the codec asked for, on the sending side's offer (setCodecPreferences, where the engine has it)
  const order = cs => cs.filter(x => x.mimeType === prefer).concat(cs.filter(x => x.mimeType !== prefer));
  if (prefer) for (const t of a.getTransceivers())
    if (t.sender.track && t.sender.track.kind === 'video' && t.setCodecPreferences) t.setCodecPreferences(order(RTCRtpSender.getCapabilities('video').codecs));
  r.codecPreferences = typeof RTCRtpTransceiver !== 'undefined' && 'setCodecPreferences' in RTCRtpTransceiver.prototype;
  const offer = await a.createOffer(); await a.setLocalDescription(offer); await b.setRemoteDescription(offer);
  if (prefer) for (const t of b.getTransceivers()) {
    if (t.receiver.track.kind !== 'video' || !t.setCodecPreferences) continue;
    t.setCodecPreferences(order(RTCRtpReceiver.getCapabilities('video').codecs));
  }
  const answer = await b.createAnswer(); await b.setLocalDescription(answer); await a.setRemoteDescription(answer);
  const t0 = performance.now();
  while ((a.connectionState !== 'connected' || b.connectionState !== 'connected') && performance.now() - t0 < 15000) {
    if (a.connectionState === 'failed' || b.connectionState === 'failed') break;
    await new Promise(f => setTimeout(f, 100));
  }
  r.connectMs = Math.round(performance.now() - t0);
  r.a = a.connectionState; r.b = b.connectionState; r.ice = b.iceConnectionState; r.gathering = a.iceGatheringState;
  await new Promise(f => setTimeout(f, 4000));
  const s = await b.getStats(), codecs = {};
  s.forEach(x => { if (x.type === 'codec') codecs[x.id] = x.mimeType; });
  s.forEach(x => {
    if (x.type !== 'inbound-rtp') return;
    if (x.kind === 'video') Object.assign(r, { videoCodec: codecs[x.codecId], framesDecoded: x.framesDecoded, width: x.frameWidth, height: x.frameHeight, fps: x.framesPerSecond, videoBytes: x.bytesReceived });
    if (x.kind === 'audio') Object.assign(r, { audioCodec: codecs[x.codecId], audioPackets: x.packetsReceived, audioSamples: x.totalSamplesReceived, audioBytes: x.bytesReceived });
  });
  const sa = await a.getStats();
  sa.forEach(x => { if (x.type === 'outbound-rtp') r['sent_' + x.kind] = x.packetsSent; });
  s.forEach(x => { if (x.type === 'local-candidate') r.local = (r.local || []).concat(x.candidateType + '/' + x.protocol); });
  r.tracks = got;
  r.shown = shown; r.shownWidth = v.videoWidth; r.shownHeight = v.videoHeight;
  r.played = v.getVideoPlaybackQuality ? v.getVideoPlaybackQuality().totalVideoFrames : null;
  clearInterval(tick); v.srcObject = null; v.remove(); a.close(); b.close(); mic.getTracks().forEach(t => t.stop());
  return r;
}
(async () => {
  const out = {};
  for (const p of ['video/VP8', 'video/VP9', 'video/H264']) {
    try { out[p] = await call(p); } catch (e) { out[p] = { error: e.name + ': ' + e.message }; }
  }
  await fetch('/report/loopback', { method: 'POST', body: JSON.stringify(out) });
  document.title = 'done';
})();
</script>"""

# a service worker that shows the notifications a page asks for (after a delay, if asked: the page may be gone by then)
# and reports its notificationclick (title, tag, body, its window clients and what clients.focus() gave) to /report/swclick-<tag>
SW_JS = b"""self.addEventListener('install', () => self.skipWaiting());
self.addEventListener('activate', e => e.waitUntil(self.clients.claim()));
const report = (name, o) => fetch('/report/' + name, { method: 'POST', body: JSON.stringify(o) });
self.addEventListener('message', e => {
  const m = e.data || {};
  e.waitUntil((async () => {
    if (m.delay) await new Promise(f => setTimeout(f, m.delay));
    try {
      for (const n of m.show) await self.registration.showNotification(n.title, { body: n.body, tag: n.tag, data: { n: n.title } });
      const list = await self.registration.getNotifications();
      await report('swshown-' + m.name, { ok: true, open: list.map(x => x.title) });
    } catch (err) { await report('swshown-' + m.name, { ok: false, error: err.name + ': ' + err.message }); }
  })());
});
self.addEventListener('notificationclick', e => {
  e.waitUntil((async () => {
    const r = { title: e.notification.title, tag: e.notification.tag, body: e.notification.body, data: e.notification.data };
    const cs = await self.clients.matchAll({ type: 'window', includeUncontrolled: true });
    r.clients = cs.length;
    if (cs.length) { try { await cs[0].focus(); r.focus = 'resolved'; } catch (err) { r.focus = err.name; } }
    e.notification.close();
    await report('swclick-' + e.notification.tag, r);
  })());
});
self.addEventListener('notificationclose', e => e.waitUntil(report('swclose-' + e.notification.title.replace(/ /g, '_'), {})));
"""

# the worker up and controlling the page, then the notifications asked for; the page waits for the click's report
SW_VIEW_PAGE = b"""<!doctype html><title>sw</title><script>
(async () => {
  const o = { permission: Notification.permission };
  try {
    const reg = await navigator.serviceWorker.register('/sw.js');
    await navigator.serviceWorker.ready;
    reg.active.postMessage({ name: 'view', show: [{ title: 'LordOS first', body: 'uno', tag: 't1' },
                                                  { title: 'LordOS second', body: 'dos \\u00f1', tag: 't1' }] });
    for (let i = 0; i < 150 && (await (await fetch('/status/swclick-t1')).text()) !== 'yes'; i++) await new Promise(f => setTimeout(f, 200));
  } catch (e) { o.error = e.name + ': ' + e.message; }
  await fetch('/report/swview', { method: 'POST', body: JSON.stringify(o) });
  document.title = 'done';
})();
</script>"""

# the worker asked for a notification in 3 s, then the page goes to another origin: no view has the worker's origin
SW_AWAY_PAGE = b"""<!doctype html><title>sw away</title><script>
(async () => {
  const reg = await navigator.serviceWorker.register('/sw.js');
  await navigator.serviceWorker.ready;
  reg.active.postMessage({ name: 'away', delay: 3000, show: [{ title: 'LordOS away', body: 'no view', tag: 't2' }] });
  location.href = XORIGIN + '/away';
})();
</script>"""

# on the other origin: done once the worker reported the D-Bus click
AWAY_PAGE = b"""<!doctype html><title>away</title><script>
(async () => {
  for (let i = 0; i < 150 && (await (await fetch(MAIN + '/status/swclick-t2')).text()) !== 'yes'; i++) await new Promise(f => setTimeout(f, 200));
  document.title = 'done';
})();
</script>"""

reports = {}
requests = []


class Handler(http.server.BaseHTTPRequestHandler):
    def log_message(self, *a):
        pass

    def page(self, body, extra=()):
        self.send_response(200)
        self.send_header("Content-Type", "text/html")
        for k, v in extra:
            self.send_header(k, v)
        self.end_headers()
        self.wfile.write(body)

    def do_GET(self):
        requests.append((self.path, {k.lower(): v for k, v in self.headers.items()}))
        if self.path == "/page/ua":
            self.page(UA_PAGE, [("Accept-CH", ACCEPT_CH)])
        elif self.path == "/page/coi":
            self.page(SAB_PAGE, [("Cross-Origin-Opener-Policy", "same-origin"), ("Cross-Origin-Embedder-Policy", "require-corp")])
        elif self.path == "/page/plain":
            self.page(SAB_PAGE)
        elif self.path == "/page/loopback":
            self.page(LOOPBACK_PAGE)
        elif self.path == "/page/dip":
            self.page(DIP_PAGE.replace(b"XORIGIN", b"'http://127.0.0.1:%d'" % xport), DIP_HEADERS)
        elif self.path == "/page/soap":
            self.page(DIP_PAGE.replace(b"XORIGIN", b"'http://127.0.0.1:%d'" % xport),
                      [("Cross-Origin-Embedder-Policy", "require-corp"), ("Cross-Origin-Opener-Policy", "same-origin-allow-popups")])
        elif self.path == "/page/dipnocoep":
            self.page(DIP_PAGE.replace(b"XORIGIN", b"'http://127.0.0.1:%d'" % xport),
                      [("Cross-Origin-Opener-Policy", "same-origin-allow-popups"), ("Document-Isolation-Policy", "isolate-and-require-corp")])
        elif self.path == "/page/swview":
            self.page(SW_VIEW_PAGE)
        elif self.path == "/page/swaway":
            self.page(SW_AWAY_PAGE.replace(b"XORIGIN", b"'http://127.0.0.1:%d'" % xport))
        elif self.path == "/sw.js":
            self.send_response(200)
            self.send_header("Content-Type", "text/javascript")
            self.end_headers()
            self.wfile.write(SW_JS)
        elif self.path.startswith("/status/"):
            self.send_response(200)
            self.send_header("Content-Type", "text/plain")
            self.send_header("Access-Control-Allow-Origin", "*")
            self.end_headers()
            self.wfile.write(b"yes" if "/report/" + self.path[len("/status/"):] in reports else b"no")
        elif self.path == "/worker.js":
            self.send_response(200)
            self.send_header("Content-Type", "text/javascript")
            self.send_header("Cross-Origin-Embedder-Policy", "require-corp")
            self.end_headers()
            self.wfile.write(WORKER_JS)
        else:
            self.send_response(404)
            self.end_headers()

    def do_POST(self):
        n = int(self.headers.get("Content-Length", "0"))
        requests.append((self.path, {k.lower(): v for k, v in self.headers.items()}))
        reports[self.path] = json.loads(self.rfile.read(n) or b"null")
        self.send_response(204)
        self.end_headers()


class Server(socketserver.ThreadingMixIn, http.server.HTTPServer):
    daemon_threads = True


class XHandler(http.server.BaseHTTPRequestHandler):
    """the other origin: two scripts, one without CORP and one with CORP cross-origin"""
    def log_message(self, *a):
        pass

    def do_GET(self):
        if self.path == "/away":
            self.send_response(200)
            self.send_header("Content-Type", "text/html")
            self.end_headers()
            self.wfile.write(AWAY_PAGE.replace(b"MAIN", b"'http://127.0.0.1:%d'" % port))
            return
        if self.path not in ("/res/nocorp.js", "/res/corp.js"):
            self.send_response(404)
            self.end_headers()
            return
        self.send_response(200)
        self.send_header("Content-Type", "text/javascript")
        if self.path == "/res/corp.js":
            self.send_header("Cross-Origin-Resource-Policy", "cross-origin")
        self.end_headers()
        self.wfile.write(b"window.__x = 1;")


def fail(msg):
    print("FAIL", msg)
    sys.exit(1)


def ok(msg):
    print("ok  ", msg)


def run(path, secs=40, mock=False, swnotify=False):
    env = dict(os.environ)
    if mock:
        env["WEBCALLS_MOCK_CAPTURE"] = "1"
    home = tempfile.mkdtemp(prefix="webcalls-home-")
    env.update(HOME=home, XDG_CACHE_HOME=home + "/.cache", XDG_DATA_HOME=home + "/.local/share", XDG_CONFIG_HOME=home + "/.config")
    cmd = [CHECK, "http://127.0.0.1:%d%s" % (port, path), str(secs)]
    if swnotify:
        # a session bus of its own, with webcalls-check's notification server on it (never the desktop's)
        env["WEBCALLS_SWNOTIFY"] = "1"
        env.pop("DBUS_SESSION_BUS_ADDRESS", None)
        cmd = ["dbus-run-session", "--"] + cmd
    p = subprocess.run(cmd, env=env, capture_output=True, text=True, timeout=secs + 30)
    lines = p.stdout.splitlines()
    if p.returncode != 0:
        print(p.stdout + p.stderr)
        fail("%s: the page didn't finish" % path)
    return [l for l in lines if l.startswith("webcall: ")], lines


def brands(major, full):
    """Chromium's GREASE brand list, worked out independently of webua.c"""
    chars = " (:-./);=?_"
    grease = ("Not%sA%sBrand" % (chars[major % 11], chars[(major + 1) % 11]), ["8", "99", "24"][major % 3])
    order = [(0, 1, 2), (0, 2, 1), (1, 0, 2), (1, 2, 0), (2, 0, 1), (2, 1, 0)][major % 6]
    v = full if full else str(major)
    gv = grease[1] + ".0.0.0" if full else grease[1]
    out = [None] * 3
    out[order[0]] = (grease[0], gv)
    out[order[1]] = ("Chromium", v)
    out[order[2]] = ("Google Chrome", v)
    return out


def header_of(bl):
    return ", ".join('"%s";v="%s"' % b for b in bl)


srv = Server(("127.0.0.1", 0), Handler)
port = srv.server_address[1]
threading.Thread(target=srv.serve_forever, daemon=True).start()
xsrv = Server(("127.0.0.1", 0), XHandler)
xport = xsrv.server_address[1]
threading.Thread(target=xsrv.serve_forever, daemon=True).start()

# ---- identity ----
calls, out = run("/page/ua")
engine = next((l for l in out if l.startswith("engine: ")), "engine: ?")
print(engine)
r = reports.get("/report/ua") or fail("no report from the identity page")
ua = r["ua"]
if "Chrome/" not in ua or "LordOS" in ua:
    fail("the user agent isn't Chrome's: " + ua)
MAJOR = int(ua.split("Chrome/")[1].split(".")[0])
full = r["he"]["uaFullVersion"] if r.get("he") else None
if not full or not full.startswith("%d." % MAJOR):
    fail("uaFullVersion %r doesn't match the string's major %d" % (full, MAJOR))
low = brands(MAJOR, None)
hi = brands(MAJOR, full)
nav = [h for p, h in requests if p == "/page/ua"][0]
want_low = {"sec-ch-ua": header_of(low), "sec-ch-ua-mobile": "?0", "sec-ch-ua-platform": '"Linux"'}
for k, v in want_low.items():
    if nav.get(k) != v:
        fail("first request: %s is %r, want %r" % (k, nav.get(k), v))
if any(k.startswith("sec-ch-ua-") and k not in want_low for k in nav):
    fail("first request carries a high-entropy hint before the site asked: %s" % sorted(nav))
ok("first request: Sec-CH-UA %s, Mobile ?0, Platform \"Linux\", nothing else" % nav["sec-ch-ua"])
rep = [h for p, h in requests if p == "/report/ua"][0]
arch = {"x86_64": "x86", "aarch64": "arm"}.get(os.uname().machine, "")
want_hi = dict(want_low)
want_hi.update({"sec-ch-ua-full-version-list": header_of(hi), "sec-ch-ua-platform-version": '""', "sec-ch-ua-arch": '"%s"' % arch,
                "sec-ch-ua-bitness": '"64"', "sec-ch-ua-model": '""', "sec-ch-ua-wow64": "?0", "sec-ch-ua-form-factors": '"Desktop"'})
for k, v in want_hi.items():
    if rep.get(k) != v:
        fail("after Accept-CH: %s is %r, want %r" % (k, rep.get(k), v))
if "sec-ch-ua-full-version" in rep:
    fail("Sec-CH-UA-Full-Version sent though the site didn't ask for it")
ok("after Accept-CH: the seven high-entropy hints the site asked for, Chrome's values (arch %s)" % arch)
want_uad = {"brands": [{"brand": b, "version": v} for b, v in low], "mobile": False, "platform": "Linux"}
if r["uad"] != want_uad:
    fail("userAgentData.toJSON() %r, want %r" % (r["uad"], want_uad))
want_he = {"architecture": arch, "bitness": "64", "brands": want_uad["brands"], "formFactors": ["Desktop"],
           "fullVersionList": [{"brand": b, "version": v} for b, v in hi], "mobile": False, "model": "", "platform": "Linux",
           "platformVersion": "", "uaFullVersion": full, "wow64": False}
if r["he"] != want_he or list(r["he"].keys()) != list(want_he.keys()):
    fail("getHighEntropyValues %r, want %r (in this order)" % (r["he"], want_he))
if r["heSome"] != {"bitness": "64", "brands": want_uad["brands"], "mobile": False, "platform": "Linux"}:
    fail("getHighEntropyValues(['bitness']) %r" % r["heSome"])
if r["heNone"] != want_uad or r["noArg"] != "TypeError" or r["tag"] != "[object NavigatorUAData]" or not r["sameBrands"] or r["construct"] != "TypeError":
    fail("userAgentData's shape: %r" % {k: r[k] for k in ("heNone", "noArg", "tag", "sameBrands", "construct")})
if r["vendor"] != "Google Inc.":
    fail("navigator.vendor %r" % r["vendor"])
want_native = ["function get userAgentData() { [native code] }", "function getHighEntropyValues() { [native code] }",
               "function toString() { [native code] }"]
if r["native"][:3] != want_native:
    fail("functions don't read as native: %r" % r["native"])
if r["api"]["RTCPeerConnection"] == "function" and "[native code]" not in r["native"][3]:
    fail("RTCPeerConnection doesn't read as native: %r" % r["native"][3])
ok("navigator.userAgentData as Chrome's (toJSON, getHighEntropyValues in a dictionary's order, frozen brands, not "
   "constructible, native-looking), navigator.vendor \"Google Inc.\" (navigator.platform %r)" % r["platform"])
if r.get("gum") is None or not any(("getUserMedia audio: failed: %s" % r["gum"]) in c for c in calls):
    fail("getUserMedia's failure (%r) not in the call log: %r" % (r.get("gum"), calls))
envline = [c for c in calls if c.startswith("webcall: page: ")]
if not envline:
    fail("no 'page:' line in the call log: %r" % calls)
if not any(c.startswith("webcall: client hints asked by the app: sec-ch-ua-full-version-list") for c in calls):
    fail("the Client Hints the site asked for aren't in the call log: %r" % calls)
ok("call log: " + " | ".join(c[len("webcall: "):] for c in calls))
api = r["api"]
if api["RTCPeerConnection"] == "function":
    if api.get("turnQuery") != "ok ok SyntaxError":
        fail("TURN URLs with ?transport=: %r, want 'ok ok SyntaxError' (lordos-turn-transport-query.patch)" % api.get("turnQuery"))
    ok("TURN URLs with ?transport=tcp and ?transport=udp accepted, another query refused")
print("     APIs:", json.dumps(api, sort_keys=True))

# ---- isolation ----
run("/page/plain")
plain = reports.get("/report/plain") or fail("no report from the plain page")
if plain["sab"] != "undefined" or plain["coi"]:
    fail("SharedArrayBuffer on a page that isn't cross-origin isolated: %r" % plain)
run("/page/coi")
coi = reports.get("/report/coi") or fail("no report from the isolated page")
if not coi["coi"]:
    fail("COOP+COEP page isn't cross-origin isolated: %r" % coi)
if WEBRTC and coi["sab"] != "function":
    fail("no SharedArrayBuffer on the cross-origin-isolated page: %r" % coi)
ok("SharedArrayBuffer: %s on the cross-origin-isolated page, undefined on the plain one (Atomics %s)" % (coi["sab"], coi["atomics"]))
run("/page/plain")
again = reports["/report/plain"]
if again["sab"] != "undefined":
    fail("SharedArrayBuffer leaked to a plain page after an isolated one: %r" % again)
ok("a plain page after the isolated one still has no SharedArrayBuffer")

# ---- Document-Isolation-Policy ----
calls, _ = run("/page/dip")
d = reports.get("/report/dip") or fail("no report from the DIP page")
print("     DIP page:", json.dumps(d, sort_keys=True))
if d.get("noCorp") != "blocked" or d.get("withCorp") != "loaded":
    fail("DIP page: a cross-origin script without CORP %s, with CORP %s (want blocked, loaded)" % (d.get("noCorp"), d.get("withCorp")))
ok("DIP page: a cross-origin script without CORP blocked, one with CORP cross-origin loaded (COEP require-corp enforced)")
pl = [c for c in calls if c.startswith("webcall: page: ")]
if not pl or not pl[0].endswith(", DIP isolate-and-require-corp"):
    fail("the call log's page line lacks 'DIP isolate-and-require-corp': %r" % pl)
wk = d.get("worker") or {}
if DIP:
    if not d["coi"] or d["sab"] != "function":
        fail("the DIP page isn't cross-origin isolated (lordos-document-isolation-policy.patch): %r" % d)
    if wk.get("sab") != "function" or not wk.get("wrote") or d.get("sharedValue") != 42:
        fail("the DIP page's dedicated worker: %r, shared value %r (want SharedArrayBuffer there and 42 written)" % (wk, d.get("sharedValue")))
    ok("DIP page (COOP same-origin-allow-popups, COEP require-corp, DIP isolate-and-require-corp): crossOriginIsolated, "
       "SharedArrayBuffer in the page and its worker (worker crossOriginIsolated %s), 42 written there through a shared buffer" % wk.get("coi"))
else:
    print("     DIP page without --dip: crossOriginIsolated %s, SharedArrayBuffer %s (an engine without the DIP patch isn't isolated)" % (d["coi"], d["sab"]))
ok("call log: " + pl[0][len("webcall: "):])
for name, what in (("soap", "COOP same-origin-allow-popups with COEP, no DIP"), ("dipnocoep", "DIP without COEP")):
    run("/page/" + name)
    r2 = reports.get("/report/" + name) or fail("no report from /page/" + name)
    if r2["coi"] or r2["sab"] != "undefined" or (r2.get("worker") or {}).get("sab") not in ("undefined", None):
        fail("%s is cross-origin isolated or has SharedArrayBuffer: %r" % (what, r2))
    ok("%s: not cross-origin isolated, no SharedArrayBuffer in the page or its worker" % what)
run("/page/plain")
if reports["/report/plain"]["sab"] != "undefined":
    fail("SharedArrayBuffer leaked to a plain page after the DIP one: %r" % reports["/report/plain"])
ok("a plain page after the DIP ones still has no SharedArrayBuffer")

# ---- a service worker's notifications ----
if SWNOTIFY:
    import shutil
    shutil.which("dbus-run-session") or fail("--swnotify needs dbus-run-session (package dbus-daemon)")
    _, out = run("/page/swview", 60, swnotify=True)
    v = reports.get("/report/swview") or fail("no report from the service-worker page")
    shown = reports.get("/report/swshown-view") or fail("the worker didn't report its notifications: %r" % out)
    if v.get("error") or not shown.get("ok"):
        fail("service-worker page: %r, worker: %r" % (v, shown))
    sn = [l for l in out if l.startswith("show-notification: ")]
    want = ["show-notification: title=LordOS first body=uno tag=t1", "show-notification: title=LordOS second body=dos \u00f1 tag=t1"]
    if sn != want:
        fail("the view's show-notification: %r, want %r (output %r)" % (sn, want, out))
    if any(l.startswith("dbus Notify") for l in out):
        fail("a service worker's notification went to D-Bus though its page's view is open: %r" % out)
    ok("service worker: two notifications with one tag reached the view's show-notification (title, body, tag), none on D-Bus")
    if "notification closed: title=LordOS first body=uno tag=t1" not in out:
        fail("the first notification wasn't closed when the second took its tag: %r" % out)
    ok("the second replaced the first (same tag, same origin): the first closed (getNotifications: %r)" % shown.get("open"))
    c = reports.get("/report/swclick-t1") or fail("the click (webkit_notification_clicked) didn't reach the worker's notificationclick: %r" % out)
    if c.get("title") != "LordOS second" or c.get("body") != "dos \u00f1" or c.get("data") != {"n": "LordOS second"}:
        fail("notificationclick got %r" % c)
    ok("webkit_notification_clicked reached the worker's notificationclick (title, body, data; %d window client)" % c.get("clients", 0))
    if c.get("focus") != "resolved" or "focus-from-service-worker" not in out:
        fail("clients.focus(): %r, lordos-focus-from-service-worker %s" % (c.get("focus"), "focus-from-service-worker" in out))
    ok("the worker's clients.focus() resolved and the view emitted lordos-focus-from-service-worker")
    _, out = run("/page/swaway", 60, swnotify=True)
    away = reports.get("/report/swshown-away") or fail("the worker didn't report its notification after the page left: %r" % out)
    if not away.get("ok"):
        fail("the worker's notification with no view: %r" % away)
    dn = [l for l in out if l.startswith("dbus Notify: ")]
    if len(dn) != 1 or not dn[0].startswith("dbus Notify: app=webcalls-check title=LordOS away body=no view ") or any(l.startswith("show-notification") for l in out):
        fail("with no view of the worker's origin, want one D-Bus Notify and no show-notification: %r" % out)
    ok("no view of the worker's origin: WebKit's D-Bus notification as before (%s)" % dn[0][len("dbus Notify: "):])
    c = reports.get("/report/swclick-t2") or fail("the D-Bus click (ActionInvoked) didn't reach the worker's notificationclick: %r" % out)
    if c.get("title") != "LordOS away":
        fail("notificationclick after the D-Bus click got %r" % c)
    ok("the D-Bus click reached the worker's notificationclick (%d window clients)" % c.get("clients", 0))

# ---- loopback ----
if api["RTCPeerConnection"] != "function":
    if WEBRTC:
        fail("RTCPeerConnection is %s: this WebKit has no WebRTC" % api["RTCPeerConnection"])
    print("skip WebRTC loopback: not built in this WebKit (RTCPeerConnection %s)" % api["RTCPeerConnection"])
    sys.exit(0)
calls, out = run("/page/loopback", 90, mock=True)
lb = reports.get("/report/loopback") or fail("no report from the loopback page")
print("     loopback:", json.dumps(lb, sort_keys=True))
VIDEO = ("video/VP8", "video/VP9", "video/H264")
for want, res in ((k, lb.get(k)) for k in VIDEO):
    if not res or res.get("error"):
        fail("%s: %r" % (want, res))
    if NO_NETWORK_KNOWN and not res.get("candidates") and res["a"] != "connected":
        print("blocked %s: no ICE candidate gathered (the web process has no network; known, see docs/WEBAPPS.md \"Calls\")" % want)
        continue
    if res["a"] != "connected" or res["b"] != "connected":
        fail("%s: not connected (%s / %s, ICE %s, gathering %s; candidates %s)" % (want, res["a"], res["b"], res.get("ice"), res.get("gathering"), res.get("candidates")))
    if not res["tracks"]["audio"] or not res["tracks"]["video"]:
        fail("%s: tracks %r" % (want, res["tracks"]))
    if (res.get("framesDecoded") or 0) < 10 or not res.get("width"):
        fail("%s: no video frames: %r" % (want, res))
    if (res.get("shown") or 0) < 10 or not res.get("shownWidth"):
        fail("%s: the <video> element showed no frames: %r" % (want, res))
    if (res.get("audioPackets") or 0) < 10:
        fail("%s: no audio: %r" % (want, res))
    if (res.get("audioCodec") or "").lower() != "audio/opus":       # WebKit writes "audio/OPUS"
        fail("%s: audio codec %r" % (want, res.get("audioCodec")))
    if (res.get("videoCodec") or "").lower() != want.lower():
        fail("video codec %r, the receiver preferred %s" % (res.get("videoCodec"), want))
    ok("loopback %s: connected in %d ms, %s + %s, %d frames %dx%d at %.0f fps, %d shown by <video> at %dx%d, %d audio packets" % (
        want, res["connectMs"], res["audioCodec"], res["videoCodec"], res["framesDecoded"], res["width"], res["height"],
        res.get("fps") or 0, res["shown"], res["shownWidth"], res["shownHeight"], res["audioPackets"]))
if NO_NETWORK_KNOWN and all(not (lb.get(k) or {}).get("candidates") for k in VIDEO):
    for n in ["peer connection 1 created", "peer connection 1: ice-gathering gathering"]:
        if not any(n in c for c in calls):
            fail("call log lacks %r: %r" % (n, calls))
    ok("call log: %s" % " | ".join(c[len("webcall: "):] for c in calls[:10]))
    sys.exit(0)
need = ["peer connection 1 created", "peer connection 1: connection connected", "peer connection 2: connection connected"]
for n in need:
    if not any(n in c for c in calls):
        fail("call log lacks %r: %r" % (n, calls))
for c in calls:
    for bad in ("candidate:", "a=", "127.0.0.1", "ufrag", "fingerprint"):
        if bad in c:
            fail("call log line with %r: %s" % (bad, c))
ok("call log: %d lines, no SDP, candidate or address: %s" % (len(calls), " | ".join(c[len("webcall: "):] for c in calls[:12])))
