// calculator_skin_scene.mjs — a scene of tests/menubar_test.mjs (not run by itself): the Calculator with its display and
// keypad stubbed (tests/app_skin_helpers.js). press() takes only a key of the keypad; the arithmetic is the calculator's.
import { mountStubbed, commonChecks, check, flush } from './app_skin_helpers.js';
import Calculator from '../../apps/calculator/index.jsx';

const st = await mountStubbed('calculator', Calculator);
const m = () => st.seen.keypad;
check('calculator: the keys', m() && m().keys.flat().length >= 19 && m().keys.flat().some((k) => k.id === '=' && k.kind === 'equals'), JSON.stringify(m() && m().keys));
for (const k of ['1', '2', '+', '3', '=']) { m().actions.press(k); await flush(); }
check('calculator: 12+3 = 15, by the calculator', st.seen.display.result === '15', st.seen.display.result);
m().actions.press('alert(1)');
m().actions.press('9;');
await flush();
check('calculator: anything not a key is refused', st.seen.display.expression === '15' && st.seen.display.result === '15', st.seen.display.expression);
// GitHub #271: % and ± are keys of the keypad, with the calculator's meaning
check('calculator: % and ± keys', ['%', '±'].every((id) => m().keys.flat().some((k) => k.id === id && k.kind === 'function')));
for (const k of ['C', '2', '0', '0', '+', '1', '0', '%', '=']) { m().actions.press(k); await flush(); }
check('calculator: 200 + 10% = 220', st.seen.display.result === '220', st.seen.display.result);
for (const k of ['C', '7', '±', '×', '3', '=']) { m().actions.press(k); await flush(); }
check('calculator: ±7 × 3 = −21', st.seen.display.result === '−21', st.seen.display.result);
await commonChecks('calculator', st, { bar: 'keypad' });
process.exit(0);
