// photos_skin_scene.mjs — a scene of tests/menubar_test.mjs (not run by itself): Photos with its places and header stubbed
// (tests/app_skin_helpers.js). select() opens only a listed place; the commands are Photos' own (smaller, larger, refresh).
import { mountStubbed, commonChecks, check, flush } from './app_skin_helpers.js';
import Photos from '../../apps/photos/index.jsx';

const st = await mountStubbed('photos', Photos);
const side = () => st.seen.sidebar;
check('photos: All Photos is listed and selected', side() && side().items[0].label === 'photos.allPhotos' && side().items[0].selected, JSON.stringify(side() && side().items));
check('photos: the header commands', st.seen.toolbar && st.seen.toolbar.commands.map((c) => c.id).join() === 'smaller,larger,refresh');
st.seen.toolbar.actions.command('larger');
await flush();
check('photos: larger, by Photos', st.seen.toolbar.commands.find((c) => c.id === 'smaller').disabled === false);
side().actions.select('dir:/etc');
await flush();
check('photos: a place not listed is refused', side().items[0].selected);
await commonChecks('photos', st, { own: 'photos.folders' });
process.exit(0);
