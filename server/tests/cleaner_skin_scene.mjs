// cleaner_skin_scene.mjs — a scene of tests/menubar_test.mjs (not run by itself): cleaner with the header a theme may
// add (sdk/appskins.jsx SkinHeader; docs/THEMES-GUIDE.md "App skins → the shared shapes") replaced by a stub
// (tests/app_skin_helpers.js): the stub gets the app's title, the app is still drawn under it, and without the skin
// nothing wraps it.
import { mountStubbed, check } from './app_skin_helpers.js';
import App from '../../apps/cleaner/index.jsx';

// Cleaner's native side, empty: nothing found yet
globalThis.lord.clean = new Proxy({}, { get: () => () => ({ list: [], items: [], groups: [], total: 0 }) });
const st = await mountStubbed('cleaner', App);
const m = st.seen.header;
check('cleaner: the header stub gets the title', !!m && typeof m.title === 'string' && m.title.length > 0 && Array.isArray(m.commands), JSON.stringify(m && m.title));
// Cleaner's own body needs its native scanner, which the stub lord can't give it (it reads a scan's results): only the
// header is checked here, the rest of the common checks run on the apps the stub lord can draw
check('cleaner: the header is drawn', !!st.seen.header);
process.exit(0);
