// utf8_test.mjs — sdk/utf8.js against node's TextEncoder/TextDecoder, and no TextEncoder/TextDecoder left in the
// code the desktop runs (QuickJS has neither).
import { readFileSync, readdirSync, statSync } from 'node:fs';
import { join } from 'node:path';
import { utf8Encode, utf8Decode, utf8Length } from '../../sdk/utf8.js';

let fails = 0;
const check = (what, ok) => { if (!ok) { fails++; console.log('FAIL', what); } };
const samples = ['', 'a', 'ñandú', '日本語', 'emoji 👍🏽 and flag 🇦🇷', 'זה עברית 123', '\u0000x', 'a'.repeat(1000)];
for (const s of samples) {
  const want = new TextEncoder().encode(s);
  const got = utf8Encode(s);
  check(`encode ${JSON.stringify(s).slice(0, 20)}`, got.length === want.length && got.every((v, i) => v === want[i]));
  check(`length ${JSON.stringify(s).slice(0, 20)}`, utf8Length(s) === want.length);
  check(`decode ${JSON.stringify(s).slice(0, 20)}`, utf8Decode(want) === s);
}
for (const bad of [[0xff], [0xc0, 0x80], [0xe0, 0x80, 0x80], [0xed, 0xa0, 0x80], [0xf4, 0x90, 0x80, 0x80], [0xe2, 0x82], [0x80], [0x61, 0xc3]]) {
  check(`malformed ${bad}`, utf8Decode(new Uint8Array(bad)) === new TextDecoder().decode(new Uint8Array(bad)));
}
// the desktop's own code must not reach for the browser globals QuickJS doesn't have
const root = new URL('../../', import.meta.url).pathname;
const offenders = [];
const walk = (d) => {
  for (const f of readdirSync(d)) {
    const p = join(d, f);
    if (f === 'node_modules' || f.endsWith('.bundle.js') || f === 'tests') continue;
    if (statSync(p).isDirectory()) walk(p);
    else if (/\.(js|jsx|mjs)$/.test(f) && /\bnew Text(En|De)coder\b/.test(readFileSync(p, 'utf8'))) offenders.push(p.slice(root.length));
  }
};
for (const d of ['sdk', 'apps', 'runtime', 'themes']) walk(join(root, d));
check(`no TextEncoder/TextDecoder in desktop code: ${offenders.join(', ')}`, offenders.length === 0);
console.log(fails ? `utf8-test: ${fails} failed` : 'utf8-test: ok');
process.exit(fails ? 1 : 0);
