// imager_skin_scene.mjs — a scene of tests/menubar_test.mjs (not run by itself): imager with the header a theme may
// add (sdk/appskins.jsx SkinHeader; docs/THEMES-GUIDE.md "App skins → the shared shapes") replaced by a stub
// (tests/app_skin_helpers.js): the stub gets the app's title, the app is still drawn under it, and without the skin
// nothing wraps it.
import { mountStubbed, commonChecks, check } from './app_skin_helpers.js';
import App from '../../apps/imager/index.jsx';

const st = await mountStubbed('imager', App);
const m = st.seen.header;
check('imager: the header stub gets the title', !!m && typeof m.title === 'string' && m.title.length > 0 && Array.isArray(m.commands), JSON.stringify(m && m.title));
await commonChecks('imager', st, { bar: 'header' });
process.exit(0);
