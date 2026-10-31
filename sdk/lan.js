// lan.js — Files → Network (docs/FILES.md, "Network"). lord.lan runs one bounded pass over the local
// network, off until the page opens; JS names no address, port or socket there. Mounting a share is
// the closed share-mount action: everything in it is validated again as root.
//
//   lan.available()                              a build with lord.lan
//   lan.start(asked) / lan.stop()                start(): false while one runs or the last is recent
//   lan.poll()                                   { state, running, probed, total, hosts: [...] }
//   lan.mount({ kind, addr, share, user, domain, password })    → the folder it was mounted on
//   lan.unmount(path)
//   lan.saved(kind, addr, share) / lan.remember(...) / lan.forget(...)      the password, in the keyring only
import { admin } from './admin.js';
import { keyring } from './vault.js';

const OWNER = 'files';
const service = (kind, addr, share) => `${kind}://${addr}/${share}`;

export const lan = {
  available: () => typeof lord !== 'undefined' && !!lord.lan,
  start: (asked = false) => lord.lan.start(asked),
  stop: () => lord.lan.stop(),
  poll: () => { try { return JSON.parse(lord.lan.poll()); } catch (e) { return { state: 'idle', running: false, probed: 0, total: 0, hosts: [] }; } },
  mount: ({ kind, addr, share, user = '', domain = '', password = '' }) => admin.run('share-mount', [kind, addr, share, user, domain, password].join('\n'), null, { quiet: true }).then((p) => String(p || '').trim()),
  unmount: (path) => admin.run('share-unmount', path, null, { quiet: true }),
  // what was remembered for this share: { user, password }, or null (no keyring, locked, nothing saved)
  async saved(kind, addr, share) {
    try {
      const it = (await keyring.list(OWNER)).find((x) => x.service === service(kind, addr, share));
      if (!it) return null;
      const full = await keyring.get(OWNER, { service: it.service, account: it.account });
      return full ? { user: it.account, password: full.secret } : null;
    } catch (e) { return null; }
  },
  remember: (kind, addr, share, user, password, label) => keyring.set(OWNER, { service: service(kind, addr, share), account: user, secret: password, label }),
  forget: (kind, addr, share, user) => keyring.remove(OWNER, { service: service(kind, addr, share), account: user }).catch(() => {}),
};
