// store_card_scene.mjs — a scene of tests/textfield_menu_test.mjs (not run by itself): a Store card (apps/store/ui.js)
// whose one-line texts are longer than the card. The kind line (category · developer, after the Flathub tag) and the
// foot's note end in an ellipsis instead of being cut through a letter (seen on the Flathub home's screenshots,
// tests/store_rankings_headless.sh, 2026-11-22: "Utilities · Ronnie Nis"), like the name above them. Prints "ok <name>"
// or "FAIL <name> <detail>".
import React from 'react';
import * as rt from '@lordos/runtime';
import { enableShellLayers } from '../../sdk/layers.js';
import { Card } from '../../apps/store/ui.js';

const { styles, parents, texts, removed } = globalThis.__lordTest;
const h = React.createElement;
const flush = async () => { for (let i = 0; i < 20; i++) await new Promise((res) => setImmediate(res)); };
const check = (name, ok, detail = '') => console.log(ok ? `ok ${name}` : `FAIL ${name} ${detail}`);

const NAME = 'An app with a rather long name indeed';
const KIND = 'Utilities · A developer whose name is much longer than the card';
const NOTE = 'A note far longer than the foot of the card can hold';
enableShellLayers();
rt.render(h(rt.View, { style: { width: 250 } },
  h(Card, { icon: null, name: NAME, tag: 'Flathub', kind: KIND, summary: 'Summary', note: NOTE }),
  h(Card, { icon: null, name: NAME, kind: KIND, summary: 'Summary', note: NOTE })));
await flush();

const inTree = (id) => { for (let n = id; n; n = parents.get(n)) if (removed.has(n)) return false; return true; };
// the style of the Text that shows s (the raw text's own style, or its parent's)
const stylesOf = (s) => [...texts].filter(([id, x]) => x === s && inTree(id)).map(([id]) => (styles.get(id) && styles.get(id).whiteSpace ? styles.get(id) : styles.get(parents.get(id)) || {}));
for (const [what, s, n] of [['the name', NAME, 2], ['the kind line, after the tag and without it', KIND, 2], ['the note', NOTE, 2]]) {
  const st = stylesOf(s);
  check(`${what}: shown`, st.length === n, JSON.stringify([...texts.values()]));
  check(`${what}: one line that ends in an ellipsis`, st.length > 0 && st.every((x) => x.whiteSpace === 'nowrap' && x.overflow === 'hidden' && x.textOverflow === 'ellipsis'),
    JSON.stringify(st));
}
process.exit(0);
