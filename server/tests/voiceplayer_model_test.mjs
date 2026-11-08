// voiceplayer_model_test.mjs — where the reading-aloud player goes and how its wave moves (sdk/voiceplayermodel.js).
//
//   node tests/voiceplayer_model_test.mjs        (from server/; run_suites.sh runs it when node exists)
import path from 'node:path';
import { fileURLToPath } from 'node:url';

const here = path.dirname(fileURLToPath(import.meta.url));
const { placePlayer, rememberPlace, smoothBands, PLAYER_BARS } = await import(path.join(here, '../../sdk/voiceplayermodel.js'));

let n = 0, bad = 0;
const eq = (name, got, want) => {
  n++;
  const g = JSON.stringify(got), w = JSON.stringify(want);
  if (g !== w) { bad++; console.log(`✗ ${name}: want ${w}, got ${g}`); }
};
const ok = (name, v) => eq(name, !!v, true);

const size = { w: 400, h: 60 };
const A = { id: 1, name: 'DP-1', x: 0, y: 0, w: 1920, h: 1080, main: true };
const B = { id: 2, name: 'DP-2', x: 1920, y: 0, w: 2560, h: 1440 };
const C = { id: 3, name: 'HDMI-A-1', x: -1280, y: 200, w: 1280, h: 1024 };
const all = [A, B, C];

// first time: the bottom center of the reading window's display, else the main one
eq('main, centered', placePlayer(all, null, null, size), { x: 760, y: 1080 - 140 - 60 });
eq('near DP-2', placePlayer(all, null, B, size).x, 1920 + 1080);
ok('near DP-2: on it', placePlayer(all, null, B, size).y > 1000);
// where it was left, on its display
const r = rememberPlace(all, 1920 + 100, 300, size);
eq('remembered on DP-2', r, { display: 'DP-2', dx: 100, dy: 300 });
eq('comes back there', placePlayer(all, r, A, size), { x: 2020, y: 300 });
// on a display to the left (negative x)
const l = rememberPlace(all, -1200, 900, size);
eq('left display', l.display, 'HDMI-A-1');
eq('left display: back', placePlayer(all, l, null, size), { x: -1200, y: 900 });
// the display isn't there any more: the main one
eq('gone display', placePlayer([A], r, null, size), placePlayer([A], null, null, size));
// dragged off the edge: kept inside
eq('off the right edge', rememberPlace(all, 1920 + 2560 - 10, 50, size), { display: 'DP-2', dx: 2560 - 400 - 8, dy: 50 });
eq('nonsense saved', placePlayer(all, { display: 'DP-2', dx: NaN, dy: 1 }, null, size), placePlayer(all, null, null, size));
eq('no displays', placePlayer([], null, null, size).x >= 0, true);

// the wave
const z = smoothBands(null, null, 16);
eq('bars', z.length, PLAYER_BARS);
ok('nothing: flat', z.every((v) => v === 0));
const up = smoothBands(z, Array(16).fill(255), 16);
ok('rises at once', up.every((v) => v === 1));
const down = smoothBands(up, Array(16).fill(0), 16);
ok('falls slowly', down.every((v) => v > 0.8 && v < 1));
const later = smoothBands(down, Array(16).fill(0), 1000);
ok('falls within a second', later.every((v) => v === 0));
ok('bad levels are 0', smoothBands(null, [NaN, -5, 999], 16).slice(0, 3).join() === '0,0,1');

console.log(`voiceplayer_model_test: ${n - bad}/${n}`);
process.exit(bad ? 1 : 0);
