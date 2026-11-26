// help_skin_scene.mjs — a scene of tests/menubar_test.mjs (not run by itself): Help with its list of articles stubbed
// (tests/app_skin_helpers.js). select() opens only a listed article or the tour; the search narrows the list.
import { mountStubbed, commonChecks, check, flush } from './app_skin_helpers.js';
import Help from '../../apps/help/index.jsx';

const ARTS = [{ id: 'welcome', title: 'Welcome', section: 'start', order: 1 }, { id: 'files', title: 'Files', section: 'apps', app: 'files', order: 2 }];
globalThis.lord.help = { list: () => ARTS.map((a) => ({ ...a })), read: (lang, id) => `---\ntitle: ${id}\n---\nText of ${id}.\n`, search: () => [] };
const st = await mountStubbed('help', Help);
const m = () => st.seen.sidebar;
check('help: the tour and the articles', m() && m().items[0].key === '@tour' && m().items.some((x) => x.key === 'files'), JSON.stringify(m() && m().items.map((x) => x.key)));
m().actions.select('files');
await flush();
check('help: an article', m().items.find((x) => x.key === 'files').selected);
m().actions.select('../../etc/passwd');
await flush();
check('help: an article not listed is refused', m().items.find((x) => x.key === 'files').selected);
m().actions.select('@tour');
await flush();
check('help: the tour', m().items[0].selected);
await commonChecks('help', st, { bar: 'sidebar' });
process.exit(0);
