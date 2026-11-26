// logs_skin_scene.mjs — a scene of tests/menubar_test.mjs (not run by itself): Logs with its sources and toolbar stubbed
// (tests/app_skin_helpers.js). select() opens only a listed source; Live is Logs' own switch.
import { mountStubbed, commonChecks, check, flush } from './app_skin_helpers.js';
import Logs from '../../apps/logs/index.jsx';

const st = await mountStubbed('logs', Logs);
const side = () => st.seen.sidebar, bar = () => st.seen.toolbar;
check('logs: the journal sources', side() && ['j:user', 'j:system', 'j:kernel'].every((k) => side().items.some((x) => x.key === k)), JSON.stringify(side() && side().items.map((x) => x.key)));
side().actions.select('j:kernel');
await flush();
check('logs: a source', side().items.find((x) => x.key === 'j:kernel').selected);
side().actions.select('f:../../etc/shadow');
await flush();
check('logs: a file not listed is refused', side().items.find((x) => x.key === 'j:kernel').selected);
const was = bar().commands.find((c) => c.id === 'live').active;
bar().actions.command('live');
await flush();
check('logs: Live switches', bar().commands.find((c) => c.id === 'live').active !== was);
await commonChecks('logs', st, {});
process.exit(0);
