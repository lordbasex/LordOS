// skins/common.jsx — Bluebell's drawing of the two shapes most apps share (docs/THEMES-GUIDE.md, "App skins → the shared
// shapes"): a command bar (a row of labelled buttons with the place's title, in the manner of 2001) and a list pane (the
// blue task pane with a box of links). Each app's skin maps its regions to these.
import { View, Text, ScrollView, useTheme, useThemeFont } from '@lordos/sdk';
import { useState } from 'react';

const fontOf = (font) => (font ? { fontFamily: font } : null);

function Cmd({ t, font, c, onClick }) {
  const [hover, setHover] = useState(false);
  const lit = hover && !c.disabled;
  return (
    <View onClick={c.disabled ? undefined : onClick} onMouseEnter={() => setHover(true)} onMouseLeave={() => setHover(false)} accessibilityLabel={c.label}
      style={{ height: 28, flexDirection: 'row', alignItems: 'center', paddingHorizontal: 7, borderRadius: 3, opacity: c.disabled ? 0.45 : 1,
        border: `1px solid ${c.active ? t.stateBorders.pressed : lit ? t.barHoverEdge : 'transparent'}`, background: c.active ? t.barPressed : lit ? t.barHover : 'transparent' }}>
      <Text style={{ fontSize: 12, fontWeight: c.active ? 700 : 400, color: t.text, whiteSpace: 'nowrap', ...fontOf(font) }}>{c.label}</Text>
    </View>
  );
}

// model: { title, commands: [{ id, label, disabled, active, group }], actions: { command } }
export function CommandBar({ model }) {
  const t = useTheme();
  const font = useThemeFont();
  let last = null;
  return (
    <View style={{ minHeight: 36, flexDirection: 'row', alignItems: 'center', flexWrap: 'wrap', gap: 1, paddingHorizontal: 6, paddingVertical: 3,
      background: t.toolbarBg, borderBottom: `1px solid ${t.separator}` }}>
      {model.title ? <Text style={{ fontSize: 13, fontWeight: 700, color: t.paneHeadText, marginEnd: 10, whiteSpace: 'nowrap', ...fontOf(font) }}>{model.title}</Text> : null}
      {model.commands.map((c) => {
        const sep = last !== null && c.group && c.group !== last;
        last = c.group || last;
        return (
          <View key={c.id} style={{ flexDirection: 'row', alignItems: 'center' }}>
            {sep ? <View style={{ width: 1, height: 20, marginHorizontal: 4, backgroundColor: t.separator }} /> : null}
            <Cmd t={t} font={font} c={c} onClick={() => model.actions.command(c.id)} />
          </View>
        );
      })}
    </View>
  );
}

function Item({ t, font, it, onClick }) {
  const [hover, setHover] = useState(false);
  return (
    <View onClick={onClick} onMouseEnter={() => setHover(true)} onMouseLeave={() => setHover(false)}
      style={{ minHeight: 20, flexDirection: 'row', alignItems: 'center', gap: 6, paddingHorizontal: 4, borderRadius: 2, backgroundColor: it.selected ? t.selectBg : 'transparent' }}>
      <Text style={{ flex: 1, fontSize: 12, fontWeight: it.selected ? 700 : 400, color: it.selected ? t.selectText : hover ? t.accent : t.paneLink,
        whiteSpace: 'nowrap', overflow: 'hidden', textOverflow: 'ellipsis', textAlign: 'start', ...fontOf(font) }}>{it.label}</Text>
      {it.detail ? <Text style={{ fontSize: 11, color: it.selected ? t.selectText : t.textDim, ...fontOf(font) }}>{it.detail}</Text> : null}
    </View>
  );
}

// model: { title, items: [{ key, label, detail, selected, group }], actions: { select }, width? }
export function ListPane({ model, width = 220 }) {
  const t = useTheme();
  const font = useThemeFont();
  const groups = [];
  for (const it of model.items) { const g = it.group || ''; let b = groups.find((x) => x.g === g); if (!b) { b = { g, items: [] }; groups.push(b); } b.items.push(it); }
  return (
    <View style={{ width, flex: 1, background: t.paneBg }}>
      <ScrollView style={{ flex: 1 }}>
        <View style={{ paddingBottom: 12 }}>
          {groups.map((b, i) => (
            <View key={b.g || i} style={{ marginTop: 12, marginHorizontal: 10, borderTopLeftRadius: 4, borderTopRightRadius: 4, overflow: 'hidden', boxShadow: '0 1px 3px rgba(0,0,0,0.18)' }}>
              <View style={{ height: 24, justifyContent: 'center', paddingHorizontal: 9, background: t.paneHead }}>
                <Text style={{ fontSize: 12, fontWeight: 700, color: t.paneHeadText, whiteSpace: 'nowrap', overflow: 'hidden', textOverflow: 'ellipsis', textAlign: 'start', ...fontOf(font) }}>
                  {b.g || model.title}</Text>
              </View>
              <View style={{ paddingHorizontal: 6, paddingVertical: 6, gap: 2, backgroundColor: t.paneBody, borderWidth: 1, borderTopWidth: 0, borderColor: t.paneEdge }}>
                {b.items.map((it) => <Item key={it.key} t={t} font={font} it={it} onClick={() => model.actions.select(it.key)} />)}
              </View>
            </View>
          ))}
        </View>
      </ScrollView>
    </View>
  );
}
