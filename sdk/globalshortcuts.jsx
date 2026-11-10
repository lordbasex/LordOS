// globalshortcuts.jsx — Linux apps' global shortcuts (org.freedesktop.portal.GlobalShortcuts,
// server/src/ui/portal.c; the rules in globalshortcutsmodel.js and server/src/ui/gshort.c).
//
//   <GlobalShortcutsPrompt />  the sheet an app's BindShortcuts opens: each shortcut with the app's
//                              description and a key field that starts on the proposed key when it is
//                              free; nothing is bound until the person presses Save
//   <KeyComboField />          a key field: click, then press the combination (Esc stops, Backspace clears)
//   globalShortcutList() …     Settings → Keyboard: what each app was given, changed or forgotten there
//
// lord-server decides again whatever the shell answers (the keys LordOS keeps, the other apps' bindings).
// While a field records, no app's shortcut fires (lord.portal.pauseShortcuts) and no shell shortcut runs
// (captureKeys): the keys pressed only name the combination.
import { createElement as h, useEffect, useState } from 'react';
import { View, Text, mainBox } from '@lordos/runtime';
import { createStore, useStore } from './store.js';
import { t } from './i18n.js';
import { linuxAppFor, shortAppName } from './apps.js';
import { captureKeys, comboLabel, shortcutReserved } from './shortcuts.js';
import { useSystemTokens, DialogButton } from './hardware.jsx';
import { ScrollView } from './components.jsx';
import { comboOfKey, problemOf, sheetRows, setChoice, answerOf, byApp } from './globalshortcutsmodel.js';

const portal = () => (typeof lord !== 'undefined' && lord.portal && typeof lord.portal.shortcutsChosen === 'function' ? lord.portal : null);

const requests = createStore([]);       // sheets waiting, the first one shown
export const globalShortcutsRev = createStore(0);

export function globalShortcutsEvent(kind, data) {
  if (kind === 'portal-shortcuts' && data && Number.isInteger(data.id)) requests.set([...requests.get().filter((r) => r.id !== data.id), data]);
  else if (kind === 'portal-shortcuts-gone') requests.set(requests.get().filter((r) => r.id !== data?.id));
  else if (kind === 'portal-shortcuts-changed') globalShortcutsRev.set((v) => v + 1);
}

// "desktop:<id>" → the app's name; an app in no scope of its own by its process name
export function globalShortcutApp(app, comm) {
  const id = typeof app === 'string' && app.startsWith('desktop:') ? app.slice(8) : '';
  const known = id ? linuxAppFor(id) : null;
  return known ? shortAppName(known) : id || comm || app || t('shell.portalShortcuts.unknownApp');
}

export function problemText(problem, combo, owner) {
  return problem ? t(`shell.portalShortcuts.problem.${problem}`, { keys: combo ? comboLabel(combo) : '', app: globalShortcutApp(owner) }) : '';
}

// A key field. check(combo) → a problem (globalshortcutsmodel.js problemOf) or null; onChange('') unassigns.
export function KeyComboField({ value, onChange, check, th }) {
  const [recording, setRecording] = useState(false);
  const [error, setError] = useState('');
  useEffect(() => {
    if (!recording) return undefined;
    const end = captureKeys((e) => {
      const plain = !e.ctrl && !e.alt && !e.shift && !e.meta;
      if (plain && e.code === 1) { setRecording(false); return; }
      if (plain && (e.code === 14 || e.code === 111)) { setError(''); onChange(''); setRecording(false); return; }
      const combo = comboOfKey(e);
      if (!combo) return;
      const problem = check(combo);
      setRecording(false);
      if (problem) { setError(problemText(problem.problem || problem, combo, problem.owner)); return; }
      setError('');
      onChange(combo);
    });
    portal()?.pauseShortcuts(true);
    return () => { end(); portal()?.pauseShortcuts(false); };
  }, [recording]);
  const label = recording ? t('shell.portalShortcuts.press') : value ? comboLabel(value) : t('shell.portalShortcuts.none');
  return h(View, { style: { alignItems: 'flex-end', gap: 3 } },
    h(View, { onClick: () => { setError(''); setRecording(!recording); },
      style: { minWidth: 110, height: 24, paddingHorizontal: 10, borderRadius: 6, alignItems: 'center', justifyContent: 'center',
        backgroundColor: th.mode === 'light' ? 'rgba(0,0,0,0.05)' : 'rgba(0,0,0,0.32)', border: `1px solid ${recording ? th.accent : th.separator}`,
        boxShadow: recording ? `0 0 0 3px ${th.accent}40` : 'none', direction: 'ltr' } },
    h(Text, { style: { fontSize: 12, color: value || recording ? th.text : th.textDim } }, label)),
    error ? h(Text, { style: { fontSize: 11, color: '#ff453a', maxWidth: 220, textAlign: 'end' } }, error) : null);
}

