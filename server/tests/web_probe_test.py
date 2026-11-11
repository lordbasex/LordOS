#!/usr/bin/env python3
"""web_probe_test.py — lord-web's probe, end to end (docs/WEBAPPS.md, "Adding an app from an address").

  python3 -I tests/web_probe_test.py build/lord-web [--pss]

Serves a small web app from a local HTTP server of its own (127.0.0.1: the one place lord-web takes http), then:
  - `lord-web --probe` with the page's address on fd 3: the answer must be the manifest's name, start address and
    scope, the page's origin, the theme color in lower case and the manifest's icon resolved against it;
  - `lord-web --probe` on a page without a manifest: the title, the folder as the scope, the page's own icons;
  - `lord-web --probe` with a bad request line and with an https-less address elsewhere: an error, nothing loaded;
  - `lord-web --probe-icon` with the chosen icon: its type, its size and exactly its bytes; an HTML "icon" and an
    icon over 1 MB are refused.
Each probe runs with a throwaway HOME: nothing may be left in it. With --pss it also prints the probe's memory: the
largest total PSS of lord-web and every process under it (WebKit's web and network processes) while it ran.
Needs WPE WebKit and WebKit's sandbox (bubblewrap, xdg-dbus-proxy); no compositor (the probe has no window).
"""
import http.server
import json
import os
import socket
import subprocess
import sys
import tempfile
import threading
import time
import zlib
import struct

LORD_WEB = os.path.abspath(sys.argv[1] if len(sys.argv) > 1 else 'build/lord-web')
WANT_PSS = '--pss' in sys.argv
failed = 0


def check(cond, what):
    global failed
    if not cond:
        failed += 1
        print('FAILED: ' + what)


def png(w, h):
    def chunk(t, d):
        return struct.pack('>I', len(d)) + t + d + struct.pack('>I', zlib.crc32(t + d) & 0xffffffff)
    raw = b''.join(b'\x00' + b'\x25\xd3\x66\xff' * w for _ in range(h))
    return b'\x89PNG\r\n\x1a\n' + chunk(b'IHDR', struct.pack('>IIBBBBB', w, h, 8, 6, 0, 0, 0)) + \
        chunk(b'IDAT', zlib.compress(raw)) + chunk(b'IEND', b'')


ICON = png(48, 48)
SITE = {
    '/app/index.html': (b'<!doctype html><html><head><title>Test App</title>'
                        b'<meta name="theme-color" content="#123ABC">'
                        b'<link rel="manifest" href="/app/app.webmanifest">'
                        b'<link rel="icon" href="/favicon.png" sizes="16x16"></head><body>app</body></html>', 'text/html'),
    '/app/app.webmanifest': (json.dumps({
        'name': 'Lord\tTest  App', 'short_name': 'LT', 'start_url': '/app/?source=pwa', 'scope': '/app/',
        'theme_color': '#25D366',
        'icons': [{'src': 'icons/192.png', 'sizes': '192x192', 'type': 'image/png'},
                  {'src': 'javascript:alert(1)', 'sizes': '1x1'}]}).encode(), 'application/manifest+json'),
    '/app/icons/192.png': (ICON, 'image/png'),
    '/plain/page.html': (b'<!doctype html><html><head><title>  Plain   Page </title>'
                         b'<link rel="icon" href="fav.png" sizes="32x32" type="image/png"></head><body>plain</body></html>',
                         'text/html'),
    '/plain/fav.png': (ICON, 'image/png'),
    '/plain/notanicon.png': (b'<!doctype html><html><body>no</body></html>', 'image/png'),
    '/plain/huge.png': (ICON + b'\0' * (1024 * 1024 + 16), 'image/png'),
}


class Handler(http.server.BaseHTTPRequestHandler):
    def do_GET(self):
        path = self.path.split('?', 1)[0]
        if path == '/app/':
            path = '/app/index.html'
        body, ctype = SITE.get(path, (None, None))
        if body is None:
            self.send_error(404)
            return
        self.send_response(200)
        self.send_header('Content-Type', ctype)
        self.send_header('Content-Length', str(len(body)))
        self.end_headers()
        self.wfile.write(body)

    def log_message(self, *args):
        pass


def tree(root):
    """root and its descendants"""
    kids = {}
    for d in os.listdir('/proc'):
        if not d.isdigit():
            continue
        try:
            with open('/proc/%s/stat' % d) as f:
                ppid = int(f.read().rsplit(')', 1)[1].split()[1])
            kids.setdefault(ppid, []).append(int(d))
        except (OSError, ValueError, IndexError):
            pass
    out, todo = [], [root]
    while todo:
        p = todo.pop()
        out.append(p)
        todo.extend(kids.get(p, []))
    return out


def pss_of_tree(pids):
    """total PSS in kB of pids (smaps_rollup), 0 when it can't be read"""
    total = 0
    for p in pids:
        try:
            with open('/proc/%d/smaps_rollup' % p) as f:
                for line in f:
                    if line.startswith('Pss:'):
                        total += int(line.split()[1])
        except OSError:
            pass
    return total


def sandboxed(root, pids):
    """1 when a WebKit web process under root runs in a mount namespace of its own (bubblewrap's)"""
    try:
        mine = os.readlink('/proc/%d/ns/mnt' % root)
    except OSError:
        return 0
    for p in pids:
        try:
            with open('/proc/%d/comm' % p) as f:
                comm = f.read().strip()
            if comm.startswith('WPEWebProcess') and os.readlink('/proc/%d/ns/mnt' % p) != mine:
                return 1
        except OSError:
            pass
    return 0


