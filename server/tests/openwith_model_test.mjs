// openwith_model_test.mjs — sdk/openwithmodel.js: "Open with" providers (docs/FILES.md, "Open with").
// node server/tests/openwith_model_test.mjs
import { extKey, providersFor, hintsFor, suggestedFor, defaultFor, withDefault, OPEN_DEFAULTS_MAX } from '../../sdk/openwithmodel.js';

let pass = 0, fail = 0;
function eq(name, got, want) {
  const g = JSON.stringify(got), w = JSON.stringify(want);
  if (g === w) pass++; else { fail++; console.log(`✗ ${name}: want ${w}, got ${g}`); }
}
const exe = { name: 'winbox.exe', path: '/home/u/Downloads/winbox.exe' };
const docx = { name: 'Report.DOCX', path: '/home/u/Report.DOCX' };
const isExe = (f) => /\.(exe|msi)$/i.test(f.name);
const m1 = { id: 'machine:a', label: 'Win A (Windows)', accepts: (f) => !f.dir, suggests: isExe, open: () => 1 };
const m2 = { id: 'machine:b', label: 'Win B (Windows)', accepts: (f) => !f.dir, suggests: isExe, available: () => false, open: () => 1 };
const wine = { id: 'wine', label: 'Wine', accepts: isExe, suggests: isExe, open: () => 1 };
const broken = { id: 'x', label: 'X', accepts: () => { throw new Error('no'); }, open: () => 1 };
const nolabel = { id: 'y', accepts: () => true, open: () => 1 };

eq('ext: lower case', extKey('A.EXE'), 'exe');
eq('ext: none', [extKey('README'), extKey('.bashrc'), extKey('x.'), extKey('a.tar.gz')], ['', '', '', 'gz']);
eq('ext: odd characters refused', [extKey('a.e x'), extKey('a.' + 'x'.repeat(17))], ['', '']);
eq('listed: available, accepting, once, broken ones skipped', providersFor([m1, m2, wine, broken, nolabel, m1], exe).map((p) => p.id), ['machine:a', 'wine']);
eq('a document: only those that accept it', providersFor([m1, wine], docx).map((p) => p.id), ['machine:a']);
eq('folders, apps and the Trash: none', [providersFor([m1], { name: 'd', dir: true }), providersFor([m1], { name: 'a', app: true }), providersFor([m1], { ...exe, trash: true })], [[], [], []]);
eq('suggested for a program', suggestedFor([m2, m1, wine], exe)?.id, 'machine:a');
eq('nothing suggested for a document', suggestedFor([m1, wine], docx), null);
eq('default: the choice for the extension', defaultFor({ exe: 'wine' }, [m1, wine], exe)?.id, 'wine');
eq('default: case of the name ignored', defaultFor({ docx: 'machine:a' }, [m1], docx)?.id, 'machine:a');
eq('default: a provider that is gone is no default', defaultFor({ exe: 'machine:b' }, [m1, m2], exe), null);
eq('default: none chosen', [defaultFor({}, [m1], exe), defaultFor(null, [m1], exe), defaultFor({ exe: 5 }, [m1], exe)], [null, null, null]);
eq('set', withDefault({}, 'exe', 'machine:a'), { exe: 'machine:a' });
eq('forget', withDefault({ exe: 'machine:a', msi: 'wine' }, 'exe', null), { msi: 'wine' });
eq('bad keys and values dropped', withDefault({ 'a b': 'x', ok: '', msi: 'wine', __proto__x: 'z' }, 'EXE', 'w'), { msi: 'wine', exe: 'w' });
eq('a bad extension changes nothing else', withDefault({ msi: 'wine' }, '../x', 'w'), { msi: 'wine' });
let many = {};
for (let i = 0; i < OPEN_DEFAULTS_MAX + 5; i++) many = withDefault(many, `e${i}`, 'p');
eq('at most OPEN_DEFAULTS_MAX kept, the newest', [Object.keys(many).length, 'e0' in many, `e${OPEN_DEFAULTS_MAX + 4}` in many], [OPEN_DEFAULTS_MAX, false, true]);
const tip = { id: 'machines:tools', hint: (f) => (isExe(f) ? 'update the tools' : '') };
eq('hint: no provider opens it', hintsFor([tip, m2, tip], exe), ['update the tools']);
eq('hint: none when a provider opens it', hintsFor([tip, m1], exe), []);
eq('hint: nothing to say for this file', hintsFor([tip], docx), []);
eq('hint: broken or odd ones skipped', hintsFor([{ id: 'b', hint: () => { throw new Error('x'); } }, { id: 'n', hint: () => 5 }, { hint: () => 'no id' }, tip], exe), ['update the tools']);
eq('hint: folders, apps and the Trash: none', [hintsFor([tip], { name: 'd.exe', dir: true }), hintsFor([tip], { ...exe, trash: true })], [[], []]);
eq('a hint is no provider', [providersFor([tip], exe), suggestedFor([tip], exe)], [[], null]);
console.log(`openwith-model-test: ${pass} passed, ${fail} failed`);
process.exit(fail ? 1 : 0);
