// cloudmock.js — a stand-in for lord.cloud (docs/CLOUD.md section 17) for working on the interface before
// lord-server has Cloud: four accounts like the mocks' (a personal drive, a work drive syncing, an object storage
// bucket, a WebDAV server that needs a new sign-in), sample folders under ~/Cloud, statuses per file, links, a
// sign-in that finishes by itself after a few seconds, and transfers that move.
//
// Only for development: startCloud() installs it when lord.cloud is missing and settings.json has
// "cloudDevMock": true (or "states", below). It never runs otherwise, and nothing here reaches the network or rclone.

const GB = 1e9, TB = 1e12, MB = 1e6;

const PROVIDERS = [
  ['dropbox', 'Dropbox', 'oauth', 'Personal or business'], ['box', 'Box', 'oauth', 'Personal or business'],
  ['pcloud', 'pCloud', 'oauth', 'Personal or business'], ['yandex', 'Yandex Disk', 'oauth', 'Personal'],
  ['zoho', 'Zoho WorkDrive', 'oauth', 'Business'], ['hidrive', 'HiDrive', 'oauth', 'Personal'], ['jottacloud', 'Jottacloud', 'oauth', 'Personal'],
  ['premiumizeme', 'premiumize.me', 'oauth', 'Personal'], ['putio', 'put.io', 'oauth', 'Personal'],
  ['webdav', 'WebDAV', 'server', 'Nextcloud, ownCloud…'], ['sftp', 'SFTP', 'server', 'Any SSH server'], ['smb', 'SMB', 'server', 'A network share'],
  ['ftp', 'FTP', 'server', 'An FTP server'], ['mega', 'Mega', 'server', 'User and password'], ['koofr', 'Koofr', 'server', 'User and password'],
  ['seafile', 'Seafile', 'server', 'A Seafile server'], ['iclouddrive', 'iCloud Drive', 'server', 'Apple ID and a code'],
  ['s3', 'S3', 'object', 'And compatible services'], ['b2', 'Backblaze B2', 'object', 'Buckets'],
  ['swift', 'Swift', 'object', 'OpenStack object storage'],
  ['crypt', 'Encrypted folder', 'crypt', 'On top of an account'],
];
const F = (name, label, o = {}) => ({ name, label, secret: false, required: false, default: null, examples: [], ...o });
const FIELDS = {
  webdav: [F('url', 'Server', { required: true, examples: ['https://cloud.example.test/remote.php/dav/files/ana'] }),
    F('vendor', 'Preset', { default: 'nextcloud', examples: ['nextcloud', 'owncloud', 'other'] }), F('user', 'User', { required: true }),
    F('pass', 'App password', { secret: true, required: true })],
  // as lord-server lists them (17.2): rclone's help, Sensitive host and user (secret) but only the passwords isPassword
  sftp: [F('host', 'SSH host to connect to.', { required: true, secret: true, isPassword: false, help: 'SSH host to connect to.\n\nE.g. "example.com".' }),
    F('user', 'SSH username.', { secret: true, isPassword: false, help: 'SSH username.' }), F('port', 'SSH port number.', { default: '22', type: 'int', help: 'SSH port number.' }),
    F('pass', 'SSH password, leave blank to use ssh-agent.', { secret: true, isPassword: true }), F('key_pem', 'Raw PEM-encoded private key.', { secret: true, isPassword: false }),
    F('key_file', 'Path to PEM-encoded private key file.', { help: 'Path to PEM-encoded private key file.\n\nLeave blank or set key-use-agent to use ssh-agent.' }),
    F('key_file_pass', 'The passphrase to decrypt the PEM-encoded private key file.', { secret: true, isPassword: true }),
    F('key_use_agent', 'When set forces the usage of the ssh-agent.', { default: 'false', type: 'bool', help: 'When set forces the usage of the ssh-agent.\n\nWhen key-file is also set, the ".pub" file of the specified key-file is read and only the associated key is requested from the ssh-agent.' }),
    F('use_insecure_cipher', 'Enable the use of insecure ciphers and key exchange methods.', { default: 'false', type: 'bool' }),
    F('disable_hashcheck', 'Disable the execution of SSH commands to determine if remote file hashing is available.', { default: 'false', type: 'bool' }),
    F('shell_type', 'The type of SSH shell on remote server, if any.', { examples: ['none', 'unix', 'powershell', 'cmd'] })],
  smb: [F('host', 'Server', { required: true }), F('user', 'User'), F('pass', 'Password', { secret: true })],
  ftp: [F('host', 'Server', { required: true }), F('user', 'User'), F('pass', 'Password', { secret: true })],
  mega: [F('user', 'Email', { required: true }), F('pass', 'Password', { secret: true, required: true })],
  koofr: [F('user', 'Email', { required: true }), F('password', 'App password', { secret: true, required: true })],
  seafile: [F('url', 'Server', { required: true }), F('user', 'Email', { required: true }), F('pass', 'Password', { secret: true, required: true })],
  s3: [F('provider', 'Service', { default: 'Other', examples: ['AWS', 'Ceph', 'Minio', 'Other'] }), F('endpoint', 'Endpoint', { examples: ['s3.us-west-004.example.test'] }),
    F('access_key_id', 'Access key', { required: true }), F('secret_access_key', 'Secret key', { secret: true, required: true }), F('region', 'Region')],
  b2: [F('account', 'Key ID', { required: true }), F('key', 'Application key', { secret: true, required: true })],
  swift: [F('auth', 'Authentication URL', { required: true }), F('user', 'User', { required: true }), F('key', 'Key', { secret: true, required: true })],
  iclouddrive: [F('apple_id', 'Apple ID', { required: true }), F('password', 'Password', { secret: true, required: true })],
  crypt: [F('remote', 'Folder to encrypt', { required: true, examples: ['Work:Private'] }), F('password', 'Password', { secret: true, required: true })],
};

