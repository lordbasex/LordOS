#!/usr/bin/env python3
# hands_e2e.py — Hands end to end (docs/HANDS.md "Tests"): the real build/lordos-mcp --headless, speaking MCP over stdio as
# Claude Code does, against a private headless desktop (build/lord-server-test --hands-qa --test-hooks, a private bus, a
# demo home). Nothing of the person's is read or touched. From server/, after `npm run build` and `make`:
#   python3 tests/hands_e2e.py
#   1. the maintainer's example: open Notes, File → Import…, pick Documents/Shopping list.md, Choose: the note exists;
#      and a recording exported as a GIF;
#   2. a permission prompt (Screenshots asks for the screen): a click aimed at it and a key are refused, the screenshot
#      shows it black;
#   3. the banner shows on the display while the agent is connected, and the person's click on its Stop (through the test
#      hooks, as a real pointer) stops the agent: its next call is refused as stopped.
# Exit 0 passed, 1 failed, 77 skipped (no theme bundle or binaries).
import base64, json, os, re, socket, subprocess, sys, tempfile, time, zlib

HERE = os.path.dirname(os.path.abspath(__file__))
SERVER = os.path.abspath(os.path.join(HERE, '..'))
ROOT = os.path.abspath(os.path.join(SERVER, '..'))
MCP = os.path.join(SERVER, 'build/lordos-mcp')
LS = os.path.join(SERVER, 'build/lord-server-test')
fails = passes = 0


def check(ok, name):
    global fails, passes
    if ok:
        passes += 1
        print(f'  ok    {name}')
    else:
        fails += 1
        print(f'  FAIL  {name}')


class Agent:
    def __init__(self, app):
        env = dict(os.environ, LORDOS_MCP_SERVER=LS, LORDOS_MCP_TEST_HOOKS='1')
        env.pop('DBUS_SESSION_BUS_ADDRESS', None)
        self.p = subprocess.Popen([MCP, '--headless', app], stdin=subprocess.PIPE, stdout=subprocess.PIPE, stderr=subprocess.PIPE, env=env, text=True, bufsize=1)
        self.dir = None
        t0 = time.time()
        while time.time() - t0 < 90:
            line = self.p.stderr.readline()
            if not line:
                break
            m = re.search(r'demo home (\S+)/home', line)
            if m:
                self.dir = m.group(1)
                break
        self.id = 0
        self.req('initialize', {'protocolVersion': '2025-06-18', 'clientInfo': {'name': 'claude-code'}})

    def req(self, method, params=None):
        self.id += 1
        self.p.stdin.write(json.dumps({'jsonrpc': '2.0', 'id': self.id, 'method': method, 'params': params or {}}) + '\n')
        self.p.stdin.flush()
        return json.loads(self.p.stdout.readline())

    def call(self, name, **args):
        return self.req('tools/call', {'name': name, 'arguments': args})['result']

    def text(self, name, **args):
        r = self.call(name, **args)
        t = '\n'.join(c.get('text', '') for c in r['content'] if c['type'] == 'text')
        return ('ERROR: ' if r.get('isError') else '') + t

    def png(self, **args):
        r = self.call('computer', action='screenshot', **args)
        for c in r['content']:
            if c['type'] == 'image':
                return base64.b64decode(c['data'])
        return None

    def hook(self, cmd):
        s = socket.socket(socket.AF_UNIX, socket.SOCK_SEQPACKET)
        s.connect(os.path.join(self.dir, 'lordos-test-hooks.sock'))
        s.send(cmd.encode())
        a = s.recv(256).decode()
        s.close()
        return a

    def close(self):
        try:
            self.p.stdin.close()
            self.p.wait(15)
        except Exception:
            self.p.kill()


def ref(text, pattern):
    m = re.search(pattern + r'[^\n]*?\[(ref_\d+)\]', text)
    return m.group(1) if m else None


