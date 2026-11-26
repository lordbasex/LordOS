// sysinfo_skin_scene.mjs — a scene of tests/menubar_test.mjs (not run by itself): System Information with its tabs stubbed
// (tests/app_skin_helpers.js): the six tabs and Refresh; only a listed tab opens.
import { mountStubbed, commonChecks, check, flush } from './app_skin_helpers.js';
import App from '../../apps/sysinfo/index.jsx';

globalThis.lord.system = new Proxy({ info: () => ({ user: 'demo', hostname: 'demo-pc', os: 'Debian 13', kernel: '6.12', arch: 'x86_64', cpu: 'Demo CPU', cores: 4, memTotalKB: 8e6, memAvailableKB: 4e6, uptime: 60 }),
}, { get: (o, k) => (k in o ? o[k] : () => undefined) });
globalThis.lord.sysinfo = new Proxy({ report: () => ({ cpu: { model: 'Demo CPU' }, gpus: [], disks: [], net: [], checks: [], usb: [], pci: [] }) }, { get: (o, k) => (k in o ? o[k] : () => undefined) });
const st = await mountStubbed('sysinfo', App);
const m = () => st.seen.toolbar;
if (!m()) { check('sysinfo: the report is there under the stub lord', false, 'systemReport() gave nothing'); process.exit(0); }
check('sysinfo: the tabs and Refresh', m().commands.map((c) => c.id).join() === 'overview,hardware,compat,checks,perf,report,refresh');
m().actions.command('checks');
await flush();
check('sysinfo: a tab', m().commands.find((c) => c.id === 'checks').active);
await commonChecks('sysinfo', st, {});
process.exit(0);
