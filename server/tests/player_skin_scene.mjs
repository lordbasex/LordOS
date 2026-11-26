// player_skin_scene.mjs — a scene of tests/menubar_test.mjs (not run by itself): the Player with its views, header and
// transport stubbed (tests/app_skin_helpers.js). select() opens only a listed view; nothing plays, so the transport offers
// no command.
import { mountStubbed, commonChecks, check, flush } from './app_skin_helpers.js';
import Player from '../../apps/player/index.jsx';

const st = await mountStubbed('player', Player);
const side = () => st.seen.sidebar;
check('player: the views', side() && side().items.map((x) => x.key).join() === 'songs,artists,albums,videos,queue,streams', JSON.stringify(side() && side().items.map((x) => x.key)));
side().actions.select('albums');
await flush();
check('player: select a view', side().items.find((x) => x.key === 'albums').selected && st.seen.toolbar.title === 'player.albums');
side().actions.select('video');
await flush();
check('player: the video view is not listed while nothing plays', side().items.find((x) => x.key === 'albums').selected);
check('player: no transport while nothing plays', !st.seen.transport);
await commonChecks('player', st, { absent: ['transport'] });
process.exit(0);
