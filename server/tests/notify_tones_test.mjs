// notify_tones_test.mjs — Settings → Notifications → an app → Sound (sdk/notifytones.js): what each choice stores and
// plays, older settings (true / false) keeping their meaning, and every sound being one lord-server synthesizes
// (server/src/sound/snd.c snd_play).
//
//   node tests/notify_tones_test.mjs        (from server/; run_suites.sh runs it when node exists)
import fs from 'node:fs';
import path from 'node:path';
import { fileURLToPath } from 'node:url';

const here = path.dirname(fileURLToPath(import.meta.url));
const root = path.join(here, '..', '..');
const N = await import(path.join(root, 'sdk/notifytones.js'));

let n = 0, bad = 0;
const eq = (name, got, want) => {
  n++;
  const g = JSON.stringify(got), w = JSON.stringify(want);
  if (g !== w) { bad++; console.log(`✗ ${name}: want ${w}, got ${g}`); }
};

eq('the menu', N.TONE_CHOICES, ['default', 'none', 'pop', 'chime', 'ping', 'drop']);
eq('older settings', [N.toneOf(true), N.toneOf(false), N.toneOf(undefined), N.toneChoice(true), N.toneChoice(false), N.toneChoice(undefined)],
  ['notify', null, 'notify', 'default', 'none', 'default']);
eq('a chosen sound', [N.toneOf('chime'), N.toneChoice('ping'), N.prefOfChoice('drop')], ['chime', 'ping', 'drop']);
eq('default and none', [N.prefOfChoice('default'), N.prefOfChoice('none'), N.prefOfChoice('bogus')], [true, false, true]);
eq('an unknown stored name plays the default', [N.toneOf('siren'), N.toneChoice('siren')], ['notify', 'default']);
for (const c of N.TONE_CHOICES) eq(`round trip ${c}`, N.toneChoice(N.prefOfChoice(c)), c);

// every sound is one lord-server knows
const snd = fs.readFileSync(path.join(root, 'server/src/sound/snd.c'), 'utf8');
for (const name of [N.DEFAULT_TONE, ...N.NOTIFY_TONES]) eq(`snd_play knows ${name}`, snd.includes(`!strcmp(name, "${name}")`), true);

// every choice has its label in every complete language (a partial one shows the missing labels in English)
const partial = (lang) => { try { return JSON.parse(fs.readFileSync(path.join(root, 'assets/locales', lang, 'meta.json'), 'utf8')).partial === true; } catch (e) { return false; } };
for (const lang of fs.readdirSync(path.join(root, 'assets/locales'), { withFileTypes: true }).filter((e) => e.isDirectory() && !partial(e.name)).map((e) => e.name)) {
  const prefs = JSON.parse(fs.readFileSync(path.join(root, 'assets/locales', lang, 'prefs.json'), 'utf8'));
  for (const c of N.TONE_CHOICES) eq(`${lang}: prefs.notif.tone.${c}`, typeof prefs[`notif.tone.${c}`], 'string');
}

console.log(bad ? `notify-tones: ${bad} of ${n} failed` : `notify-tones: ${n}/${n} ok`);
process.exit(bad ? 1 : 0);
