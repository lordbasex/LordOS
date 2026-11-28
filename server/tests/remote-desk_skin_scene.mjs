// remote-desk_skin_scene.mjs — a scene of tests/menubar_test.mjs (not run by itself): remote-desk with the header a theme may
// add (sdk/appskins.jsx SkinHeader; docs/THEMES-GUIDE.md "App skins → the shared shapes") replaced by a stub
// (tests/app_skin_helpers.js): the stub gets the app's title, the app is still drawn under it, and without the skin
// nothing wraps it.
import { mountStubbed, commonChecks, check } from './app_skin_helpers.js';
import App from '../../apps/remote-desk/index.jsx';

const st = await mountStubbed('remote-desk', App);
const m = st.seen.header;
check('remote-desk: the header stub gets the title', !!m && typeof m.title === 'string' && m.title.length > 0 && Array.isArray(m.commands), JSON.stringify(m && m.title));
await commonChecks('remote-desk', st, { bar: 'header' });
process.exit(0);
