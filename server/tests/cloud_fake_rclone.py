#!/usr/bin/env python3
# cloud_fake_rclone.py — a stand-in for rclone in tests/cloud_signin_fake.sh (issue #87): `rclone authorize` prints a
# token as the real one does, and `rclone rcd` answers the rc calls a browser sign-in makes, with OneDrive's config
# state machine as rclone v1.75.2 runs it (backend/onedrive/onedrive.go Config, lib/oauthutil ConfigOAuth): the token
# question, the connection type, the drive (/me/drives' list), "Drive OK?". $HOME/.fake-rclone.json sets "drives"
# (lord-server gives rclone only HOME, PATH, LANG, USER and XDG_RUNTIME_DIR, so not through the environment):
#   "real"           one drive that opens
#   "stale,real"     a drive first that can't be opened (driveid_final's ConfigError back to choose_type), then the real
#   "none"           no drive at all (chooseDrive's ConfigError "No drives found")
# Every rc call is appended to its "log" file as "method state result" lines. Test builds only (LORDOS_CLOUD_RCLONE).
import base64, json, os, socketserver, sys, time
from http.server import BaseHTTPRequestHandler

try:
    with open(os.path.join(os.environ.get('HOME', '/nonexistent'), '.fake-rclone.json')) as f:
        CFG = json.load(f)
except (OSError, ValueError):
    CFG = {}
LOG = CFG.get('log', '/dev/null')
DRIVES = [d for d in CFG.get('drives', 'real').split(',') if d and d != 'none']
TOKEN = json.dumps({'access_token': 'A' * 3000, 'token_type': 'Bearer', 'refresh_token': 'r' * 900, 'expiry': '2026-11-09T15:00:00Z'})


def log(*parts):
    with open(LOG, 'a') as f:
        f.write(' '.join(str(p) for p in parts) + '\n')


def authorize():
    sys.stderr.write('2026/11/09 12:00:00 NOTICE: Please go to the following link: http://127.0.0.1:53682/auth?state=fakeState123456\n')
    sys.stderr.write('2026/11/09 12:00:00 NOTICE: Log in and authorize rclone for access\n')
    sys.stderr.flush()
    time.sleep(0.3)
    blob = base64.b64encode(json.dumps({'token': TOKEN}).encode()).decode()
    sys.stdout.write('Paste the following into your remote machine --->\n%s\n<---End paste\n' % blob)
    sys.stdout.flush()
    return 0


def question(state, name, default, examples):
    return {'State': state, 'Option': {'Name': name, 'Help': name, 'Default': default, 'DefaultStr': str(default).lower() if isinstance(default, bool) else default,
            'ValueStr': '', 'Type': 'string', 'Examples': [{'Value': v, 'Help': v} for v in examples], 'Exclusive': True, 'Required': False},
            'OAuth': None, 'Result': '', 'Error': ''}


def config_step(state, result):
    """rclone's BackendConfig for the onedrive backend, from the state rclone was given."""
    if state == '':
        return question('*oauth-confirm,choose_type,,', 'config_refresh_token', True, ['true', 'false'])
    if state == '*oauth-confirm,choose_type,,':
        if result != 'false':
            return {'State': '', 'Error': 'fake: would open a browser'}
        state = 'choose_type'
    if state == 'choose_type':
        return question('choose_type_done', 'config_type', 'onedrive', ['onedrive', 'sharepoint', 'url', 'search', 'driveid', 'siteid', 'path'])
    if state == 'choose_type_done':
        if result != 'onedrive':
            return {'State': '', 'Error': 'fake: only onedrive'}
        if not DRIVES:
            return {'State': 'choose_type', 'Option': None, 'Error': 'No drives found'}
        return question('driveid_final', 'config_driveid', 'id-' + DRIVES[0], ['id-' + d for d in DRIVES])
    if state == 'driveid_final':
        if result.startswith('id-stale'):
            return {'State': 'choose_type', 'Option': None, 'Error': 'Failed to query root for drive "%s": itemNotFound: Item not found' % result}
        CONFIG['drive_id'] = result
        return question('driveid_final_end', 'config_drive_ok', True, ['true', 'false'])
    if state == 'driveid_final_end':
        return {'State': '', 'Option': None, 'Error': ''} if result == 'true' else config_step('choose_type', '')
    return {'State': '', 'Error': 'unknown state %r' % state}


CONFIG = {}


class Handler(BaseHTTPRequestHandler):
    protocol_version = 'HTTP/1.0'

    def log_message(self, *a):
        pass

    def address_string(self):
        return 'unix'

    def answer(self, code, obj):
        body = json.dumps(obj).encode()
        self.send_response(code)
        self.send_header('Content-Type', 'application/json')
        self.send_header('Content-Length', str(len(body)))
        self.end_headers()
        self.wfile.write(body)

    def do_POST(self):
        n = int(self.headers.get('Content-Length') or 0)
        try:
            req = json.loads(self.rfile.read(n) or b'{}')
        except ValueError:
            req = {}
        m = self.path.lstrip('/')
        opt = req.get('opt') or {}
        if m in ('config/create', 'config/update'):
            st, res = opt.get('state', ''), opt.get('result', '')
            if m == 'config/create':
                CONFIG.clear()
                CONFIG.update({k: v for k, v in (req.get('parameters') or {}).items()})
                CONFIG['type'] = req.get('type', '')
            log(m, st or '-', res or '-')
            return self.answer(200, config_step(st if opt.get('continue') else '', res))
        log(m)
        if m == 'core/version':
            return self.answer(200, {'version': 'v1.75.2-fake'})
        if m == 'config/providers':
            return self.answer(200, {'providers': [{'Name': 'onedrive', 'Description': 'Microsoft OneDrive', 'Prefix': 'onedrive', 'Options': [
                {'Name': 'client_id', 'Type': 'string', 'Sensitive': True}, {'Name': 'client_secret', 'Type': 'string', 'IsPassword': True},
                {'Name': 'token', 'Type': 'string', 'Sensitive': True}, {'Name': 'region', 'Type': 'string', 'Default': 'global'}]}]})
        if m == 'config/get':
            return self.answer(200, dict(CONFIG))
        if m == 'mount/mount':
            return self.answer(500, {'error': 'fake: no mounts here', 'status': 500})
        if m == 'core/quit':
            self.answer(200, {})
            os._exit(0)
        return self.answer(200, {})


class Server(socketserver.ThreadingMixIn, socketserver.UnixStreamServer):
    daemon_threads = True


def rcd(argv):
    addr = argv[argv.index('--rc-addr') + 1]
    path = addr[len('unix://'):] if addr.startswith('unix://') else addr
    try:
        os.unlink(path)
    except OSError:
        pass
    srv = Server(path, Handler)
    os.chmod(path, 0o600)
    srv.serve_forever()


if __name__ == '__main__':
    args = sys.argv[1:]
    if args and args[0] == 'authorize':
        sys.exit(authorize())
    if args and args[0] == 'rcd':
        rcd(args)
    sys.exit(2)
