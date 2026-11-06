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
  sftp: [F('host', 'Server', { required: true }), F('user', 'User'), F('port', 'Port', { default: '22' }), F('pass', 'Password', { secret: true })],
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
    up: { files: 0, bytes: 0, done: 0 }, down: { files: 0, bytes: 0, done: 0 }, kept: [], linksSupported: true, ...extra });
  const st = {
    service: { running: true, version: '1.75.2', uptimeS: 5400, rssKb: 41 * 1024, restarts: 0 },
    options: { bwUp: 0, bwDown: 0, pauseOnBattery: true, pauseOnMetered: true, autoFree: true, freeBelowGb: 10 },
    accounts: [
      acc('m1', 'Personal', 'dropbox', 'Dropbox', { total: 2 * TB, used: 612 * GB, free: 1388 * GB, trashed: 20 * GB }, { cacheBytes: 9.1 * GB, keptBytes: 2.1 * GB }),
      acc('m2', 'Work', 'pcloud', 'pCloud Business', { total: 1 * TB, used: 540 * GB, free: 460 * GB, trashed: 20 * GB, other: 30 * GB },
        { state: 'syncing', cacheBytes: 7.2 * GB, keptBytes: 4.1 * GB, up: { files: 1, bytes: 86e3, done: 34e3, name: 'Budget 2027.ods' }, down: { files: 2, bytes: 1.8 * GB, done: 1.15 * GB, name: 'Kickoff recording.mp4' } }),
      acc('m3', 'Photos archive', 's3', 'S3 · Backblaze B2', { used: 161 * GB }, { cacheBytes: 1.6 * GB, linksSupported: false }),
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
      st.conflicts = [{ path: `${w.mount}/Projects/Proposal v3.odt`, kept: `${w.mount}/Projects/Proposal v3 (this computer).odt`, account: w.id, at: Date.now() }];
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
    add(kind, name) {
      const p = PROVIDERS.find((x) => x[0] === kind);
      if (!p) return { error: 'cloud.badKind' };
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
      push();
      return { url: l.url };
    },
    unlink(path) { const a = ofPath(path); if (a && links[a.id]) links[a.id] = links[a.id].filter((x) => x.path !== path); push(); return { ok: true }; },
    links: (id) => JSON.parse(JSON.stringify(links[id] || [])),
    pause(id) { for (const a of st.accounts) if (!id || a.id === id) { a.prev = a.state === 'paused' ? a.prev : a.state; a.state = 'paused'; } push(); },
    resume(id) { for (const a of st.accounts) if ((!id || a.id === id) && a.state === 'paused') a.state = a.prev || 'ok'; push(); },
    setOptions(o) { Object.assign(st.options, o); push(); },
    setAccountOptions(id, o) { const a = find(id); if (a) { a.options = { ...(a.options || {}), ...o }; push(); } },
    restart() { st.service.uptimeS = 0; st.service.restarts++; push(); },
    showLog: () => {},
  };
  push();
}
