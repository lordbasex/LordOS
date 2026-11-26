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
#      hooks, as a real pointer) stops the agent: its next call is refused as stopped;
#   4. a community theme's trust dialog (Settings → Appearance, an untrusted theme): Enter while it has the keyboard and a
#      click on it are refused, and the theme stays untrusted;
#   5. every capture path blacks out handsDeny nodes: Monitor's Network tab (its firewall rule switches) as a window and as
#      the display, both pure black where the switches are, and the window's capture no longer fails;
#   6. a window that runs off the screen: its screenshot says the part on the desktop, and a zoom at its edge works;
#      a wallpaper's sheet (Settings → Wallpaper) is in the window's screenshot and a zoom over it works;
#   7. the agent's cursor in human mode: it shows on the display while it glides and where it clicks, never in the agent's
#      own screenshot, and the click lands.
# Exit 0 passed, 1 failed, 77 skipped (no theme bundle or binaries).
import base64, json, os, re, shutil, socket, subprocess, sys, tempfile, time, zlib

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
        # lordos-mcp's own HOME (where an export is saved) is a temporary folder: the person's is never written
        self.home = tempfile.mkdtemp(prefix='hands-e2e-')
        env = dict(os.environ, LORDOS_MCP_SERVER=LS, LORDOS_MCP_TEST_HOOKS='1', HOME=self.home)
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

    def call(self, tool, **args):
        return self.req('tools/call', {'name': tool, 'arguments': args})['result']

    def text(self, tool, **args):
        r = self.call(tool, **args)
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
        import shutil
        shutil.rmtree(self.home, ignore_errors=True)


# the real account and host: never on the QA desktop (its identity is "Alex Demo", "demo", "demo-pc")
import pwd
_pw = pwd.getpwuid(os.getuid())
REAL = [x for x in {_pw.pw_name, _pw.pw_gecos.split(',')[0].strip(), socket.gethostname()} if x and len(x) >= 3 and x not in ('demo', 'demo-pc', 'Alex Demo')]


def no_leak(text, where):
    found = [x for x in REAL if x.lower() in (text or '').lower()]
    check(not found, f'no real account or host name in {where}' + (' (leaked)' if found else ''))


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
        no_leak(t, 'Notes\' tree')
        no_leak(a.text('read_window', window_id='desktop'), 'the desktop\'s tree')
        a.text('record', action='stop')
        r = a.call('record', action='export')
        data = next((base64.b64decode(c['data']) for c in r['content'] if c['type'] == 'image'), b'')
        out = f'{len(data)} bytes'
        ok = data[:6] == b'GIF89a'
        try:
            import io
            from PIL import Image
            im = Image.open(io.BytesIO(data))
            ok = ok and im.n_frames >= 5
            im.seek(im.n_frames - 1)
            im.convert('RGB')
        except ImportError:
            pass
        w = a.text('record', action='export', name='../escape.gif')
        check(w.startswith('ERROR'), 'an export never takes a path')
        w = a.text('record', action='export', name='notes-import.gif')
        check(os.path.exists(os.path.join(a.home, 'Pictures/LordOS QA/notes-import.gif')), f'saved only in ~/Pictures/LordOS QA ({w})')
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


