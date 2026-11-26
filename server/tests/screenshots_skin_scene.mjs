// screenshots_skin_scene.mjs — a scene of tests/menubar_test.mjs (not run by itself): Screenshots' capture bar stubbed
// (tests/app_skin_helpers.js): close, the five modes, the color picker, text and Capture; a mode is chosen by Screenshots.
import { mountStubbed, commonChecks, check, flush, settings } from './app_skin_helpers.js';
import Screenshots from '../../apps/screenshots/index.jsx';

const st = await mountStubbed('screenshots', Screenshots, { id: 'screenshots' });
const m = () => st.seen.toolbar;
check('screenshots: the bar', m() && m().commands.map((c) => c.id).join() === 'close,full,window,area,recordFull,recordArea,color,text,primary', JSON.stringify(m() && m().commands.map((c) => c.id)));
m().actions.command('window');
await flush();
check('screenshots: a mode, kept in the options', (settings.get().screenshots || {}).mode === 'window' && m().commands.find((c) => c.id === 'window').active);
m().actions.command('recordArea');
await flush();
check('screenshots: a recording mode turns Capture into Record', m().commands.find((c) => c.id === 'primary').label === 'screenshots.record');
await commonChecks('screenshots', st, {});
process.exit(0);