const TREES = {
  Work: [['Projects', 1], ['Projects/2026', 1], ['Projects/Archive', 1], ['Projects/Client handover', 1], ['Projects/Site photos', 1],
    ['Projects/Budget 2027.ods', 0, 'up'], ['Projects/Kickoff recording.mp4', 0, 'down'], ['Projects/Floor plan.pdf', 0, 'here'],
    ['Projects/Logo final.svg', 0, 'kept'], ['Projects/Interviews.zip', 0, 'cloud'], ['Projects/Notes.md', 0, 'error'],
    ['Projects/Proposal v3.odt', 0, 'cloud'], ['Projects/Timeline.ods', 0, 'here'], ['Contracts', 1], ['Templates', 1], ['Quarterly report.pdf', 0, 'cloud']],
  Personal: [['Pictures', 1], ['Pictures/Trip 2026', 1], ['Pictures/Family', 1], ['Pictures/Scans', 1], ['Pictures/IMG_4012.jpg', 0, 'here'],
    ['Pictures/IMG_4013.jpg', 0, 'cloud'], ['Pictures/Sunset.jpg', 0, 'kept'], ['Pictures/Panorama.tif', 0, 'cloud'], ['Pictures/Ticket.pdf', 0, 'cloud'], ['Documents', 1]],
  'Photos archive': [['2024', 1], ['2025', 1], ['2024/Interviews.zip', 0, 'cloud'], ['2024/Beach.jpg', 0, 'here']],
  'Home NAS': [['Shared', 1]],
};