def png_rgb(data):
    """the RGB rows of an 8-bit RGB PNG (what lord-server writes)"""
    assert data[:8] == b'\x89PNG\r\n\x1a\n'
    pos, idat, w, h = 8, b'', 0, 0
    while pos < len(data):
        n = int.from_bytes(data[pos:pos + 4], 'big')
        kind = data[pos + 4:pos + 8]
        body = data[pos + 8:pos + 8 + n]
        if kind == b'IHDR':
            w, h = int.from_bytes(body[0:4], 'big'), int.from_bytes(body[4:8], 'big')
        elif kind == b'IDAT':
            idat += body
        pos += 12 + n
    raw = zlib.decompress(idat)
    rows, prev, stride = [], bytearray(w * 3), w * 3
    for y in range(h):
        f, line = raw[y * (stride + 1)], bytearray(raw[y * (stride + 1) + 1:(y + 1) * (stride + 1)])
        for i in range(stride):
            a = line[i - 3] if i >= 3 else 0
            b = prev[i]
            c = prev[i - 3] if i >= 3 else 0
            if f == 1: line[i] = (line[i] + a) & 255
            elif f == 2: line[i] = (line[i] + b) & 255
            elif f == 3: line[i] = (line[i] + (a + b) // 2) & 255
            elif f == 4:
                pa, pb, pc = abs(b - c), abs(a - c), abs(a + b - 2 * c)
                line[i] = (line[i] + (a if pa <= pb and pa <= pc else b if pb <= pc else c)) & 255
        rows.append(bytes(line))
        prev = line
    return w, h, rows


def ppm(path):
    d = open(path, 'rb').read()
    m = re.match(rb'P6\s+(\d+)\s+(\d+)\s+255\s', d)
    w, h = int(m.group(1)), int(m.group(2))
    return w, h, d[m.end():]


def notes_import():
    print('1. Notes: File → Import… → Shopping list.md')
    a = Agent('notes')
    try:
        check(a.dir is not None, 'the QA desktop is up')
        w = a.text('windows_context')
        check('"id":"notes"' in w, 'windows_context lists Notes')
        t = a.text('read_window', window_id='notes', filter='interactive')
        check(t.startswith('window "Notes" id=notes'), 'read_window: the window and its refs')
        a.text('record', action='start')
        check(not a.text('computer', action='left_click', ref=ref(t, r'clickable "File"')).startswith('ERROR'), 'File clicked')
        time.sleep(0.5)
        t = a.text('find', query='Import')
        check(not a.text('computer', action='left_click', ref=ref(t, r'"Import…"')).startswith('ERROR'), 'Import… clicked')
        time.sleep(0.8)
        t = a.text('read_window', window_id='notes', filter='interactive')
        r = ref(t, r'clickable "Documents"')
        check(r is not None, 'the file picker shows Documents')
        a.text('computer', action='left_click', ref=r)
        time.sleep(0.6)
        t = a.text('read_window', window_id='notes', filter='interactive')
        r = ref(t, r'clickable "Shopping list\.md')
        check(r is not None, 'the fixture file is listed')
        a.text('computer', action='left_click', ref=r)
        time.sleep(0.4)
        t = a.text('read_window', window_id='notes', filter='interactive')
        r = ref(t, r'button "Choose \(1\)"')
        check(r is not None, 'one file chosen')
        a.text('computer', action='left_click', ref=r)
        time.sleep(1.0)
        t = a.text('read_window', window_id='notes')
        check('Shopping list' in t and 'Bread' in t, 'the note exists, with its text')
        a.text('record', action='stop')
        gif = os.path.join(a.dir, 'qa.gif')
        out = a.text('record', action='export', path=gif)
        ok = os.path.exists(gif) and open(gif, 'rb').read(6) == b'GIF89a'
        try:
            from PIL import Image
            im = Image.open(gif)
            ok = ok and im.n_frames >= 5
            im.seek(im.n_frames - 1)
            im.convert('RGB')
        except ImportError:
            pass
        check(ok, f'a recording exported as a GIF ({out})')
        log = a.text('read_app_log', app='notes')
        check(not log.startswith('ERROR'), 'read_app_log answers')
    finally:
        a.close()


def permission_prompt():
    print('2. a permission prompt is out of reach')
    a = Agent('screenshots')
    try:
        t = a.text('find', window_id='desktop', query='Capture')
        r = ref(t, r'clickable "Capture"')
        check(r is not None, 'Screenshots\' Capture found')
        a.text('computer', action='left_click', ref=r)
        time.sleep(1.5)
        c = a.text('computer', action='left_click', coordinate=[640, 250])
        check('Refused (system-layer)' in c, 'a click aimed at the permission prompt is refused')
        k = a.text('computer', action='key', text='Return')
        check('Refused (system-layer)' in k, 'a key while it shows is refused')
        w, h, rows = png_rgb(a.png())
        mid = rows[h // 4][(w // 2) * 3:(w // 2) * 3 + 3]
        check(abs(mid[0] - mid[1]) < 8 and abs(mid[1] - mid[2]) < 8 and mid[0] < 80, f'the screenshot shows it black ({tuple(mid)})')
        t = a.text('read_window', window_id='desktop')
        check('Allow' not in t and 'wants to' not in t, 'read_window has no prompt in it')
    finally:
        a.close()


def banner_stop():
    print('3. the banner and its Stop')
    a = Agent('notes')
    try:
        check(not a.text('windows_context').startswith('ERROR'), 'the agent acts')
        time.sleep(0.5)
        check(a.hook('frame output').startswith('ok'), 'the display\'s picture')
        w, h, px = ppm(os.path.join(a.dir, 'lordos-test-frame-output.ppm'))
        y = 23
        row = [px[(y * w + x) * 3:(y * w + x) * 3 + 3] for x in range(w)]
        purple = [x for x, p in enumerate(row) if p[0] > 150 and p[1] < 130 and p[2] > 190]
        check(len(purple) > 100, f'the banner is on the display ({len(purple)} purple pixels on its row)')
        white = [x for x, p in enumerate(row) if purple and purple[0] <= x <= purple[-1] + 40 and p[0] > 240 and p[1] > 240 and p[2] > 240]
        check(bool(white), 'its Stop button')
        sx = (white[-1] + max(x for x in white if x > white[-1] - 50)) // 2 if white else w // 2
        # the screenshot an agent takes never shows it
        sw, sh, rows = png_rgb(a.png())
        r = rows[y * sh // h]
        cnt = sum(1 for x in range(sw) if r[x * 3] > 150 and r[x * 3 + 1] < 130 and r[x * 3 + 2] > 190)
        check(cnt < 20, f'the agent\'s screenshot doesn\'t show the banner ({cnt})')
        c = a.text('computer', action='left_click', coordinate=[sx * sw // w, y * sh // h])
        check('Refused (system-layer)' in c or 'Refused (hidden-from-agent)' in c, f'the agent can\'t click Stop itself ({c})')
        for cmd in (f'ptr {sx} {y}', 'btn 0 1', 'btn 0 0'):
            a.hook(cmd)
        time.sleep(0.5)
        t = a.text('windows_context')
        check('stopped the agent' in t, 'Stop (the person\'s click) stops the agent')
        a.hook('frame output')
        w, h, px = ppm(os.path.join(a.dir, 'lordos-test-frame-output.ppm'))
        row = [px[(y * w + x) * 3:(y * w + x) * 3 + 3] for x in range(w)]
        check(sum(1 for p in row if p[0] > 150 and p[1] < 130 and p[2] > 190) < 20, 'the banner is gone')
    finally:
        a.close()


def main():
    if not (os.access(MCP, os.X_OK) and os.access(LS, os.X_OK) and os.path.exists(os.path.join(ROOT, 'themes/default/theme.bundle.js'))
            and os.access('/usr/bin/dbus-daemon', os.X_OK)):
        print('skipped: needs build/lordos-mcp, build/lord-server-test, themes/default/theme.bundle.js and dbus-daemon')
        return 77
    os.environ.pop('DBUS_SESSION_BUS_ADDRESS', None)
    notes_import()
    permission_prompt()
    banner_stop()
    print(f'hands-e2e: {passes} passed, {fails} failed')
    return 1 if fails else 0


if __name__ == '__main__':
    sys.exit(main())
