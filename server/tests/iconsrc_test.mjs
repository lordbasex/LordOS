// The <Image src> for a Linux app's icon file (sdk/iconsrc.js): PNGs as they are, SVGs drawn by lord-media at the size
// shown, symbolic ones in the given color. node server/tests/iconsrc_test.mjs
import { iconSrc, isSymbolic, hexColor } from '../../sdk/iconsrc.js';

let ok = 0;
const eq = (name, a, b) => { if (JSON.stringify(a) !== JSON.stringify(b)) { console.log(`FAIL ${name}: ${JSON.stringify(a)} != ${JSON.stringify(b)}`); process.exitCode = 1; } else ok++; };

const vol = '/usr/share/icons/Adwaita/symbolic/status/audio-volume-high-symbolic.svg';
const app = '/usr/share/icons/hicolor/scalable/apps/editor.svg';

// PNGs and nothing
eq('a PNG as it is', iconSrc('/usr/share/pixmaps/a.png', 16, '#fff'), '/usr/share/pixmaps/a.png');
eq('no icon', iconSrc('', 16), '');
eq('undefined', iconSrc(undefined, 16), '');
eq('a relative SVG is not drawn', iconSrc('icons/a.svg', 16), 'icons/a.svg');

// SVG: the size at twice the box, between 16 and 256
eq('an app icon in the Dock', iconSrc(app, 48), `svg:96:-:${app}`);
eq('small', iconSrc(app, 4), `svg:16:-:${app}`);
eq('large', iconSrc(app, 512), `svg:256:-:${app}`);
eq('scale 1', iconSrc(app, 20, null, 1), `svg:20:-:${app}`);
eq('a full-color one keeps its colors', iconSrc(app, 16, '#ffffff'), `svg:32:-:${app}`);

// symbolic
eq('the tray volume icon in the bar color', iconSrc(vol, 16, '#f5f5f7'), `svg:32:f5f5f7:${vol}`);
eq('symbolic with no color keeps its own', iconSrc(vol, 16), `svg:32:-:${vol}`);
eq('symbolic, color that does not parse', iconSrc(vol, 16, 'tomato'), `svg:32:-:${vol}`);
eq('a -symbolic name', isSymbolic('/usr/share/icons/hicolor/scalable/status/net-symbolic.svg'), true);
eq('an -rtl variant', isSymbolic('/x/audio-volume-high-symbolic-rtl.svg'), true);
eq('a symbolic folder', isSymbolic('/usr/share/icons/Adwaita/symbolic/status/a.svg'), true);
eq('not symbolic', isSymbolic(app), false);
eq('a PNG is never symbolic', isSymbolic('/x/a-symbolic.png'), false);

// colors
eq('#rgb', hexColor('#fA0'), 'ffaa00');
eq('#rrggbb', hexColor('#F5F5F7'), 'f5f5f7');
eq('#rrggbbaa drops alpha', hexColor('#11223380'), '112233');
eq('rgba', hexColor('rgba(235, 235, 245, 0.55)'), 'ebebf5');
eq('rgb clamps', hexColor('rgb(300,0,12)'), 'ff000c');
eq('a name is not a color here', hexColor('white'), '');
eq('nothing', hexColor(null), '');

if (!process.exitCode) console.log(`iconsrc-test: ${ok} ok`);
