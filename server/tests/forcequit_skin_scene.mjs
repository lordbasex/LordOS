// forcequit_skin_scene.mjs — a scene of tests/menubar_test.mjs (not run by itself): Force Quit with its list stubbed
// (tests/app_skin_helpers.js). Force Quit stays disabled until a listed app is chosen; quitting is Force Quit's own.
import { mountStubbed, commonChecks, check, flush } from './app_skin_helpers.js';
import App from '../../apps/forcequit/index.jsx';

const st = await mountStubbed('forcequit', App);
const m = () => st.seen.list;
check('forcequit: the list and Force Quit, disabled', m() && Array.isArray(m().items) && m().commands[0].id === 'quit' && m().commands[0].disabled);
m().actions.select('nosuchapp');
await flush();
check('forcequit: an app not listed is refused', !m().items.some((x) => x.selected) && m().commands[0].disabled);
await commonChecks('forcequit', st, { bar: 'list' });
process.exit(0);
