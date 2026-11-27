// help_skin_scene.mjs — a scene of tests/menubar_test.mjs (not run by itself): Help with its list of articles stubbed
// (tests/app_skin_helpers.js). select() opens only a listed article or the tour; the search narrows the list; a search with
// nothing found shows Lumo confused in Help's own sidebar (docs/LORDOS-BRAND.md, "Lumo poses"), once.
import { mountStubbed, commonChecks, check, flush, shown, settings } from './app_skin_helpers.js';
import Help from '../../apps/help/index.jsx';

const ARTS = [{ id: 'welcome', title: 'Welcome', section: 'start', order: 1 }, { id: 'files', title: 'Files', section: 'apps', app: 'files', order: 2 }];
globalThis.lord.help = { list: () => ARTS.map((a) => ({ ...a })), read: (lang, id) => `---\ntitle: ${id}\n---\nText of ${id}.\n`, search: () => [] };
// the image each node shows (the stub lord has no setProp), to find Lumo's pose
const srcs = new Map();
globalThis.lord.setProp = (id, k, v) => { if (k === 'src') { if (v) srcs.set(id, String(v)); else srcs.delete(id); } };
const { parents, removed, styles, texts } = globalThis.__lordTest;
const inTree = (id) => { for (let n = id; n; n = parents.get(n)) if (removed.has(n)) return false; return true; };
const lumos = (pose) => [...srcs].filter(([id, s]) => s.startsWith(`asset:mascot/lumo-${pose}-`) && inTree(id)).length;
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
// a search that finds nothing: the skin's list is empty; Help's own sidebar shows the empty state with Lumo confused
check('help: no Lumo before the search', lumos('confused') === 0);
m().actions.search('zzzz-nothing');
await flush();
check('help: a search with nothing found empties the list', m().items.length === 0, JSON.stringify(m().items));
settings.set({ themeParts: { layouts: false } });
await flush();
check('help: no results says so', shown('help.noResults'));
check('help: no results shows Lumo confused, once', lumos('confused') === 1, String(lumos('confused')));
// the title takes the sidebar's whole width (QA ws168: shrunk to its measured width it broke into one more line than
// measured, and "No se encontraron artículos." showed only "No se encontraron")
const titleNode = [...texts].filter(([id, v]) => v === 'help.noResults' && inTree(id)).map(([id]) => parents.get(id))[0];
const ts = titleNode && styles.get(titleNode);
check('help: the no-results title takes the sidebar\'s width', !!ts && ts.width === '100%' && ts.textAlign === 'center', JSON.stringify(ts));
process.exit(0);
