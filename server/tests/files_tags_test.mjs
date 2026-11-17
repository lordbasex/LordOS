// files_tags_test.mjs — Files' color tags and comments (apps/files/tags.js): the last tag can be removed (GitHub #181).
import { toggledTags, withComment, movedKey } from '../../apps/files/tags.js';

let ok = 0, bad = 0;
const eq = (label, got, want) => {
  if (JSON.stringify(got) === JSON.stringify(want)) ok++;
  else { bad++; console.log(`FAIL ${label}: ${JSON.stringify(got)} != ${JSON.stringify(want)}`); }
};

// a store that merges an object into the old state, as the SDK's createStore does: the maps must be set
// whole (set(() => map)), or a removed path comes back
const store = (state) => ({ get: () => state, set(p) { state = typeof p === 'function' ? p(state) : { ...state, ...p }; } });

const a = '/home/u/a.txt', b = '/home/u/b.txt';
let m = toggledTags({}, [a], 'red');
eq('add one', m, { [a]: ['red'] });
m = toggledTags(m, [a], 'red');
eq('remove the only tag: the path goes', m, {});
m = toggledTags(toggledTags({}, [a], 'red'), [a], 'blue');
eq('two tags', m, { [a]: ['red', 'blue'] });
m = toggledTags(m, [a], 'red');
eq('remove one of two', m, { [a]: ['blue'] });
m = toggledTags(m, [a], 'blue');
eq('remove the last one', m, {});
const s = store({ [a]: ['blue'] });
const next = toggledTags(s.get(), [a], 'blue');
s.set(() => next);
eq('set whole: the last tag stays removed', s.get(), {});
const merged = store({ [a]: ['blue'] });
merged.set(next);
eq('(set merged would bring it back)', merged.get(), { [a]: ['blue'] });

// several paths: on only when every one has it
m = toggledTags({ [a]: ['red'] }, [a, b], 'red');
eq('mixed: adds to all', m, { [a]: ['red'], [b]: ['red'] });
m = toggledTags(m, [a, b], 'red');
eq('all have it: removed from all', m, {});
eq('the input is not changed', (() => { const o = { [a]: ['red'] }; toggledTags(o, [a], 'red'); return o; })(), { [a]: ['red'] });

// comments
eq('comment set', withComment({}, a, 'hi'), { [a]: 'hi' });
eq('comment cleared', withComment({ [a]: 'hi' }, a, '  '), {});
eq('comment bounded', withComment({}, a, 'x'.repeat(600))[a].length, 500);

// moves
eq('move', movedKey({ [a]: ['red'] }, a, b), { [b]: ['red'] });
const same = { [b]: ['red'] };
eq('move of an untagged path: unchanged', movedKey(same, a, '/x') === same, true);
eq('move onto itself: unchanged', movedKey(same, b, b) === same, true);

console.log(`files-tags-test: ${ok} ok, ${bad} failed`);
process.exit(bad ? 1 : 0);
