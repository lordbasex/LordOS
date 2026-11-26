// characters_skin_scene.mjs — a scene of tests/menubar_test.mjs (not run by itself): Characters with its tabs and groups
// stubbed (tests/app_skin_helpers.js). The tabs switch between emoji and symbols; select() opens only a listed group.
import { mountStubbed, commonChecks, check, flush } from './app_skin_helpers.js';
import Characters from '../../apps/characters/index.jsx';

const st = await mountStubbed('characters', Characters);
const bar = () => st.seen.toolbar, side = () => st.seen.sidebar;
check('characters: two tabs', bar() && bar().commands.map((c) => c.id).join() === 'emoji,symbols');
bar().actions.command('symbols');
await flush();
check('characters: the symbols tab and its blocks', bar().commands[1].active && side() && side().items.length > 3 && side().items[0].key === '0', JSON.stringify(side() && side().items.slice(0, 3)));
side().actions.select('2');
await flush();
check('characters: a block', side().items[2].selected);
side().actions.select('999');
await flush();
check('characters: a block not listed is refused', side().items[2].selected);
await commonChecks('characters', st, {});
process.exit(0);
