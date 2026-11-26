// monitor_skin_scene.mjs — a scene of tests/menubar_test.mjs (not run by itself): Monitor with its tabs stubbed
// (tests/app_skin_helpers.js). command() opens only a tab Monitor offers on this computer.
import { mountStubbed, commonChecks, check, flush } from './app_skin_helpers.js';
import Monitor from '../../apps/monitor/index.jsx';

globalThis.lord.monitor = new Proxy({ available: () => true }, { get: (o, k) => (k in o ? o[k] : () => undefined) });
const st = await mountStubbed('monitor', Monitor);
const m = () => st.seen.toolbar;
check('monitor: the tabs', m() && m().commands.some((c) => c.id === 'processes') && m().commands.find((c) => c.active), JSON.stringify(m() && m().commands.map((c) => c.id)));
m().actions.command('memory');
await flush();
check('monitor: a tab', m().commands.find((c) => c.id === 'memory').active);
m().actions.command('battery');
await flush();
check("monitor: a tab this computer doesn't offer is refused", m().commands.find((c) => c.id === 'memory').active);
await commonChecks('monitor', st, {});
process.exit(0);
