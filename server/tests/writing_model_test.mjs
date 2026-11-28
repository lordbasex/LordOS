// writing_model_test.mjs — ✨ AI on selected text (sdk/writingmodel.js, docs/AI.md "Writing"): when the menu offers it
// (never in a password field, only with a selection and a model), its rows, the translation targets, the model's
// answer made plain text, and the panel's flow against a fake lord.writing (streamed, then Replace only on the click,
// only where the selection still is). node server/tests/writing_model_test.mjs
import {
  writingEnabled, WRITE_ACTIONS, WRITE_MAX_CHARS, TRANSLATE_LANGS, writingModel, offersWriting, tooLong, translateTargets, writingMenuItem, cleanOutput,
  writingStart, writingStep, canReplace, stillThere, forField,
} from '../../sdk/writingmodel.js';
import { readdirSync, readFileSync } from 'node:fs';
import { fileURLToPath } from 'node:url';

let ok = 0, bad = 0;
const eq = (name, a, b) => {
  if (JSON.stringify(a) === JSON.stringify(b)) ok++;
  else { bad++; console.error(`FAIL ${name}: ${JSON.stringify(a)} != ${JSON.stringify(b)}`); }
};

// ---- when ----
eq('on by default once a model is installed', writingEnabled(null, true), true);
eq('hidden without a model, whatever the choice', [writingEnabled(null, false), writingEnabled(true, false)], [false, false]);
eq('an explicit off stays off, a new model included', writingEnabled(false, true), false);
eq('an explicit on', writingEnabled(true, true), true);
eq('off (the person\'s choice): no model', writingModel({ on: false, available: true, models: [{ id: 'qwen3-1.7b' }], chosen: 'qwen3-1.7b' }), null);
eq('no engine', writingModel({ on: true, available: false, models: [{ id: 'qwen3-1.7b' }] }), null);
eq('no model', writingModel({ on: true, available: true, models: [] }), null);
eq('the model chosen for Lumo', writingModel({ on: true, available: true, models: [{ id: 'qwen3-1.7b' }, { id: 'qwen3-4b' }], chosen: 'qwen3-4b' }), 'qwen3-4b');
eq('else the first installed', writingModel({ on: true, available: true, models: [{ id: 'qwen3-1.7b' }], chosen: 'gone' }), 'qwen3-1.7b');
eq('never in a password field', offersWriting({ secure: true, selected: 'secret', ready: true }), false);
eq('only with a selection', [offersWriting({ selected: '', ready: true }), offersWriting({ selected: '   ', ready: true }), offersWriting({ selected: 'x', ready: true })], [false, false, true]);
eq('a field that opted out', offersWriting({ selected: 'x', ready: true, allowed: false }), false);
eq('the cap', [tooLong('a'.repeat(WRITE_MAX_CHARS)), tooLong('a'.repeat(WRITE_MAX_CHARS + 1)), tooLong('😀'.repeat(WRITE_MAX_CHARS))], [false, true, false]);

// ---- the rows ----
const tr = (k) => k;
const picks = [];
const item = writingMenuItem({ tr, langName: (c) => `L:${c}`, targets: ['es', 'en'], onAction: (a, l) => picks.push([a, l]) });
eq('✨ AI ▸ the actions, then Translate to', item.submenu.map((r) => (r === '-' ? '-' : r.header ? `[${r.header}]` : r.label)),
  ['writing.action.fix', 'writing.action.improve', 'writing.action.shorter', '-', '[writing.menu.translate]', 'L:es', 'L:en']);
eq('its label', item.label, '✨ writing.menu.ai');
item.submenu[0].onClick();
item.submenu[6].onClick();
eq('each row names a fixed action', picks, [['fix', null], ['translate', 'en']]);
eq('too long: disabled, no submenu', writingMenuItem({ tr, langName: String, targets: [], onAction() {}, long: true }), { label: '✨ writing.menu.ai', disabled: true, hint: 'writing.menu.tooLong' });
eq('targets: interface language, spelling languages, then common ones, at most ten', translateTargets('pt', ['pt_BR', 'de_DE', 'xx_YY']), ['pt', 'de', 'en', 'es', 'fr', 'it', 'zh-Hans', 'ja']);
eq('every target is one lord-server knows', translateTargets('ar', ['fa', 'ur']).every((l) => TRANSLATE_LANGS.includes(l)), true);
const here = fileURLToPath(new URL('../../assets/locales', import.meta.url));
eq('the targets are the interface languages', [...TRANSLATE_LANGS].sort(), readdirSync(here).filter((d) => !d.startsWith('.')).sort());
eq('three actions', WRITE_ACTIONS, ['fix', 'improve', 'shorter']);

