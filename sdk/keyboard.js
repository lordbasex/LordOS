// keyboard.js — the user's keyboard layouts (libxkbcommon in the server).
//
// settings.keyboards = [{ layout: 'latam', variant: '' }, { layout: 'us', variant: '', ime: 'kana' }]
// settings.keyboardIndex = the active one. keyboard.next() switches to the next one (menu bar button
// or ❖⌥Space). Any xkb-data layout works: keyboard.list() returns all of them with
// their variants. With `ime` it is an input method (ime.c) on top of the Latin keyboard `layout`:
// hangul (Korean), kana (Japanese), pinyin (Simplified Chinese).
import { setLayout } from '@lordos/runtime';
import { settings } from './settings.js';

const native = () => typeof lord.keyboard !== 'undefined' && lord.keyboard.current().xkb;

export function configured() {
  const s = settings.get();
  if (Array.isArray(s.keyboards) && s.keyboards.length) return s.keyboards;
  return [{ layout: s.keyboard || 'latam', variant: '' }];      // settings from before M5
}

let applied = '';
let imeStatus = 0;      // 1 = the input method did not find its dictionary
export const IME_ENGINES = [
  { id: 'hangul', lang: 'ko', label: '한' },
  { id: 'kana', lang: 'ja', label: 'あ' },
  { id: 'pinyin', lang: 'zh', label: '拼' },
];
function apply() {
  const list = configured();
  const idx = Math.min(settings.get().keyboardIndex || 0, list.length - 1);
  const key = JSON.stringify([list, idx]);
  if (key === applied) return;
  applied = key;
  if (native() || lord.keyboard?.set) {
    const ok = lord.keyboard.set(list.map((k) => k.layout).join(','), list.map((k) => k.variant || '').join(','));
    if (ok) lord.keyboard.setGroup(idx);
    imeStatus = lord.ime ? lord.ime.set(list[idx].ime || '') : -1;
    if (ok) return;
  }
  setLayout(list[idx].layout === 'us' ? 'us' : 'latam');        // fallback without XKB
}

let catalog = null;
export const keyboard = {
  list() { if (!catalog) catalog = lord.keyboard?.list?.() || []; return catalog; },
  configured,
  active: () => configured()[Math.min(settings.get().keyboardIndex || 0, configured().length - 1)],
  next() {
    const n = configured().length;
    settings.set({ keyboardIndex: ((settings.get().keyboardIndex || 0) + 1) % n });
  },
  set: (keyboards) => settings.set({ keyboards, keyboardIndex: 0 }),
  // Short label for the menu bar: "LA", "US", "BR", "한", "あ", "拼"…
  label: (k) => IME_ENGINES.find((e) => e.id === k.ime)?.label || (k.label || k.layout).slice(0, 2).toUpperCase(),
  engines: IME_ENGINES,
  // Is the active input method's dictionary missing? (install the language pack)
  missingDictionary: () => imeStatus === 1,
};

apply();
settings.subscribe(apply);
