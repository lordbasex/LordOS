// fonts_skin_scene.mjs — a scene of tests/menubar_test.mjs (not run by itself): Fonts with its collections and families
// stubbed (tests/app_skin_helpers.js). select() opens only a listed collection or family; installing stays Fonts'.
import { mountStubbed, commonChecks, check, flush } from './app_skin_helpers.js';
import Fonts from '../../apps/fonts/index.jsx';

const F = (family, style, mono) => ({ family, style, ps: `${family}-${style}`, weight: 400, italic: false, mono, color: false, path: `/usr/share/fonts/${family}.ttf`, index: 0 });
globalThis.lord.fonts = { list: () => [F('Demo Sans', 'Regular', false), F('Demo Mono', 'Regular', true)] };
const st = await mountStubbed('fonts', Fonts);
const m = () => st.seen.sidebar;
check('fonts: collections and families', m() && m().items.some((x) => x.key === 'c:mono') && m().items.some((x) => x.key === 'f:Demo Sans'), JSON.stringify(m() && m().items.map((x) => x.key)));
m().actions.select('c:mono');
await flush();
check('fonts: a collection narrows the families', !m().items.some((x) => x.key === 'f:Demo Sans') && m().items.some((x) => x.key === 'f:Demo Mono'));
m().actions.select('f:Demo Mono');
await flush();
check('fonts: a family', m().items.find((x) => x.key === 'f:Demo Mono').selected);
m().actions.select('f:Not Installed');
await flush();
check('fonts: a family not listed is refused', m().items.find((x) => x.key === 'f:Demo Mono').selected);
await commonChecks('fonts', st, { bar: 'sidebar' });
process.exit(0);
