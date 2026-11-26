// clock_skin_scene.mjs — a scene of tests/menubar_test.mjs (not run by itself): the Clock with its toolbar stubbed
// (tests/app_skin_helpers.js): its four tabs and Add, the one shown active.
import { mountStubbed, commonChecks, check, flush } from './app_skin_helpers.js';
import Clock from '../../apps/clock/index.jsx';

const st = await mountStubbed('clock', Clock);
const m = () => st.seen.toolbar;
check('clock: the tabs', m() && ['world', 'alarms', 'timer', 'stopwatch'].every((id) => m().commands.some((c) => c.id === id)), JSON.stringify(m() && m().commands.map((c) => c.id)));
m().actions.command('stopwatch');
await flush();
check('clock: a tab, and no Add there', m().commands.find((c) => c.id === 'stopwatch').active && !m().commands.some((c) => c.id === 'add'));
m().actions.command('add');
await flush();
check('clock: Add is refused where the Clock has none', m().commands.find((c) => c.id === 'stopwatch').active);
await commonChecks('clock', st, { own: 'clock.tab.world' });
process.exit(0);
