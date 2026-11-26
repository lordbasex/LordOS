// skins/apps.jsx — Bluebell's skins for the apps whose regions take the shared shapes (a command bar, a list pane;
// docs/THEMES-GUIDE.md "App skins → the shared shapes"): each region is drawn by skins/common.jsx. Apps with their own
// shapes have their own file (files, settings, terminal, editor, store).
import { View, Text, TextInput, useTheme, useThemeFont } from '@lordos/sdk';
import { useState } from 'react';
import { CommandBar, ListPane } from './common.jsx';

// a list pane with the app's search field above it (Notes)
function SearchPane({ model }) {
  const t = useTheme();
  return (
    <View style={{ width: 240, background: t.paneBg }}>
      <View style={{ paddingHorizontal: 10, paddingTop: 10 }}><TextInput value={model.query} onChange={model.actions.search} placeholder={model.title} size={12} /></View>
      <ListPane model={model} width={240} />
    </View>
  );
}

// ---- Calculator: a sunken white display and raised grey keys with coloured labels, as on a desk calculator of the era ----
function CalcDisplay({ model }) {
  const t = useTheme();
  const font = useThemeFont();
  return (
    <View style={{ height: 70, justifyContent: 'flex-end', alignItems: 'flex-end', paddingHorizontal: 8, marginBottom: 6, backgroundColor: t.inputBg, direction: 'ltr',
      borderWidth: 2, borderTopColor: '#808080', borderLeftColor: '#808080', borderBottomColor: '#ffffff', borderRightColor: '#ffffff' }}>
      <Text style={{ fontSize: 12, color: t.textDim, whiteSpace: 'nowrap', ...(font ? { fontFamily: font } : null) }}>{model.expression || ' '}</Text>
      <Text style={{ fontSize: 28, color: t.text, whiteSpace: 'nowrap', ...(font ? { fontFamily: font } : null) }}>{model.result}</Text>
    </View>
  );
}
function CalcKey({ k, onPress }) {
  const [down, setDown] = useState(false);
  const color = k.kind === 'operator' ? '#c8302a' : k.kind === 'function' ? '#7a1f8a' : k.kind === 'equals' ? '#c8302a' : '#0a246a';
  return (
    <View onMouseDown={() => setDown(true)} onMouseUp={() => setDown(false)} onMouseLeave={() => setDown(false)} onClick={onPress}
      style={{ flex: 1, alignItems: 'center', justifyContent: 'center', borderRadius: 3, background: down ? 'linear-gradient(180deg, #d9d6c9, #f2f1ea)' : 'linear-gradient(180deg, #ffffff, #e3e0d4)',
        borderWidth: 1, borderTopColor: down ? '#808080' : '#ffffff', borderLeftColor: down ? '#808080' : '#ffffff', borderBottomColor: down ? '#ffffff' : '#808080', borderRightColor: down ? '#ffffff' : '#808080' }}>
      <Text style={{ fontSize: 18, fontWeight: 700, color }}>{k.label}</Text>
    </View>
  );
}
function CalcKeypad({ model }) {
  return (
    <View style={{ flex: 1, gap: 6, direction: 'ltr' }}>
      {model.keys.map((row, r) => <View key={r} style={{ flex: 1, flexDirection: 'row', gap: 6 }}>{row.map((k) => <CalcKey key={k.id} k={k} onPress={() => model.actions.press(k.id)} />)}</View>)}
    </View>
  );
}

export const appSkins = {
  photos: { sidebar: ListPane, toolbar: CommandBar },
  calculator: { display: CalcDisplay, keypad: CalcKeypad },
  notes: { sidebar: SearchPane, toolbar: CommandBar },
};
