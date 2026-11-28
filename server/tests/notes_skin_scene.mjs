// notes_skin_scene.mjs — a scene of tests/menubar_test.mjs (not run by itself): Notes with both regions a theme may draw
// (sidebar, toolbar; docs/THEMES-GUIDE.md "App skins → Notes") replaced by stubs (tests/app_skin_helpers.js). The list
// pane carries titles and dates, never a note's text; select() opens only a listed note; the commands are Notes' own.
import { mountStubbed, commonChecks, check, flush } from './app_skin_helpers.js';
import { notesStore } from '../../apps/notes/store.js';
import Notes from '../../apps/notes/index.jsx';


const st = await mountStubbed('notes', Notes);
notesStore.set({ loaded: true, list: [
  { slug: 'a', title: 'Groceries', body: '# Groceries\nsecret text', color: 'yellow', updated: '2026-11-20T10:00:00Z' },
  { slug: 'b', title: 'Ideas', body: '# Ideas\nmore', color: 'blue', updated: '2026-11-21T10:00:00Z' },
] });
await flush();
const side = () => st.seen.sidebar;
check('notes: the list pane has both notes', side() && side().items.map((x) => x.label).sort().join() === 'Groceries,Ideas', JSON.stringify(side() && side().items));
check("notes: no note's text in the model", !JSON.stringify(side()).includes('secret'));
side().actions.select('a');
await flush();
check('notes: select opens the note', side().items.find((x) => x.key === 'a').selected && st.seen.toolbar && st.seen.toolbar.title === 'Groceries', JSON.stringify(st.seen.toolbar && st.seen.toolbar.title));
side().actions.select('nosuch');
await flush();
check('notes: an unknown note is refused', side().items.find((x) => x.key === 'a').selected);
st.seen.toolbar.actions.command('edit');
await flush();
check('notes: command edit', st.seen.toolbar.commands.find((c) => c.id === 'edit').active);
await commonChecks('notes', st, { own: 'notes.search' });
process.exit(0);
