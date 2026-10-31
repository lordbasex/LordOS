// session_model_test.mjs — session restore's data rules (sdk/sessionmodel.js): what may enter the
// saved list, what a new session does with it, the launch plan and frames on changed screens.
//
//   node tests/session_model_test.mjs        (from server/; run_suites.sh runs it when node exists)
import path from 'node:path';
import { fileURLToPath } from 'node:url';

const here = path.dirname(fileURLToPath(import.meta.url));
const M = await import(path.join(here, '../../sdk/sessionmodel.js'));
const { cleanArgs, cleanFrame, cleanEntry, cleanList, restoreDecision, planRestore, fitFrame, MAX_WINDOWS } = M;

let n = 0, bad = 0;
const eq = (name, got, want) => {
  n++;
  const a = JSON.stringify(got), b = JSON.stringify(want);
  if (a !== b) { bad++; console.error(`FAIL ${name}: ${a} != ${b}`); }
};

const F = { x: 10, y: 20, w: 400, h: 300 };
const ZONES = ['left', 'right'];

// args: only the listed fields of the listed apps, never a nonce
eq('nonce stripped', cleanArgs('files', { path: '/home/u/Docs', nonce: 9 }), { path: '/home/u/Docs' });
eq('unlisted app', cleanArgs('store', { install: '/tmp/evil.deb' }), null);
eq('mail compose is no state', cleanArgs('mail', { compose: { to: 'x' } }), null);
eq('relative path', cleanArgs('editor', { path: 'a/b' }), null);
eq('non-string path', cleanArgs('editor', { path: { toString: 'x' } }), null);
eq('home path', cleanArgs('files', { path: '~/Docs' }), { path: '~/Docs' });
eq('section with spaces', cleanArgs('settings', { section: 'a b' }), null);
eq('section', cleanArgs('settings', { section: 'network', page: 'vpn' }), { section: 'network', page: 'vpn' });

// frames
eq('frame rounds', cleanFrame({ x: 1.4, y: 2.6, w: 300, h: 200, extra: 1 }), { x: 1, y: 3, w: 300, h: 200 });
eq('frame NaN', cleanFrame({ x: NaN, y: 0, w: 300, h: 200 }), null);
eq('frame string', cleanFrame({ x: '0', y: 0, w: 300, h: 200 }), null);
eq('frame tiny', cleanFrame({ x: 0, y: 0, w: 1, h: 1 }), null);
eq('frame huge', cleanFrame({ x: 0, y: 0, w: 100000, h: 200 }), null);

// entries read back: only the known fields survive
eq('typed ids', ['app:files', 'lpk:thing', 'desktop:org.gnome.Foo'].map((id) => cleanEntry({ id, f: F })?.id), ['app:files', 'lpk:thing', 'desktop:org.gnome.Foo']);
eq('window with no .desktop', cleanEntry({ id: 'linux:foo', f: F }), null);
eq('launcher', cleanEntry({ id: 'shell:launcher', f: F }), null);
eq('injected desktop id', cleanEntry({ id: 'desktop:../../bin/sh', f: F }), null);
eq('never: forcequit', cleanEntry({ id: 'app:forcequit', f: F }), null);
eq('never: appabout', cleanEntry({ id: 'app:appabout', f: F }), null);
eq('never: installer', cleanEntry({ id: 'app:installer', f: F }), null);
eq('never: sysprobe', cleanEntry({ id: 'app:sysprobe', f: F }), null);
eq('extra fields dropped', cleanEntry({ id: 'desktop:foo', f: F, exec: 'rm -rf ~', argv: ['x'], env: {}, pid: 3, title: 'secret', path: '/x', max: 1, snap: 'left' }, ZONES),
  { id: 'desktop:foo', f: F, max: 1, snap: 'left' });
eq('args never for Linux apps', cleanEntry({ id: 'desktop:files', f: F, args: { path: '/secret/doc' } }), { id: 'desktop:files', f: F });
eq('unknown zone', cleanEntry({ id: 'app:files', f: F, snap: 'nowhere' }, ZONES), { id: 'app:files', f: F });
eq('not an object', cleanEntry('app:files'), null);
eq('list not an array', cleanList({ a: 1 }), []);
eq('cap', cleanList(Array.from({ length: 40 }, () => ({ id: 'app:files', f: F }))).length, MAX_WINDOWS);
eq('cap keeps the front', cleanList(Array.from({ length: 13 }, (_, i) => ({ id: i === 12 ? 'app:notes' : 'app:files', f: F }))).at(-1).id, 'app:notes');

// the owner's table: clean → always, unclean → ask, everything closed → nothing, off → nothing
eq('clean always', restoreDecision('always', true, 2), 'run');
eq('clean ask', restoreDecision('ask', true, 2), 'ask');
eq('clean off', restoreDecision('off', true, 2), 'none');
eq('unclean always', restoreDecision('always', false, 2), 'ask');
eq('unclean ask', restoreDecision('ask', false, 2), 'ask');
eq('unclean off', restoreDecision('off', false, 2), 'none');
eq('nothing open', restoreDecision('always', true, 0), 'none');
eq('nothing open, unclean', restoreDecision('always', false, 0), 'none');
eq('bad mode', restoreDecision('sometimes', true, 2), 'none');

// the plan
const e = (id) => ({ id, f: F });
const ids = (s) => s.map((x) => [x.entry.id, x.launch]);
eq('in order', ids(planRestore([e('a'), e('b')], new Map())), [['a', true], ['b', true]]);
eq('already open is skipped', ids(planRestore([e('a'), e('a'), e('b')], new Map([['a', 1]]))), [['a', true], ['b', true]]);
eq('two launches', ids(planRestore([e('a'), e('a')], new Map())), [['a', true], ['a', true]]);
eq('one launch, two frames', ids(planRestore([e('a'), e('a')], new Map(), (id) => id === 'a')), [['a', true], ['a', false]]);

// frames on today's screens
const D = [{ x: 0, y: 0, w: 1280, h: 800 }];
const area = (d) => ({ x: d.x, y: d.y + 24, w: d.w, h: d.h - 80 });
eq('kept', fitFrame({ x: 100, y: 100, w: 400, h: 300 }, D, area), { x: 100, y: 100, w: 400, h: 300 });
eq('too big', fitFrame({ x: 0, y: 0, w: 4000, h: 3000 }, D, area), { x: 0, y: 24, w: 1280, h: 720 });
eq('partly off the edge', fitFrame({ x: 1000, y: 500, w: 400, h: 300 }, D, area), { x: 1000, y: 500, w: 400, h: 300 });
eq('too far right', fitFrame({ x: 1220, y: 100, w: 400, h: 300 }, D, area), { x: 1184, y: 100, w: 400, h: 300 });
eq('display gone', fitFrame({ x: 3000, y: 100, w: 400, h: 300 }, D, area), { x: 440, y: 234, w: 400, h: 300 });
eq('under the bar', fitFrame({ x: 10, y: 0, w: 400, h: 300 }, D, area), { x: 10, y: 24, w: 400, h: 300 });
eq('second display', fitFrame({ x: 1400, y: 100, w: 400, h: 300 }, [...D, { x: 1280, y: 0, w: 800, h: 600 }], area), { x: 1400, y: 100, w: 400, h: 300 });

console.log(bad ? `session-model: ${bad}/${n} failed` : `session-model: ${n} ok`);
process.exit(bad ? 1 : 0);