def probe(mode, line, timeout=25):
    """runs lord-web <mode> with line on fd 3; (answer bytes, exit code, home left behind, peak PSS kB, sandboxed)"""
    home = tempfile.mkdtemp(prefix='lord-web-probe-home.')
    ours, theirs = socket.socketpair()
    env = {'HOME': home, 'PATH': '/usr/bin:/bin', 'LANG': 'C.UTF-8'}
    for k in ('XDG_RUNTIME_DIR', 'DBUS_SESSION_BUS_ADDRESS'):
        if k in os.environ:
            env[k] = os.environ[k]
    p = subprocess.Popen([LORD_WEB, mode], env=env,
                         preexec_fn=lambda: (os.dup2(theirs.fileno(), 3), os.set_inheritable(3, True)),
                         stdout=subprocess.DEVNULL, stderr=subprocess.PIPE, close_fds=False)
    theirs.close()
    ours.sendall(line)
    ours.settimeout(1.0)
    data, peak, boxed, until = b'', 0, 0, time.time() + timeout
    while time.time() < until:
        pids = tree(p.pid)
        boxed = boxed or sandboxed(p.pid, pids)
        if WANT_PSS:
            peak = max(peak, pss_of_tree(pids))
        try:
            k = ours.recv(65536)
            if not k:
                break
            data += k
        except socket.timeout:
            if p.poll() is not None:
                break
    try:
        code = p.wait(timeout=10)
    except subprocess.TimeoutExpired:
        p.kill()
        code = -9
    err = p.stderr.read().decode(errors='replace')
    ours.close()
    left = []
    for d, dirs, files in os.walk(home):
        left += [os.path.join(d, f) for f in files]
    subprocess.run(['rm', '-rf', home])
    if code != 0:
        print('lord-web %s exited with %s:\n%s' % (mode, code, err))
    return data, code, left, peak, boxed


def answer(data):
    try:
        return json.loads(data.split(b'\n', 1)[0])
    except ValueError:
        return {}


srv = http.server.ThreadingHTTPServer(('127.0.0.1', 0), Handler)
threading.Thread(target=srv.serve_forever, daemon=True).start()
base = 'http://127.0.0.1:%d' % srv.server_address[1]

# a page with a manifest
data, code, left, peak, boxed = probe('--probe', (base + '/app/\n').encode())
a = answer(data)
check(code == 0, 'probe exits with 0')
check(data.endswith(b'\n') and data.count(b'\n') == 1, 'one line: %r' % data[:200])
check(a.get('ok') is True, 'ok: %r' % a)
check(a.get('name') == 'Lord Test App', 'name from the manifest, spaces folded: %r' % a.get('name'))
check(a.get('startUrl') == base + '/app/?source=pwa', 'startUrl: %r' % a.get('startUrl'))
check(a.get('scope') == base + '/app/', 'scope: %r' % a.get('scope'))
check(a.get('origin') == base, 'origin: %r' % a.get('origin'))
check(a.get('themeColor') == '#25d366', 'themeColor: %r' % a.get('themeColor'))
check(a.get('manifest') is True, 'manifest: true')
check(a.get('icons') == [{'src': base + '/app/icons/192.png', 'sizes': '192x192', 'type': 'image/png'}],
      'icons, resolved, javascript: dropped: %r' % a.get('icons'))
check(not left, 'nothing left in the throwaway home: %r' % left[:5])
check(boxed, 'the web process ran in WebKit\'s sandbox (a mount namespace of its own)')
if WANT_PSS:
    print('probe PSS: %d MB (lord-web and its WebKit processes, peak)' % (peak // 1024))

# a page without one
data, code, left, _, _ = probe('--probe', (base + '/plain/page.html#top\n').encode())
a = answer(data)
check(a.get('ok') is True and a.get('manifest') is False, 'no manifest: %r' % a)
check(a.get('name') == 'Plain Page', 'name from the title: %r' % a.get('name'))
check(a.get('startUrl') == base + '/plain/page.html', 'startUrl without the fragment: %r' % a.get('startUrl'))
check(a.get('scope') == base + '/plain/', 'scope: the folder: %r' % a.get('scope'))
check(a.get('themeColor') is None, 'no theme color: %r' % a.get('themeColor'))
check(a.get('icons') == [{'src': base + '/plain/fav.png', 'sizes': '32x32', 'type': 'image/png'}], 'link icons: %r' % a.get('icons'))

# requests that never load anything
for line, what in ((b'http://example.invalid/\n', 'http elsewhere'), (b'file:///etc/passwd\n', 'a file'),
                   (b'https://a.example/\nhttps://b.example/\n', 'two lines'), (b'https://a.example/', 'no newline')):
    data, code, left, _, _ = probe('--probe', line, timeout=10)
    check(answer(data) == {'ok': False, 'error': 'web.probe.request'}, '%s refused: %r' % (what, data[:200]))

# the icon step
data, code, left, _, _ = probe('--probe-icon', (base + '/app/icons/192.png\n').encode())
head, _, body = data.partition(b'\n')
h = answer(head)
check(h == {'ok': True, 'type': 'image/png', 'size': len(ICON)}, 'icon head: %r' % head)
check(body == ICON, 'exactly the icon bytes (%d of %d)' % (len(body), len(ICON)))
check(not left, 'nothing left in the throwaway home: %r' % left[:5])
data, code, left, _, _ = probe('--probe-icon', (base + '/plain/notanicon.png\n').encode())
check(answer(data) == {'ok': False, 'error': 'web.probe.iconType'}, 'an HTML icon refused: %r' % data[:200])
data, code, left, _, _ = probe('--probe-icon', (base + '/plain/huge.png\n').encode())
check(answer(data) == {'ok': False, 'error': 'web.probe.iconTooBig'}, 'an icon over 1 MB refused: %r' % data[:200])

srv.shutdown()
print('web-probe-test: %s' % ('ok' if not failed else '%d failed' % failed))
sys.exit(1 if failed else 0)
