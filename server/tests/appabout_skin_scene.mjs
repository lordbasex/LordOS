// appabout_skin_scene.mjs — a scene of tests/menubar_test.mjs (not run by itself): About App with its banner stubbed
// (tests/app_skin_helpers.js): the same banner shape as About's, for one app, with no action.
import { mountStubbed, commonChecks, check } from './app_skin_helpers.js';
import App from '../../apps/appabout/index.jsx';

const st = await mountStubbed('appabout', App, { args: { key: 'files' } });
const m = () => st.seen.banner;
check('appabout: the banner names the app', m() && m().name === 'files' && Object.keys(m().actions).length === 0, JSON.stringify(m() && m().name));
await commonChecks('appabout', st, { bar: 'banner' });
process.exit(0);
