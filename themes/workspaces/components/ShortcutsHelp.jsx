// The shortcuts' help (the lifebuoy on the Context Spine, ❖/, or the crown's menu): every shortcut that works now,
// the theme's own marked as such and the ones every theme shares (Search, the clipboard, Screenshots' captures…), read
// from the SDK's own list (sdk/shortcuts.js shortcutList, the one Settings → Keyboard shows), so it can never disagree
// with what the keys do. Then what the pointer and the touchpad do in this theme. A search field (from the first
// character), a filter by group, Esc or Done closes; a button opens Settings → Keyboard, where they are changed.
import { View, Text, ScrollView, SearchField, Layer, useStore, createStore, shortcutList, shortcutsRev, wm, t as tr, mainDisplay } from '@lordos/sdk';
import { memo, useState } from 'react';
import { LifebuoyGlyph } from '../glyphs.jsx';

export const shortcutsHelp = createStore(false);

// What the pointer and the touchpad do here: [what, how] locale keys.
const POINTER = [
  ['shell.ws.help.m.rename', 'shell.ws.help.m.renameHow'],
  ['shell.ws.help.m.menu', 'shell.ws.help.m.menuHow'],
  ['shell.ws.help.m.peek', 'shell.ws.help.m.peekHow'],
  ['shell.ws.help.m.group', 'shell.ws.help.m.groupHow'],
  ['shell.ws.help.m.overview', 'shell.ws.help.m.overviewHow'],
  ['shell.ws.help.m.apps', 'shell.ws.help.m.appsHow'],
  ['shell.ws.help.m.switch', 'shell.ws.help.m.switchHow'],
];

const MODS = '❖⌃⌥⇧';
// '❖⌥D' → ['❖', '⌥', 'D']: each modifier on its own keycap, the key after them on one
export function keycaps(combo) {
  const out = [];
  let i = 0;
  const chars = [...combo];
  while (i < chars.length && MODS.includes(chars[i])) out.push(chars[i++]);
  const rest = chars.slice(i).join('');
  if (rest) out.push(rest);
  return out;
}
const fold = (s) => String(s || '').toLowerCase().normalize('NFD').replace(/[̀-ͯ]/g, '');

function Keys({ t, combos }) {
  return (
    <View style={{ flexDirection: 'row', flexWrap: 'wrap', justifyContent: 'flex-end', gap: 6, direction: 'ltr', flexShrink: 0, maxWidth: 210 }}>
      {combos.map((c, n) => (
        <View key={n} style={{ flexDirection: 'row', gap: 3 }}>
          {keycaps(c).map((k, j) => (
            <View key={j} style={{ minWidth: 22, height: 22, paddingHorizontal: 6, borderRadius: 6, alignItems: 'center', justifyContent: 'center',
              backgroundColor: t.windowBg, border: `1px solid ${t.windowBorder}`, borderBottomWidth: 2 }}>
              <Text style={{ fontSize: 11.5, color: t.text, whiteSpace: 'nowrap' }}>{k}</Text>
            </View>
          ))}
        </View>
      ))}
    </View>
  );
}

function Row({ t, label, theme, children }) {
  return (
    <View style={{ minHeight: 30, flexDirection: 'row', alignItems: 'center', gap: 8, paddingVertical: 2 }}>
      <Text style={{ flex: 1, fontSize: 12.5, color: t.text, textAlign: 'start' }}>{label}</Text>
      {theme ? (
        <View style={{ paddingHorizontal: 6, height: 17, borderRadius: 6, justifyContent: 'center', backgroundColor: t.itemHover }}>
          <Text style={{ fontSize: 9.5, fontWeight: 700, color: t.accent }}>{tr('shell.ws.help.theme')}</Text>
        </View>
      ) : null}
      {children}
    </View>
  );
}

