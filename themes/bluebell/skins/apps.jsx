// skins/apps.jsx — Bluebell's skins for the apps whose regions take the shared shapes (a command bar, a list pane;
// docs/THEMES-GUIDE.md "App skins → the shared shapes"): each region is drawn by skins/common.jsx. Apps with their own
// shapes have their own file (files, settings, terminal, editor, store).
import { View, Text, TextInput, useTheme, useThemeFont, useSettings } from '@lordos/sdk';
import { makeTheme } from '../tokens.js';
import { StartMark } from '../glyphs.jsx';
import { useState } from 'react';
import { CommandBar, ListPane } from './common.jsx';

// a list pane with the app's search field above it (Notes)
function SearchPane({ model }) {
  const t = useTheme();
  return (
    <View style={{ width: 240, background: t.paneBg }}>
      <View style={{ paddingHorizontal: 10, paddingTop: 10 }}><TextInput value={model.query} onChange={model.actions.search} placeholder={model.title} size={12} /></View>
      <ListPane model={model} width={240} style={{ flex: 1 }} />
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

// ---- Player: a transport bar of round blue buttons and a green progress strip, as on a media player of the era ----
function Transport({ model }) {
  const t = useTheme();
  const font = useThemeFont();
  const f = model.duration ? Math.min(1, model.position / model.duration) : 0;
  return (
    <View style={{ height: 64, flexDirection: 'row', alignItems: 'center', gap: 12, paddingHorizontal: 14, background: t.barBg }}>
      <View style={{ flexDirection: 'row', gap: 6, direction: 'ltr' }}>
        {model.commands.map((c) => (
          <View key={c.id} onClick={() => model.actions.command(c.id)} style={{ width: c.id === 'toggle' ? 40 : 30, height: c.id === 'toggle' ? 40 : 30, borderRadius: 20, alignItems: 'center', justifyContent: 'center',
            background: c.active ? 'linear-gradient(180deg, #8fdc7a, #2f8a25)' : 'linear-gradient(180deg, #8ec5ff, #2f63d8)', border: '1px solid rgba(255,255,255,0.8)' }}>
            <Text style={{ fontSize: c.id === 'toggle' ? 15 : 12, color: '#ffffff' }}>{c.label}</Text>
          </View>
        ))}
      </View>
      <View style={{ flex: 1, minWidth: 0, gap: 5 }}>
        <Text style={{ fontSize: 12, fontWeight: 700, color: '#ffffff', whiteSpace: 'nowrap', overflow: 'hidden', textOverflow: 'ellipsis', textAlign: 'start', ...(font ? { fontFamily: font } : null) }}>{model.title}</Text>
        <View style={{ height: 8, borderRadius: 4, backgroundColor: 'rgba(0,0,0,0.35)', direction: 'ltr', overflow: 'hidden' }}>
          <View style={{ width: `${Math.round(f * 100)}%`, height: 8, background: 'linear-gradient(180deg, #8fdc7a, #2f8a25)' }} />
        </View>
      </View>
    </View>
  );
}

// ---- a command bar whose commands are tabs (Monitor): property-sheet tabs under the title, as in the era's dialogs ----
function TabStrip({ model }) {
  const t = useTheme();
  const font = useThemeFont();
  return (
    <View style={{ background: t.toolbarBg, borderBottom: `1px solid ${t.separator}` }}>
      {model.title ? <Text style={{ fontSize: 13, fontWeight: 700, color: t.paneHeadText, paddingHorizontal: 10, paddingTop: 6, textAlign: 'start', ...(font ? { fontFamily: font } : null) }}>{model.title}</Text> : null}
      <View style={{ height: 28, flexDirection: 'row', alignItems: 'flex-end', flexWrap: 'wrap', gap: 2, paddingHorizontal: 8 }}>
        {model.commands.map((c) => (
          <View key={c.id} onMouseDown={() => model.actions.command(c.id)} style={{ height: c.active ? 25 : 22, marginTop: c.active ? 0 : 3, justifyContent: 'center', paddingHorizontal: 10,
            borderTopLeftRadius: 3, borderTopRightRadius: 3, borderWidth: 1, borderBottomWidth: 0, borderColor: t.separator, borderTopColor: c.active ? '#e8a33c' : t.separator,
            borderTopWidth: c.active ? 2 : 1, backgroundColor: c.active ? t.windowBg : t.sidebarBg }}>
            <Text style={{ fontSize: 12, fontWeight: c.active ? 700 : 400, color: t.text, whiteSpace: 'nowrap', ...(font ? { fontFamily: font } : null) }}>{c.label}</Text>
          </View>
        ))}
      </View>
    </View>
  );
}

// ---- Screenshots: the capture bar as a compact blue bar of small square buttons and a green Capture button ----
const SHOT_GLYPH = { close: '✕', full: '▭', window: '❐', area: '⬚', recordFull: '●', recordArea: '◍', color: '◉', text: 'T' };
function ShotBar({ model }) {
  // the capture bar floats outside the theme's tree (the SDK's floating layer): Bluebell's tokens are made here
  const [st] = useSettings();
  const t = makeTheme(st);
  const font = useThemeFont();
  return (
    <View style={{ flex: 1, flexDirection: 'row', alignItems: 'center', gap: 4, paddingHorizontal: 8, borderRadius: 6, background: t.barBg, border: `1px solid ${t.frameEdge}`, direction: 'ltr' }}>
      {model.commands.map((c) => (c.id === 'primary' ? (
        <View key={c.id} onClick={() => model.actions.command(c.id)} style={{ height: 30, marginStart: 6, paddingHorizontal: 12, borderRadius: 4, justifyContent: 'center', background: t.startBg, border: '1px solid rgba(255,255,255,0.7)' }}>
          <Text style={{ fontSize: 13, fontWeight: 700, color: '#ffffff', whiteSpace: 'nowrap', ...(font ? { fontFamily: font } : null) }}>{c.label}</Text>
        </View>
      ) : (
        <View key={c.id} onClick={() => model.actions.command(c.id)} accessibilityLabel={c.label}
          style={{ width: 30, height: 30, borderRadius: 4, alignItems: 'center', justifyContent: 'center', border: `1px solid ${c.active ? '#ffffff' : 'rgba(255,255,255,0.3)'}`,
            background: c.active ? 'linear-gradient(180deg, #8ec5ff, #2f63d8)' : 'rgba(255,255,255,0.08)' }}>
          <Text style={{ fontSize: 14, color: c.group === 'record' ? '#ff9b8a' : '#ffffff' }}>{SHOT_GLYPH[c.id] || '•'}</Text>
        </View>
      )))}
    </View>
  );
}

// ---- About: a blue banner with the bluebell and the version, as on an "about" box of the era ----
function AboutBanner({ model }) {
  const t = useTheme();
  const font = useThemeFont();
  const f = font ? { fontFamily: font } : null;
  return (
    <View style={{ alignSelf: 'stretch', marginHorizontal: -20, marginTop: -20, marginBottom: 8 }}>
      <View style={{ height: 96, flexDirection: 'row', alignItems: 'center', gap: 16, paddingHorizontal: 24, background: t.titleBg }}>
        <View style={{ width: 60, height: 60, borderRadius: 30, backgroundColor: 'rgba(255,255,255,0.92)', alignItems: 'center', justifyContent: 'center' }}><StartMark size={44} /></View>
        <View style={{ flex: 1, gap: 3 }}>
          <Text style={{ fontSize: 22, fontWeight: 700, color: '#ffffff', textAlign: 'start', ...f }}>LordOS</Text>
          <Text style={{ fontSize: 12, color: '#d8e4f8', textAlign: 'start', ...f }}>{[model.version, model.build].filter(Boolean).join(' · ')}</Text>
        </View>
      </View>
      <View style={{ height: 3, background: 'linear-gradient(90deg, #f0a040, #ffd36b)' }} />
      <View style={{ paddingHorizontal: 24, paddingTop: 10, gap: 4 }}>
        <Text style={{ fontSize: 12, fontWeight: 700, color: t.paneHeadText, textAlign: 'start', ...f }}>{model.code}</Text>
        {model.codeNote ? <Text style={{ fontSize: 11, color: t.textDim, textAlign: 'start', ...f }}>{model.codeNote}</Text> : null}
        <Text style={{ fontSize: 12, color: t.text, textAlign: 'start', ...f }}>{model.tagline}</Text>
      </View>
    </View>
  );
}

export const appSkins = {
  logs: { sidebar: (p) => <ListPane {...p} width={210} />, toolbar: CommandBar },
  fonts: { sidebar: SearchPane },
  characters: { toolbar: CommandBar, sidebar: (p) => <ListPane {...p} width={180} /> },
  help: { sidebar: SearchPane },
  about: { banner: AboutBanner },
  disks: { sidebar: ListPane },
  screenshots: { toolbar: ShotBar },
  clock: { toolbar: CommandBar },
  monitor: { toolbar: TabStrip },
  player: { sidebar: ListPane, toolbar: CommandBar, transport: Transport },
  photos: { sidebar: ListPane, toolbar: CommandBar },
  calculator: { display: CalcDisplay, keypad: CalcKeypad },
  notes: { sidebar: SearchPane, toolbar: CommandBar },
};
