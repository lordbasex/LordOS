// store_subscribers_test.mjs — sdk/store.js: a subscriber that throws doesn't keep the others from hearing a change
// (QA ws174: Quick Settings' Dark, Transparency and accent set from a community theme showed only on the next render,
// because one subscriber's native call was refused on the theme's stack), and agentInput (sdk/adminsyncmodel.js)
// counts a refused lord.hands.injecting as "not injecting".
//
//   node tests/store_subscribers_test.mjs        (from server/; run_suites.sh runs it when node exists)
import path from 'node:path';
import { fileURLToPath } from 'node:url';

const here = path.dirname(fileURLToPath(import.meta.url));
const { createStore } = await import(path.join(here, '../../sdk/store.js'));
const { agentInput } = await import(path.join(here, '../../sdk/adminsyncmodel.js'));

let n = 0, bad = 0;
const eq = (name, got, want) => {
  n++;
  const g = JSON.stringify(got), w = JSON.stringify(want);
  if (g !== w) { bad++; console.log(`✗ ${name}: want ${w}, got ${g}`); }
};

const warn = console.warn; const logged = []; console.warn = (m) => logged.push(String(m));
const s = createStore({ dark: false });
const heard = [];
s.subscribe(() => heard.push('a'));
s.subscribe(() => { throw new Error('refused on the theme stack'); });
s.subscribe(() => heard.push('c'));
let threw = false;
try { s.set({ dark: true }); } catch (e) { threw = true; }
eq('set does not throw', threw, false);
eq('state changed', s.get(), { dark: true });
eq('every other subscriber heard it', heard, ['a', 'c']);
eq('the failure is reported once per set', logged.filter((l) => l.includes('subscriber failed')).length, 1);
s.set({ dark: true });
console.warn = warn;
eq('an equal state still notifies (new object)', heard.length, 4);

globalThis.lord = { hands: { injecting: () => { throw new Error('refused'); } } };
eq('agentInput: refused is not injecting', agentInput(), false);
globalThis.lord = { hands: { injecting: () => true } };
eq('agentInput: injecting', agentInput(), true);
globalThis.lord = {};
eq('agentInput: no Hands', agentInput(), false);

console.log(bad ? `store-subscribers: ${bad}/${n} failed` : `store-subscribers: ${n} ok`);
process.exit(bad ? 1 : 0);
