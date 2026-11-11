// theme_portal_scene.mjs — a scene of tests/textfield_menu_test.mjs (not run by itself): what Linux apps
// (org.freedesktop.portal.Settings, lord.portal.set) and isolated apps (lord.broker.publish 'theme') are told
// about the desktop's colors comes only from the theme's own ThemeProvider. A nested one, as Notes' light paper
// inside a dark desktop, colors only its subtree: before, every render sent its light mode and then the theme's
// dark one, so the portal's color scheme went back and forth and Chrome's profile picker, which follows it,
// flickered. Prints one line per check: "ok <name>" or "FAIL <name> <detail>".
import React, { useState } from 'react';
import * as rt from '@lordos/runtime';
import { ThemeProvider, useTheme } from '../../sdk/theme.js';

const h = React.createElement;
const flush = async () => { for (let i = 0; i < 20; i++) await new Promise((res) => setImmediate(res)); };
const check = (name, ok, detail = '') => console.log(ok ? `ok ${name}` : `FAIL ${name} ${detail}`);

const sent = [], published = [];
lord.portal = { set: (m) => { sent.push(m); } };
lord.broker = { publish: (topic, json) => { if (topic === 'theme') published.push(JSON.parse(json)); } };

let seen = null, bumpPaper = null, bumpDesk = null;
// a Notes-like window: its own light tokens for the note under a dark desktop, re-rendered on its own (typing, hover)
function Paper() {
  const [n, setN] = useState(0);
  bumpPaper = () => setN(n + 1);
  return h(ThemeProvider, { tokens: { ...useTheme(), mode: 'light', accent: '#b8860b' } }, h(Inner, { n }));
}
function Inner() { seen = useTheme(); return h(rt.View, { style: { width: 10, height: 10 } }); }
function Desktop() {
  const [n, setN] = useState(0);
  bumpDesk = () => setN(n + 1);
  return h(ThemeProvider, { tokens: { mode: 'dark', accent: '#0a84ff', tick: n } }, h(Paper));
}

rt.render(h(Desktop));
await flush();
check('the nested provider colors its subtree', seen && seen.mode === 'light' && seen.accent === '#b8860b', JSON.stringify(seen && seen.mode));
check('the portal is told the desktop\'s dark mode once', sent.length === 1 && sent[0].dark === true && sent[0].accent === '#0a84ff', JSON.stringify(sent));
for (let i = 0; i < 3; i++) { bumpPaper(); await flush(); bumpDesk(); await flush(); }
check('renders of either provider never send the paper\'s light mode', sent.every((m) => m.dark === true), JSON.stringify(sent.map((m) => m.dark)));
check('nor send the same colors again', sent.length === 1, `${sent.length} sends`);
check('isolated apps get only the desktop\'s tokens', published.length > 0 && published.every((t) => t.mode === 'dark'), JSON.stringify(published.map((t) => t.mode)));