export const ShortcutsHelp = memo(function ShortcutsHelp({ t }) {
  const open = useStore(shortcutsHelp);
  useStore(shortcutsRev);
  const [q, setQ] = useState('');
  const [tab, setTab] = useState('all');
  if (!open) return null;
  const close = () => { shortcutsHelp.set(false); setQ(''); setTab('all'); };
  const f = fold(q.trim());
  const match = (...s) => !f || s.some((x) => fold(x).includes(f));
  const keyGroups = shortcutList().map((g) => ({ id: g.group, title: tr(`settings.kb.shortcuts.${g.group}`),
    rows: g.items.filter((it) => match(it.label, it.keys)).map((it) => ({ key: it.id, label: it.label, theme: it.from === 'theme', combos: it.combos })) }));
  const pointer = { id: 'pointer', title: tr('shell.ws.help.mouse'),
    rows: POINTER.map(([a, b]) => ({ key: a, label: tr(a), how: tr(b), theme: true })).filter((r) => match(r.label, r.how)) };
  const all = [...keyGroups, pointer];
  const shown = all.filter((g) => (tab === 'all' || tab === g.id) && g.rows.length);
  const d = mainDisplay();
  const w = Math.min(960, d.w - 48), h = Math.min(740, d.h - 48);
  const tabBtn = (id, label) => (
    <View key={id} onClick={() => setTab(id)} style={{ height: 26, paddingHorizontal: 11, borderRadius: 13, justifyContent: 'center', backgroundColor: tab === id ? t.accent : t.itemHover }}>
      <Text style={{ fontSize: 12, fontWeight: tab === id ? 600 : 500, color: tab === id ? t.onAccent : t.text, whiteSpace: 'nowrap' }}>{label}</Text>
    </View>
  );
  // two columns: the groups dealt left and right by their length, so the columns end close to each other
  const cols = [[], []], len = [0, 0];
  for (const g of shown) { const c = len[0] <= len[1] ? 0 : 1; cols[c].push(g); len[c] += g.rows.length + 2; }
  const one = w < 720;
  const section = (g) => (
    <View key={g.id} style={{ gap: 2, marginBottom: 10 }}>
      <Text style={{ fontSize: 11, fontWeight: 700, color: t.textDim, letterSpacing: 0.4, marginBottom: 2, textAlign: 'start' }}>{g.title.toUpperCase()}</Text>
      {g.rows.map((r) => (
        <Row key={r.key} t={t} label={r.label} theme={r.theme}>
          {r.combos ? <Keys t={t} combos={r.combos} /> : <Text style={{ fontSize: 11.5, color: t.textDim, maxWidth: 220, textAlign: 'end' }}>{r.how}</Text>}
        </Row>
      ))}
    </View>
  );
  return (
    <Layer name="modal">
      <View onClick={close} style={{ position: 'absolute', left: 0, top: 0, width: '100%', height: '100%', backgroundColor: 'rgba(8,6,20,0.45)' }} />
      <View onClick={(e) => e?.stopPropagation?.()} style={{ position: 'absolute', left: d.x + (d.w - w) / 2, top: d.y + (d.h - h) / 2, width: w, height: h, padding: 22, gap: 10,
        borderRadius: 18, backgroundColor: t.popupBg, border: `1px solid ${t.mat.border}`, boxShadow: t.shadow }}>
        <View style={{ flexDirection: 'row', alignItems: 'center', gap: 10 }}>
          <LifebuoyGlyph size={22} color={t.text} bg={t.popupBg} />
          <Text style={{ fontSize: 19, fontWeight: 700, color: t.text, textAlign: 'start' }}>{tr('shell.ws.help.title')}</Text>
        </View>
        <Text style={{ fontSize: 12.5, color: t.textDim, textAlign: 'start' }}>{tr('shell.ws.help.sub')}</Text>
        <SearchField value={q} onChange={setQ} placeholder={tr('shell.ws.help.search')} autoFocus onCancel={close} />
        <View style={{ flexDirection: 'row', flexWrap: 'wrap', gap: 6 }}>
          {[tabBtn('all', tr('shell.ws.help.all')), ...all.filter((g) => g.rows.length || tab === g.id).map((g) => tabBtn(g.id, g.title))]}
        </View>
        <ScrollView style={{ flex: 1 }} contentStyle={{ paddingTop: 6 }}>
          {shown.length ? (
            one ? <View>{shown.map(section)}</View> : (
              <View style={{ flexDirection: 'row', gap: 30 }}>
                {cols.map((c, i) => <View key={i} style={{ flex: 1, minWidth: 0 }}>{c.map(section)}</View>)}
              </View>
            )
          ) : <Text style={{ fontSize: 12.5, color: t.textDim, paddingVertical: 20, textAlign: 'center' }}>{tr('shell.ws.help.none')}</Text>}
        </ScrollView>
        <View style={{ flexDirection: 'row', alignItems: 'center', gap: 10 }}>
          <Text style={{ flex: 1, fontSize: 12, color: t.textDim, textAlign: 'start' }}>{tr('shell.ws.help.change')}</Text>
          <View onClick={() => { close(); wm.open('settings', { args: { section: 'keyboard' } }); }}
            style={{ height: 32, paddingHorizontal: 14, borderRadius: 9, justifyContent: 'center', backgroundColor: t.itemHover }}>
            <Text style={{ fontSize: 12.5, fontWeight: 600, color: t.text }}>{tr('shell.ws.help.openKeyboard')}</Text>
          </View>
          <View onClick={close} style={{ height: 32, paddingHorizontal: 16, borderRadius: 9, justifyContent: 'center', backgroundColor: t.accent }}>
            <Text style={{ fontSize: 12.5, fontWeight: 600, color: t.onAccent }}>{tr('shell.ws.help.done')}</Text>
          </View>
        </View>
      </View>
    </Layer>
  );
});