// scenario 'states' (settings.json "cloudDevMock": "states"): a few seconds in, the WebDAV server asks for a new
// sign-in, the personal drive fills up with changes waiting, a file comes back changed on another device and the
// object storage goes offline, so the notifications and the offline dialog can be seen.
export function installCloudMock(home, scenario = '') {
  const now = Date.now();
  const mount = (name) => `${home}/Cloud/${name}`;
  const groupOf = (kind) => (PROVIDERS.find((x) => x[0] === kind) || [])[2] || 'server';
  const acc = (id, name, kind, provider, about, extra = {}) => ({ id, name, kind, provider, group: groupOf(kind), mount: mount(name), state: 'ok', about, cacheBytes: 0, keptBytes: 0,
    up: { files: 0, bytes: 0, done: 0 }, down: { files: 0, bytes: 0, done: 0 }, kept: [], linksSupported: true, mounted: true, ...extra });
  const st = {
    service: { running: true, version: '1.75.2', uptimeS: 5400, rssKb: 41 * 1024, restarts: 0 },
    options: { bwUp: 0, bwDown: 0, pauseOnBattery: true, pauseOnMetered: true, autoFree: true, freeBelowGb: 10 },
    accounts: [
      acc('m1', 'Personal', 'dropbox', 'Dropbox', { total: 2 * TB, used: 612 * GB, free: 1388 * GB, trashed: 20 * GB }, { cacheBytes: 9.1 * GB, keptBytes: 2.1 * GB }),
      acc('m2', 'Work', 'pcloud', 'pCloud Business', { total: 1 * TB, used: 540 * GB, free: 460 * GB, trashed: 20 * GB, other: 30 * GB },
        { state: 'syncing', cacheBytes: 7.2 * GB, keptBytes: 4.1 * GB, up: { files: 1, bytes: 86e3, done: 34e3, name: 'Budget 2027.ods' }, down: { files: 2, bytes: 1.8 * GB, done: 1.15 * GB, name: 'Kickoff recording.mp4' } }),
      // as rclone names it: the long description (Files and Settings show the provider list's short label)
      acc('m3', 'Photos archive', 's3', 'Amazon S3 Compliant Storage Providers including AWS, Alibaba, ArvanCloud, Ceph, ChinaMobile, Cloudflare, Dreamhost', null, { cacheBytes: 1.6 * GB, linksSupported: true, mounted: true }),
      acc('m4', 'Home NAS', 'webdav', 'WebDAV · Nextcloud', { total: 300 * GB, used: 7.4 * GB, free: 292.6 * GB }, { state: 'signin', cacheBytes: 0.5 * GB, linksSupported: false }),
    ],
    signin: null,
    recent: [
      { path: `${mount('Work')}/Projects/Budget 2027.ods`, account: 'm2', dir: 'up', at: now - 10e3 },
      { path: `${mount('Work')}/Projects/Kickoff recording.mp4`, account: 'm2', dir: 'down', at: now - 70e3 },
      { path: `${mount('Work')}/Projects/Notes.md`, account: 'm2', dir: 'up', at: now - 4 * 60e3 },
      { path: `${mount('Personal')}/Pictures/Sunset.jpg`, account: 'm1', dir: 'down', at: now - 12 * 60e3 },
      { path: `${mount('Personal')}/Pictures/IMG_4101.jpg`, account: 'm1', dir: 'up', at: now - 3600e3 },
    ],
    conflicts: [],
  };
  st.accounts[0].trashSupported = true;
  st.accounts[1].trashSupported = true;
  st.accounts[1].kept = [`${mount('Work')}/Projects/2026`, `${mount('Work')}/Contracts`, `${mount('Work')}/Templates`];
  st.accounts[0].kept = [`${mount('Personal')}/Pictures/Family`];
  const status = {};
  const links = { m2: [{ path: `${mount('Work')}/Quarterly report.pdf`, url: 'https://share.example.test/s/XZk4p9QwT2', expires: (now + 7 * 864e5) / 1000, made: now / 1000 },
    { path: `${mount('Work')}/Templates`, url: 'https://share.example.test/s/Brand8kQ', expires: 0, made: now / 1000 }] };
  let seq = 5;
  let pendingAdd = null, measureTimer = 0;

  // sample folders and files (small: only their names matter here)
  const mk = (p) => { try { lord.fs.mkdir(p); } catch (e) { /* there */ } };
  mk(`${home}/Cloud`);
  for (const a of st.accounts) {
    mk(a.mount);
    for (const [rel, dir, s] of TREES[a.name] || []) {
      const p = `${a.mount}/${rel}`;
      if (dir) mk(p); else { try { lord.fs.stat(p); } catch (e) { try { lord.fs.write(p, `${rel}\n`); } catch (er) { /* read only */ } } }
      status[p] = s || (dir ? (a.kept.includes(p) ? 'kept' : 'cloud') : 'cloud');
    }
  }

  const push = () => { if (typeof globalThis.__lordCloud === 'function') globalThis.__lordCloud(JSON.parse(JSON.stringify(st))); };
  const find = (id) => st.accounts.find((a) => a.id === id);
  const ofPath = (p) => st.accounts.find((a) => p === a.mount || p.startsWith(`${a.mount}/`));
  const keptIn = (p) => { const a = ofPath(p); return !!a && a.kept.some((k) => p === k || p.startsWith(`${k}/`)); };

  // the transfers move; the work drive's upload finishes and starts over
  setInterval(() => {
    const w = st.accounts[1];
    if (w.state === 'syncing') {
      w.up.done = Math.min(w.up.bytes, w.up.done + 9e3);
      w.down.done = Math.min(w.down.bytes, w.down.done + 22 * MB);
      if (w.down.done >= w.down.bytes) { w.down = { files: 0, bytes: 0, done: 0 }; status[`${w.mount}/Projects/Kickoff recording.mp4`] = 'here'; }
      if (w.up.done >= w.up.bytes) { w.up = { files: 1, bytes: 86e3, done: 0, name: 'Budget 2027.ods' }; }
    }
    push();
  }, 1000);

  // scenario 'locked' (settings "cloudDevMock": "locked"): the session logged in without a password, the keyring is locked
  // scenario 'crashed': rclone stopped several times (U7)
  if (scenario === 'crashed') { st.service = { ...st.service, running: false, error: 'cloud.crashed' }; }
  if (scenario === 'locked') {
    for (const a of st.accounts.slice(0, 2)) { a.state = 'locked'; a.error = 'cloud.keyringLocked'; a.about = null; a.up = { files: 0, bytes: 0, done: 0 }; a.down = { files: 0, bytes: 0, done: 0 }; }
  }
  if (scenario === 'states') {
    st.accounts[3].state = 'ok';
    setTimeout(() => {
      st.accounts[3].state = 'signin';
      const p = st.accounts[0];
      p.about = { total: 2 * TB, used: 2 * TB, free: 0, trashed: 20 * GB };
      p.state = 'error';
      p.up = { files: 14, bytes: 140 * MB, done: 0, name: 'IMG_4101.jpg' };
      st.accounts[2].state = 'offline';
      const w = st.accounts[1];
      // ours keeps the name; the cloud's version is saved next to it (17.2 item 23)
      st.conflicts = [{ path: `${w.mount}/Projects/Proposal v3.odt`, kept: `${w.mount}/Projects/Proposal v3 (cloud copy).odt`, account: w.id, at: Date.now() }];
      push();
    }, 4000);
  }

  lord.cloud = {
    available: () => true,
    state: () => JSON.parse(JSON.stringify(st)),
    // 17.2: a Promise, a little later (and a failure with settings "cloudDevMock": "providersFail")
    providers: () => new Promise((resolve, reject) => setTimeout(() => (scenario === 'providersFail' ? reject({ error: 'cloud.notRunning' })
      : resolve(PROVIDERS.map(([kind, label, group, hint]) => ({ kind, label, description: hint, group, verified: ['webdav', 'sftp', 's3'].includes(kind),
        fields: group === 'oauth' ? [] : FIELDS[kind] || [] })))), 900)),
    beginSignIn(kind, name) {
      const id = `m${seq++}`;
      const p = PROVIDERS.find((x) => x[0] === kind);
      st.signin = { id, kind, name, phase: 'waiting', url: `http://127.0.0.1:53682/auth?state=${id}`, expiresAt: Date.now() + 300e3 };
      push();
      setTimeout(() => {
        if (!st.signin || st.signin.id !== id) return;
        st.signin = { ...st.signin, phase: 'done' };
        st.accounts.push(acc(id, name, kind, p ? p[1] : kind, { total: 2 * TB, used: 612 * GB, free: 1388 * GB }));
        mk(mount(name));
        push();
      }, 8000);
      return { id, url: st.signin.url, expiresAt: st.signin.expiresAt };
    },
    openSignInPage: () => {},
    cancelSignIn: (id) => { if (st.signin && st.signin.id === id) { st.signin = null; push(); } },
    add(kind, name, fields = {}) {
      const p = PROVIDERS.find((x) => x[0] === kind);
      if (!p) return { error: 'cloud.badKind' };
      // e2e (2026-11-06): an address that doesn't answer, then trusting an SSH server's key or a certificate
      const host = String(fields.host || fields.url || fields.endpoint || '');
      if (/unreachable|invalid/.test(host)) return { error: 'cloud.unreachable' };
      if (/changed/.test(host)) {
        pendingAdd = { kind, name, label: p[1] };
        // another account kept the same old key (N4): it is offered the new one too
        Object.assign(st.accounts[3], { error: 'cloud.hostKeyChanged', old: 'SHA256:3vVq2M3xk7Jm1o8R9pZcY6fJt2uQ0yLw5kHn4bXsE1A', new: 'SHA256:Qe7mT0c2Lw9sZb4kYh1VnR8pXuJ3dF6gAo5iCe2Mtz8' });
        push();
        return { question: { kind: 'hostKeyChanged', state: 'mock-trust', host, port: 22, old: 'SHA256:3vVq2M3xk7Jm1o8R9pZcY6fJt2uQ0yLw5kHn4bXsE1A', new: 'SHA256:Qe7mT0c2Lw9sZb4kYh1VnR8pXuJ3dF6gAo5iCe2Mtz8' } };
      }
      if (kind === 'sftp' || /^https:/.test(host) && /self/.test(host)) {
        pendingAdd = { kind, name, label: p[1] };
        return kind === 'sftp'
          ? { question: { kind: 'hostKey', state: 'mock-trust', host: host || 'nas.example.test', port: Number(fields.port) || 22, fingerprint: 'SHA256:3vVq2M3xk7Jm1o8R9pZcY6fJt2uQ0yLw5kHn4bXsE1A' } }
          : { question: { kind: 'certificate', state: 'mock-trust', host: host.replace(/^https:\/\//, '').split('/')[0], port: 443, fingerprint: 'SHA256:9F:1C:7A:44:0B:E2:58:31:CC:90:6D:12:AF:73:5E:08:B4:21:9A:6F:D0:3E:77:C5:1B:84:2A:E9:60:FD:13:57', subject: 'CN=cloud.example.test', issuer: 'CN=cloud.example.test' } };
      }
      // rclone's questions (17.2): the phone maker's drive asks for a code sent to a trusted device
      if (kind === 'iclouddrive') {
        pendingAdd = { kind, name, label: p[1] };
        return { question: { state: 'mock-2fa', name: 'config_2fa', help: 'Two-factor authentication: enter the 6-digit code sent to your trusted device.', type: 'string', examples: [], isPassword: false, required: true, default: '' } };
      }
      const id = `m${seq++}`;
      st.accounts.push(acc(id, name, kind, p[1], kind === 's3' || kind === 'b2' ? { used: 0 } : null, { linksSupported: false }));
      mk(mount(name));
      push();
      return { id };
    },
    answer(state, value) {
      if (state === 'mock-trust' && pendingAdd && value === 'trust') {
        const id = `m${seq++}`;
        st.accounts.push(acc(id, pendingAdd.name, pendingAdd.kind, pendingAdd.label, null, { linksSupported: false }));
        mk(mount(pendingAdd.name));
        pendingAdd = null;
        push();
        return Promise.resolve({ id });
      }
      if (state !== 'mock-2fa' || !pendingAdd) return Promise.resolve({ error: 'cloud.badState' });
      if (!/^\d{6}$/.test(value)) return Promise.resolve({ question: { state: 'mock-2fa', name: 'config_2fa', help: 'That code is not right. Enter the 6-digit code again.', type: 'string', required: true } });
      const id = `m${seq++}`;
      st.accounts.push(acc(id, pendingAdd.name, pendingAdd.kind, pendingAdd.label, { total: 50 * GB, used: 12 * GB, free: 38 * GB }));
      mk(mount(pendingAdd.name));
      pendingAdd = null;
      push();
      return new Promise((r) => setTimeout(() => r({ id }), 600));
    },
    emptyTrash(id) { const a = find(id); return new Promise((r) => setTimeout(() => { if (a && a.about) { a.about.used -= a.about.trashed || 0; a.about.trashed = 0; push(); } r(a ? { ok: true } : { error: 'cloud.notFound' }); }, 1500)); },
    measure(id) {
      const a = find(id);
      return new Promise((r) => { measureTimer = setTimeout(() => { if (a) { a.about = { ...(a.about || {}), used: 163.4 * GB, measuredAt: Date.now() }; push(); } r({ bytes: 163.4 * GB, count: 41213 }); }, 4000); });
    },
    cancelMeasure() { clearTimeout(measureTimer); },
    delete(paths) {
      for (const p of paths) { try { lord.fs.copy(p, `${home}/.cache/lordos-cloudmock-deleted-${seq++}`, true); } catch (e) { /* gone */ } delete status[p]; }
      push();
      return new Promise((r) => setTimeout(() => r({ ok: true }), 300));
    },
    remove(id, o) {
      const a = find(id);
      if (!a) return { error: 'cloud.badId' };
      if (a.up.files && !o.discardPending) return { error: 'pending', files: a.up.files };
      st.accounts = st.accounts.filter((x) => x.id !== id);
      push();
      return { ok: true };
    },
    rename(id, name) { const a = find(id); if (!a) return { error: 'cloud.badId' }; a.name = name; push(); return { ok: true }; },
    refreshAbout: () => push(),
    websiteUrl: (id) => (find(id) && find(id).kind !== 's3' ? 'https://www.example.test/' : null),
    status(paths) {
      const out = {};
      for (const p of paths) {
        if (!ofPath(p) || p === ofPath(p).mount) continue;
        // a file made here since (Check This Account's) is on this computer
        let made = false;
        if (!status[p]) { try { made = !lord.fs.stat(p).dir; } catch (e) { made = false; } }
        let s = status[p] || (keptIn(p) ? 'kept' : made ? 'here' : 'cloud');
        if (keptIn(p) && s !== 'up' && s !== 'error') s = 'kept';
        const a = ofPath(p);
        if (s === 'down') out[p] = { st: s, pct: a.down.bytes ? a.down.done / a.down.bytes : 0.6 };
        else if (s === 'up') out[p] = { st: s, pct: a.up.bytes ? a.up.done / a.up.bytes : 0.4 };
        else if (s === 'error') out[p] = { st: s, error: 'cloud.uploadFailed' };
        else out[p] = { st: s };
      }
      return out;
    },
    keep(paths) {
      for (const p of paths) { const a = ofPath(p); if (a && !a.kept.includes(p)) { a.kept.push(p); status[p] = 'kept'; } }
      push();
    },
    unkeep(paths) {
      for (const p of paths) { const a = ofPath(p); if (a) { a.kept = a.kept.filter((k) => k !== p); if (status[p] === 'kept') status[p] = 'here'; } }
      push();
    },
    free(paths) {
      let freed = 0;
      const skipped = [];
      for (const p of paths) {
        const s = status[p];
        if (keptIn(p)) { skipped.push({ path: p, reason: 'kept' }); continue; }
        if (s === 'up') { skipped.push({ path: p, reason: 'pending' }); continue; }
        if (s === 'here' || !s) { status[p] = 'cloud'; freed += 1e6; }
      }
      push();
      return { freed, skipped };
    },
    link(path, expire) {
      const a = ofPath(path);
      if (!a || !a.linksSupported) return { error: 'unsupported' };
      const list = links[a.id] || (links[a.id] = []);
      const days = { '1d': 1, '7d': 7, '30d': 30 }[expire];
      let l = list.find((x) => x.path === path);
      if (!l) { l = { path, url: `https://share.example.test/s/${Math.random().toString(36).slice(2, 12)}`, made: Date.now() / 1000 }; list.push(l); }
      l.expires = days ? (Date.now() + days * 864e5) / 1000 : 0;
      if (a.kind === 's3' && (!days || days > 7)) l.expires = (Date.now() + 7 * 864e5) / 1000;   // 7 days at most (17.2 item 29)
      push();
      return { url: l.url, expires: l.expires ? l.expires * 1000 : null };
    },
    unlink(path) {
      const a = ofPath(path);
      if (a && a.kind === 's3') return Promise.resolve({ error: 'cloud.cantRevoke', expires: Date.now() + 5 * 864e5 });
      if (a && links[a.id]) links[a.id] = links[a.id].filter((x) => x.path !== path); push(); return { ok: true }; },
    // a walk of the account's folder (17.2 item 31), names matched without case, a little later
    search(path, needle) {
      const out = [];
      const q = String(needle).toLowerCase();
      const walk = (d, depth) => {
        let list = [];
        try { list = lord.fs.list(d); } catch (e) { return; }
        for (const e of list) {
          const p = `${d}/${e.name}`;
          if (e.name.toLowerCase().includes(q)) out.push({ path: p, dir: !!e.dir, size: e.size || 0, mtime: e.mtime || 0 });
          if (e.dir && depth < 16) walk(p, depth + 1);
        }
      };
      walk(path, 0);
      return new Promise((r) => setTimeout(() => r({ hits: out.slice(0, 500), complete: out.length <= 500 }), 700));
    },
    links: (id) => JSON.parse(JSON.stringify(links[id] || [])),
    pause(id) { for (const a of st.accounts) if (!id || a.id === id) { a.prev = a.state === 'paused' ? a.prev : a.state; a.state = 'paused'; } push(); },
    resume(id) { for (const a of st.accounts) if ((!id || a.id === id) && a.state === 'paused') a.state = a.prev || 'ok'; push(); },
    setOptions(o) { Object.assign(st.options, o); push(); },
    setAccountOptions(id, o) { const a = find(id); if (a) { a.options = { ...(a.options || {}), ...o }; push(); } },
    retry(id) { for (const a of st.accounts) if ((!id || a.id === id) && a.state === 'locked') { a.state = 'ok'; delete a.error; a.about = { used: 1 }; } push(); },
    restart() { st.service.uptimeS = 0; st.service.restarts++; push(); },
    showLog: () => {},
  };
  push();
}
