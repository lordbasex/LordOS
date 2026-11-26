// Celeste's geometry, hover edges and launcher calculator (themes/celeste/model.js), and its theme.json options as
// Settings reads them (sdk/thememeta.js). node server/tests/celeste_model_test.mjs
import fs from 'node:fs';
import { FRAME, BAR_W, GAP, F, frameGeom, workArea, drawerRect, hiddenOffset, hoverZone, DWELL, outside, calc } from '../../themes/celeste/model.js';
import { cleanThemeMeta } from '../../sdk/thememeta.js';

let ok = 0;
const eq = (name, a, b) => { if (JSON.stringify(a) !== JSON.stringify(b)) { console.log(`FAIL ${name}: ${JSON.stringify(a)} != ${JSON.stringify(b)}`); process.exitCode = 1; } else ok++; };

const d = { id: 1, x: 0, y: 0, w: 1920, h: 1080 };
const d2 = { id: 2, x: 1920, y: 0, w: 1280, h: 1024 };

// the frame: the bar on the left, mirrored to the right in right-to-left languages
eq('ltr bar', frameGeom(d, false).bar, { x: 0, y: FRAME, w: BAR_W, h: 1080 - 2 * FRAME });
eq('ltr inner', frameGeom(d, false).inner, { x: BAR_W, y: FRAME, w: 1920 - BAR_W - FRAME, h: 1080 - 2 * FRAME });
eq('rtl bar', frameGeom(d, true).bar.x, 1920 - BAR_W);
eq('rtl inner', frameGeom(d, true).inner.x, FRAME);
eq('second display', frameGeom(d2, false).bar.x, 1920);
eq('no display', frameGeom(null, false), null);

// windows go inside the frame with a gap; a display without a frame keeps all of it
eq('work area', workArea(d, true, false), { x: BAR_W + GAP, y: FRAME + GAP, w: 1920 - BAR_W - FRAME - 2 * GAP, h: 1080 - 2 * FRAME - 2 * GAP });
eq('work area rtl', workArea(d, true, true).x, FRAME + GAP);
eq('no frame', workArea(d2, false, false), { x: 1920, y: 0, w: 1280, h: 1024 });

// drawers: dashboard top centre, launcher bottom centre, session and sidebar at the end edge
const inner = frameGeom(d, false).inner;
const dash = drawerRect('dash', inner, false);
eq('dash edge', [dash.edge, dash.y, dash.x + dash.w / 2], ['top', inner.y, inner.x + inner.w / 2]);
const l = drawerRect('launcher', inner, false, 200);
eq('launcher bottom', [l.edge, l.y + l.h], ['bottom', inner.y + inner.h]);
eq('session at the end', drawerRect('session', inner, false).x + drawerRect('session', inner, false).w, inner.x + inner.w);
const innerR = frameGeom(d, true).inner;
eq('session rtl at the start', drawerRect('session', innerR, true).x, innerR.x);
eq('sidebar rtl', drawerRect('sidebar', innerR, true).x, innerR.x);
eq('unknown drawer', drawerRect('nope', inner, false), null);
eq('narrow display', drawerRect('dash', { x: 0, y: 0, w: 400, h: 300 }, false).w <= 400 - 2 * F, true);
eq('hidden top', hiddenOffset(dash, false), { x: 0, y: -(dash.h + F) });
eq('hidden end ltr', hiddenOffset(drawerRect('sidebar', inner, false), false).x > 0, true);
eq('hidden end rtl', hiddenOffset(drawerRect('sidebar', innerR, true), true).x < 0, true);

// hover edges: only pushed against the display's edge, near their spot
const cx = inner.x + inner.w / 2, cy = inner.y + inner.h / 2;
eq('top centre', hoverZone(cx, 0, d, false), 'dash');
eq('top, off centre', hoverZone(inner.x + 10, 0, d, false), null);
eq('a pixel below the top', hoverZone(cx, 3, d, false), null);
eq('bottom centre', hoverZone(cx, 1079, d, false), 'launcher');
eq('right middle', hoverZone(1919, cy, d, false), 'session');
eq('right, inside the frame only', hoverZone(1915, cy, d, false), null);
eq('right top', hoverZone(1919, inner.y + 40, d, false), 'sidebar');
eq('rtl: left middle', hoverZone(0, cy, d, true), 'session');
eq('rtl: right middle is the bar', hoverZone(1919, cy, d, true), null);
eq('another display', hoverZone(cx, 0, d2, false), null);
eq('session waits longest', Math.max(...Object.values(DWELL)), DWELL.session);
eq('outside', [outside(0, 0, { x: 10, y: 10, w: 5, h: 5 }), outside(12, 12, { x: 10, y: 10, w: 5, h: 5 }), outside(5, 12, { x: 10, y: 10, w: 5, h: 5 }, 6)], [true, false, false]);

// the calculator: arithmetic only
eq('calc', calc('2*(3+4)'), 14);
eq('calc power', calc('2^10'), 1024);
eq('calc comma', calc('1,5+1'), 2.5);
eq('calc x', calc('3x3'), 9);
eq('calc letters', calc('alert(1)'), null);
eq('calc unbalanced', calc('(1+2'), null);
eq('calc divide by zero', calc('1/0'), null);
eq('calc too deep', calc('('.repeat(40) + '1' + ')'.repeat(40)), null);
eq('calc too long', calc('1+'.repeat(150) + '1'), null);

// theme.json: every option survives Settings' check, with its labels in the shell catalog
const meta = JSON.parse(fs.readFileSync(new URL('../../themes/celeste/theme.json', import.meta.url), 'utf8'));
const clean = cleanThemeMeta(meta);
eq('options kept', clean.options.map((o) => o.key), ['scheme', 'surface', 'hover', 'wpEffect']);
eq('choices kept', clean.options.map((o) => (o.choices ? o.choices.length : 'toggle')), [5, 4, 'toggle', 6]);
const en = JSON.parse(fs.readFileSync(new URL('../../assets/locales/en/shell.json', import.meta.url), 'utf8'));
const labels = clean.options.flatMap((o) => [o.label, o.hint, ...(o.choices || []).map((c) => c[1])]).filter(Boolean);
eq('labels translated', labels.filter((k) => !(k.slice(6) in en)), []);
// an official theme since 0.8.9.6.2 (LordOS-themes), with its real author
eq('official', [clean.community, clean.author, clean.email, clean.organization, clean.license], [false, 'Federico Pereira', 'fpereira@iperfex.com', 'IPERFEX', 'MIT']);
eq('small', fs.statSync(new URL('../../themes/celeste/theme.json', import.meta.url)).size < 4096, true);

console.log(`celeste-model-test: ${ok} ok`);