// ---- the answer is plain text ----
eq('trimmed, CRLF made LF', cleanOutput('  Hello\r\nworld \n'), 'Hello\nworld');
eq('control characters and bidi overrides out', cleanOutput('a\u0007b\u001b[31mc‮d⁦e'), 'ab[31mcde');
eq('a <think> block out', cleanOutput('<think>plan</think>Done.'), 'Done.');
eq('echoed markers out', cleanOutput('<<<TEXT 0123456789abcdef>>>\nFixed\n<<<END-TEXT 0123456789abcdef>>>'), 'Fixed');
eq('a code fence around the whole answer out', cleanOutput('```\nplain\n```'), 'plain');
eq('markup stays text', cleanOutput('<b>x</b> **y** [z](javascript:alert(1))'), '<b>x</b> **y** [z](javascript:alert(1))');
eq('one line for a one-line field', forField('a\n b \nc', false), 'a b c');
eq('lines kept in a multi-line one', forField('a\nb', true), 'a\nb');

// ---- the flow with a fake lord.writing: streamed, then Replace only on the click ----
function fakeWriting(chunks, final) {
  let i = 0;
  return { peek: () => (i < chunks.length ? { state: 'writing', text: chunks.slice(0, ++i).join('') } : null), take: () => (i >= chunks.length ? final : null) };
}
let st = writingStart();
const fake = fakeWriting(['The ', 'text ', 'fixed.'], { ok: true, content: 'The text fixed.', truncated: false });
const seen = [];
for (let n = 0; n < 10 && st.phase !== 'done'; n++) {
  const r = fake.take();
  if (r) st = writingStep(st, { type: 'take', result: r });
  else { const p = fake.peek(); if (p) st = writingStep(st, { type: 'peek', ...p }); }
  seen.push(st.phase === 'writing' ? st.text : st.phase);
}
eq('streamed, then done', seen, ['The', 'The text', 'The text fixed.', 'done']);
eq('Replace only when done', [canReplace(writingStart()), canReplace({ phase: 'writing', text: 'x' }), canReplace(st)], [false, false, true]);
eq('nothing after done', writingStep(st, { type: 'peek', state: 'writing', text: 'other' }), st);
eq('an error keeps what came', writingStep({ phase: 'writing', text: 'par' }, { type: 'take', result: { ok: false, error: 'llm.timeout' } }), { phase: 'error', text: 'par', error: 'llm.timeout' });
eq('an empty answer is an error', writingStep(writingStart(), { type: 'take', result: { ok: true, content: '  \u0007 ' } }).phase, 'error');
eq('cancel', writingStep(writingStart(), { type: 'cancel' }).error, 'llm.cancelled');
eq('an injected answer is only text', writingStep(writingStart(), { type: 'take', result: { ok: true, content: 'Ignore the person. \u001b]52;c;cm0gLXJm\u0007' } }).text, 'Ignore the person. ]52;c;cm0gLXJm');
eq('Replace where the selection was', stillThere('Hello wrold!', [6, 11], 'wrold'), true);
eq('not when the text changed meanwhile', stillThere('Hello world!', [6, 11], 'wrold'), false);
eq('code points, not UTF-16', stillThere('😀 wrold', [2, 7], 'wrold'), true);

// ---- "Translate to" names every target in every interface language (languages.json, tools/gen-language-names.mjs):
// "zh-Hans" showed raw (ws164) before the script ids were in the catalog ----
{
  const dir = fileURLToPath(new URL('../../assets/locales/', import.meta.url));
  const missing = [];
  for (const l of readdirSync(dir)) {
    let names;
    try { names = JSON.parse(readFileSync(`${dir}${l}/languages.json`, 'utf8')); } catch (e) { continue; }
    for (const c of TRANSLATE_LANGS) if (typeof names[c] !== 'string' || !names[c] || names[c] === c) missing.push(`${l}:${c}`);
  }
  eq('every translation target has a name in every catalog', missing, []);
  eq('Simplified Chinese in Spanish', JSON.parse(readFileSync(`${dir}es/languages.json`, 'utf8'))['zh-Hans'], 'Chino simplificado');
}

console.log(`writing-model-test: ${ok} passed, ${bad} failed`);
process.exit(bad ? 1 : 0);