// A combination for a row of an open sheet: lord-server's verdict, the shell's own keys and the sheet's other rows
function checkRow(session, rows, i) {
  return (combo) => {
    const verdict = portal()?.shortcutCheck(combo, { session, id: rows[i].id }) || null;
    const problem = problemOf(verdict, { shellTaken: shortcutReserved, others: rows.filter((_, j) => j !== i).map((r) => r.choice).filter(Boolean) });
    return problem ? { problem, owner: verdict?.owner || '' } : null;
  };
}

function Sheet({ p }) {
  const th = useSystemTokens();
  const [rows, setRows] = useState(() => sheetRows(p, { shellTaken: shortcutReserved }));
  const app = globalShortcutApp(p.app, p.comm);
  const answer = (save) => {
    requests.set(requests.get().filter((r) => r.id !== p.id));
    portal()?.shortcutsChosen(p.id, save ? answerOf(rows) : null);
  };
  return h(View, { style: { position: 'absolute', ...mainBox(), alignItems: 'center', justifyContent: 'flex-start',
    backgroundColor: 'rgba(0,0,0,0.3)', zIndex: 100000 } },
  h(View, { style: { marginTop: 90, width: 460, borderRadius: 14, padding: 22, gap: 10, backgroundColor: th.windowBg,
    border: `1px solid ${th.separator}`, boxShadow: '0 20px 60px rgba(0,0,0,0.45)' } },
  h(Text, { style: { fontSize: 14, fontWeight: 700, color: th.text, textAlign: 'center' } }, t('shell.portalShortcuts.title', { app })),
  h(Text, { style: { fontSize: 12, color: th.textDim, textAlign: 'center', lineHeight: 1.4 } }, t('shell.portalShortcuts.body', { app })),
  h(ScrollView, { style: { maxHeight: 360, marginTop: 4 }, contentStyle: { gap: 8 } },
    rows.map((r, i) => h(View, { key: r.id, style: { flexDirection: 'row', alignItems: 'flex-start', gap: 10 } },
      h(View, { style: { flex: 1, gap: 2, paddingTop: 4 } },
        h(Text, { style: { fontSize: 13, color: th.text, textAlign: 'start' } }, r.description || r.id),
        r.note ? h(Text, { style: { fontSize: 11, color: th.textDim, textAlign: 'start' } }, problemText(r.note, r.proposed, r.owner)) : null),
      h(KeyComboField, { th, value: r.choice, check: checkRow(p.id, rows, i), onChange: (c) => setRows((now) => setChoice(now, i, c)) })))),
  h(Text, { style: { fontSize: 11, color: th.textDim, textAlign: 'center' } }, t('shell.portalShortcuts.keysHint')),
  h(View, { style: { flexDirection: 'row', gap: 10, marginTop: 8, alignSelf: 'stretch' } },
    h(DialogButton, { label: t('shell.portalShortcuts.cancel'), onClick: () => answer(false), th }),
    h(DialogButton, { label: t('shell.portalShortcuts.save'), primary: true, th, onClick: () => answer(true) }))));
}

export function GlobalShortcutsPrompt() {
  const list = useStore(requests);
  const p = list[0];
  return p ? h(Sheet, { key: p.id, p }) : null;
}

// ---- Settings → Keyboard ----

// [{ app, name, items: [{ id, description, trigger }] }]
export function globalShortcutList() {
  const list = portal()?.shortcuts() || [];
  return byApp(list).map((g) => ({ ...g, name: globalShortcutApp(g.app) }));
}
export const useGlobalShortcuts = () => { useStore(globalShortcutsRev); return globalShortcutList(); };

export function checkGlobalShortcut(app, id) {
  return (combo) => {
    const verdict = portal()?.shortcutCheck(combo, { app, id }) || null;
    const problem = problemOf(verdict, { shellTaken: shortcutReserved });
    return problem ? { problem, owner: verdict?.owner || '' } : null;
  };
}
export const setGlobalShortcut = (app, id, combo) => !!portal()?.setShortcut(app, id, combo || '');
export const forgetGlobalShortcuts = (app) => portal()?.forgetShortcuts(app) || 0;
