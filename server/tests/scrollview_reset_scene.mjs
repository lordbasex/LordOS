// scrollview_reset_scene.mjs — the scene of tests/scrollview_reset_test.mjs (not run by itself): the SDK's
// ScrollView rendered through runtime/index.js with a stub `lord` (the banner of the bundle). A view
// 100 px high over 1000 px of content is scrolled to 600, then its resetKey changes. Prints one line
// per check: "ok <name>" or "FAIL <name> <detail>".
import React from 'react';
import * as rt from '@lordos/runtime';
import { ScrollView } from '../../sdk/components.jsx';

const { styles } = globalThis.__lordTest;
const h = React.createElement;
const flush = async () => { for (let i = 0; i < 20; i++) await new Promise((res) => setImmediate(res)); };

// the scrolled box is the one with a translateY style
const shift = () => {
  for (const s of styles.values()) if ('translateY' in s) return -s.translateY || 0;
  return null;
};
const check = (name, got, want) => console.log(got === want ? `ok ${name}` : `FAIL ${name} got ${got}, want ${want}`);

let setKey = null;
const ref = { current: null };
function Scene() {
  const [key, set] = React.useState('a');
  setKey = set;
  return h(rt.View, { style: { width: 300, height: 100 } },
    h(ScrollView, { scrollRef: ref, resetKey: key }, h(rt.View, { style: { height: 1000 } })));
}

rt.render(h(Scene));
await flush();
check('starts at the top', shift(), 0);
ref.current.scrollTo(600);
await flush();
check('scrollTo moves it', shift(), 600);
setKey('a');
await flush();
check('same resetKey keeps the position', shift(), 600);
setKey('b');
await flush();
check('a new resetKey goes back to the top', shift(), 0);
check('offset() agrees', ref.current.offset(), 0);
ref.current.scrollTo(300);
await flush();
check('it scrolls again after a reset', shift(), 300);
setKey('a');
await flush();
check('back to an earlier key also starts at the top', shift(), 0);
process.exit(0);