def trust_dialog():
    print('4. the theme-trust dialog is the person\'s own')
    a = Agent('settings')
    try:
        home = os.path.join(a.dir, 'home')
        inst = subprocess.run(['node', os.path.join(HERE, 'community_theme_install.mjs'), os.path.join(HERE, 'community_themes/okminimal'), home, '--untrusted'],
                              capture_output=True, text=True)
        check(inst.returncode == 0, 'an untrusted community theme in the demo home')
        t = a.text('read_window', window_id='settings')
        check('Alex Demo' in t, 'Settings shows the demo account')
        no_leak(t, 'Settings\' tree')
        no_leak(a.text('read_window', window_id='desktop'), 'the desktop\'s tree with Settings')
        t = a.text('find', window_id='settings', query='Appearance')
        a.text('computer', action='left_click', ref=ref(t, r'row "◐ Appearance"'))
        time.sleep(1.0)
        t = a.text('find', window_id='settings', query='Test Minimal')
        r = ref(t, r'clickable "Test Minimal')
        check(r is not None, 'its card')
        a.text('computer', action='left_click', ref=r)
        time.sleep(1.0)
        t = a.text('find', window_id='settings', query='Apply button')
        r = ref(t, r'button "Apply')
        check(r is not None, 'its page with Apply')
        a.text('computer', action='left_click', ref=r)
        time.sleep(1.0)
        t = a.text('read_window', window_id='settings')
        cat = json.load(open(os.path.join(ROOT, 'assets/locales/en/settings.json')))
        title = cat['settings.style.trustTitle' if 'settings.style.trustTitle' in cat else 'style.trustTitle']
        check(title.split('{')[0][:20] not in t, 'the dialog is not in the tree')
        k = a.text('computer', action='key', text='Return')
        check('Refused (protected-dialog)' in k, f'Enter on the trust dialog refused ({k})')
        w = json.loads(a.text('windows_context'))['windows'][0]
        c = a.text('computer', action='left_click', coordinate=[w['x'] + w['w'] // 2 + 120, w['y'] + w['h'] // 2 + 40])
        check('Refused (hidden-from-agent)' in c, f'a click on its OK refused ({c})')
        rec = json.load(open(os.path.join(home, '.local/share/lordos/themes/okminimal/install.json')))
        check(rec.get('trusted') is False, 'the theme is still untrusted')
    finally:
        a.close()


def px(rows, x, y):
    return tuple(rows[y][x * 3:x * 3 + 3])


def capture_paths():
    print('5. handsDeny is black in every capture path')
    a = Agent('monitor')
    try:
        t = a.text('find', window_id='monitor', query='Network')
        a.text('computer', action='left_click', ref=ref(t, r'tab "Network"'))
        time.sleep(1.5)
        t = a.text('read_window', window_id='monitor')
        m = re.search(r'clickable "Machines[^"]*" \[ref_\d+\] @(\d+),(\d+) (\d+)x(\d+)', t)
        check(m is not None, 'the Network tab\'s first row')
        rx, ry, rw, rh = (int(v) for v in m.groups()) if m else (32, 230, 314, 40)
        sx, sy = rx + rw - 21, ry + 20                       # its Rule switch (handsDeny)
        r = a.call('computer', action='screenshot', target='window:monitor')
        check(not r.get('isError'), 'a window holding handsDeny nodes is captured (no "The action failed.")')
        img = next((base64.b64decode(c['data']) for c in r['content'] if c['type'] == 'image'), None)
        meta = next((c['text'] for c in r['content'] if c['type'] == 'text'), '')
        mm = re.search(r'area at \((\d+), (\d+)\)', meta)
        if img and mm:
            w, h, rows = png_rgb(img)
            ox, oy = int(mm.group(1)), int(mm.group(2))
            check(px(rows, sx - ox, sy - oy) == (0, 0, 0), f'the rule switch is black in the window\'s capture {px(rows, sx - ox, sy - oy)}')
        w, h, rows = png_rgb(a.png())
        check(px(rows, sx, sy) == (0, 0, 0) and px(rows, sx - 9, sy - 4) == (0, 0, 0), f'and in the display\'s {px(rows, sx, sy)}')
        check(px(rows, rx + 80, sy) != (0, 0, 0), 'the rest of the row shows')
    finally:
        a.close()


def zoom_edges():
    print('6. a window off the screen\'s edge, a sheet: screenshots and zooms')
    a = Agent('settings')
    try:
        w = json.loads(a.text('windows_context'))['windows'][0]
        c = a.text('computer', action='left_click_drag', start_coordinate=[w['x'] + w['w'] // 2, w['y'] + 16],
                   coordinate=[w['x'] + w['w'] // 2 + 420, w['y'] + 16])
        time.sleep(0.8)
        w = json.loads(a.text('windows_context'))['windows'][0]
        check(w['x'] + w['w'] > 1280, f'the window runs off the right edge ({c}; x {w["x"]} w {w["w"]})')
        r = a.call('computer', action='screenshot', target='window:settings')
        meta = next((c['text'] for c in r['content'] if c['type'] == 'text'), '')
        mm = re.search(r'(\d+)x(\d+) image of the desktop area at \((\d+), (\d+)\), (\d+)x(\d+) points', meta)
        check(mm is not None and int(mm.group(3)) + int(mm.group(5)) <= 1280, f'its screenshot is the part on the desktop ({meta[:80]})')
        if mm:
            iw, ih = int(mm.group(1)), int(mm.group(2))
            z = a.call('computer', action='zoom', region=[iw - 200, 40, iw, 240])
            check(not z.get('isError'), 'a zoom at its right edge works: ' + next((c['text'] for c in z['content'] if c['type'] == 'text'), '')[:60])
            z = a.call('computer', action='zoom', region=[iw - 100, 40, iw + 300, 240])
            check(not z.get('isError'), 'a zoom running past the edge: the part on the desktop')
        a.text('computer', action='left_click_drag', start_coordinate=[w['x'] + 200, w['y'] + 16], coordinate=[w['x'] - 220, w['y'] + 16])
        time.sleep(0.8)
        t = a.text('find', window_id='settings', query='Wallpaper')
        a.text('computer', action='left_click', ref=ref(t, r'row "[^"]*Wallpaper"'))
        time.sleep(1.2)
        t = a.text('find', window_id='settings', query='Abstract')
        a.text('computer', action='left_click', ref=ref(t, r'clickable "Abstract'))
        time.sleep(1.2)
        a.text('computer', action='scroll', coordinate=[690, 500], scroll_direction='down', scroll_amount=5)
        time.sleep(0.8)
        t = a.text('find', window_id='settings', query='Autumn lake')
        a.text('computer', action='left_click', ref=ref(t, r'clickable "[^"]*Autumn lake'))
        time.sleep(1.5)
        t = a.text('read_window', window_id='settings')
        check('Download and use' in t or 'Use on desktop' in t, 'a wallpaper\'s sheet is open')
        r = a.call('computer', action='screenshot', target='window:settings')
        img = next((base64.b64decode(c['data']) for c in r['content'] if c['type'] == 'image'), None)
        meta = next((c['text'] for c in r['content'] if c['type'] == 'text'), '')
        mm = re.search(r'area at \((\d+), (\d+)\)', meta)
        m2 = re.search(r'button "Download and use[^"]*" \[ref_\d+\] @(\d+),(\d+) (\d+)x(\d+)', t)
        if img and mm and m2:
            w2, h2, rows = png_rgb(img)
            bx, by = int(m2.group(1)) + 8 - int(mm.group(1)), int(m2.group(2)) + int(m2.group(4)) // 2 - int(mm.group(2))
            p = px(rows, bx, by)
            check(p[2] > 200 and p[0] < 80, f'the sheet is in the window\'s screenshot (its blue button {p})')
            ox, oy = int(m2.group(1)) - int(mm.group(1)), int(m2.group(2)) - int(mm.group(2))
            z = a.call('computer', action='zoom', region=[max(0, ox - 100), max(0, oy - 80), ox + 200, oy + 80])
            check(not z.get('isError'), 'a zoom over the sheet works')
        else:
            check(False, 'the sheet in the window\'s screenshot')
    finally:
        a.close()


def agent_cursor():
    print('7. the agent\'s cursor in human mode')
    a = Agent('monitor')
    try:
        check('"effective":"human"' in a.text('hands_mode', mode='human'), 'hands_mode human')
        a.hook('ptr 900 650')                                # the person's pointer, somewhere else
        t = a.text('find', window_id='monitor', query='Processes')
        m = re.search(r'tab "Processes" \[(ref_\d+)\][^@\n]*@(\d+),(\d+) (\d+)x(\d+)', t)
        check(m is not None, 'a tab to click')
        tx, ty = int(m.group(2)) + int(m.group(4)) // 2, int(m.group(3)) + int(m.group(5)) // 2
        import threading
        a.hook('frame output')
        w, h, d = ppm(os.path.join(a.dir, 'lordos-test-frame-output.ppm'))
        def violet(x0, y0, x1, y1):
            n = 0
            for y in range(max(0, y0), min(h, y1)):
                for x in range(max(0, x0), min(w, x1)):
                    p = d[(y * w + x) * 3:(y * w + x) * 3 + 3]
                    if 120 < p[0] < 165 and 70 < p[1] < 115 and p[2] > 215:
                        n += 1
            return n
        base_all, base_near = violet(0, 60, w, h), violet(tx - 10, ty - 10, tx + 140, ty + 60)
        res = {}
        th = threading.Thread(target=lambda: res.setdefault('c', a.text('computer', action='left_click', ref=m.group(1))))
        t0 = time.time()
        th.start()
        time.sleep(0.15)
        a.hook('frame output')
        th.join()
        took = time.time() - t0
        w, h, d = ppm(os.path.join(a.dir, 'lordos-test-frame-output.ppm'))
        mid = violet(0, 60, w, h) - base_all
        check(mid > 40, f'the cursor on the display while it glides ({mid} more violet pixels)')
        check(took >= 0.3, f'the click came after the glide ({took:.2f} s)')
        check(not res.get('c', '').startswith('ERROR'), f'the click landed ({res.get("c")})')
        a.hook('frame output')
        w, h, d = ppm(os.path.join(a.dir, 'lordos-test-frame-output.ppm'))
        near = violet(tx - 10, ty - 10, tx + 140, ty + 60) - base_near
        check(near > 40, f'it rests where it clicked ({near})')
        sw, sh, rows = png_rgb(a.png())
        cnt = 0
        for y in range(max(0, ty - 10), min(sh, ty + 60)):
            for x in range(max(0, tx - 10), min(sw, tx + 140)):
                p = rows[y][x * 3:x * 3 + 3]
                if 120 < p[0] < 165 and 70 < p[1] < 115 and p[2] > 215:
                    cnt += 1
        check(cnt < 10, f'the agent\'s own screenshot never shows it ({cnt})')
        t = a.text('read_window', window_id='monitor', filter='interactive')
        check(re.search(r'tab "Processes" \[ref_\d+\] \{selected\}', t) is not None, 'Processes is the tab shown')
        check('reported' not in t, 'the cursor\'s label is not in the agent\'s tree')
        check(a.hook('where') == 'ok 900 650', f'the person\'s pointer never moved ({a.hook("where")})')
        time.sleep(3.2)                                      # (the person's move above pauses an agent for 3 s)
        k = a.text('computer', action='left_click', ref=m.group(1), mode='machine')
        check(not k.startswith('ERROR'), 'one call in machine mode')
        check(a.hook('where') == 'ok 900 650', 'nor for a machine-mode click')
        a.hook('ptr 905 650')                                # the person moves: their pointer from where they left it
        check(a.hook('where') == 'ok 905 650', 'the person\'s next move takes it from where they left it')
    finally:
        a.close()


def main():
    if not (os.access(MCP, os.X_OK) and os.access(LS, os.X_OK) and os.path.exists(os.path.join(ROOT, 'themes/default/theme.bundle.js'))
            and os.access('/usr/bin/dbus-daemon', os.X_OK) and shutil.which('node')):
        print('skipped: needs build/lordos-mcp, build/lord-server-test, themes/default/theme.bundle.js, dbus-daemon and node')
        return 77
    os.environ.pop('DBUS_SESSION_BUS_ADDRESS', None)
    notes_import()
    permission_prompt()
    banner_stop()
    trust_dialog()
    capture_paths()
    zoom_edges()
    agent_cursor()
    print(f'hands-e2e: {passes} passed, {fails} failed')
    return 1 if fails else 0


if __name__ == '__main__':
    sys.exit(main())
