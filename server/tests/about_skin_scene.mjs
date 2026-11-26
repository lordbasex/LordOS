// about_skin_scene.mjs — a scene of tests/menubar_test.mjs (not run by itself): About with its banner stubbed
// (tests/app_skin_helpers.js): the version, the verified-code state and the facts as rows; one action, the system report.
import { mountStubbed, commonChecks, check } from './app_skin_helpers.js';
import About from '../../apps/about/index.jsx';

globalThis.lord.system = new Proxy({ info: () => ({ user: 'demo', hostname: 'demo-pc', os: 'Debian GNU/Linux 13', kernel: '6.12', arch: 'x86_64', cpu: 'Demo CPU', cores: 4, memTotalKB: 8e6, memAvailableKB: 4e6, uptime: 600, version: '0.8.9.6.2' }), cpuTimes: () => [100, 50] },
  { get: (o, k) => (k in o ? o[k] : () => undefined) });
const st = await mountStubbed('about', About);
const m = () => st.seen.banner;
check('about: the version and the code state', m() && m().version && m().code, JSON.stringify(m() && { v: m().version, c: m().code }));
check('about: rows of label and value', m().rows.length >= 5 && m().rows.every((r) => Object.keys(r).join() === 'label,value'));
check('about: one action', JSON.stringify(Object.keys(m().actions)) === '["systemReport"]');
await commonChecks('about', st, { bar: 'banner' });
process.exit(0);
