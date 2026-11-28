// skins/store.jsx — Luna's skin for the Store, in the manner of a programs panel of 2001: a column of big square
// buttons on the side (the Store's pages, grouped), and a toolbar with Back, the page's title, refresh and a search field
// (docs/THEMES-GUIDE.md, "App skins → Store"). The pages, the cards and every install stay the Store's.
import { View, Text, ScrollView, TextInput, useTheme, useThemeFont, t as tr, direction } from '@lordos/sdk';
import { useState } from 'react';
import { RoundArrow } from '../glyphs.jsx';

const fontOf = (font) => (font ? { fontFamily: font } : null);
// a drawn mark per group: a box (the Store), a globe (the origins), a star (the picks)
const MARK = { store: ['#ffd36b', '#e88d1c', '▣'], origin: ['#8fdc7a', '#2a7fbf', '◍'], picks: ['#c9b6ff', '#6b4fbb', '★'] };

function BigButton({ t, font, p, onClick }) {
  const [hover, setHover] = useState(false);
  const [a, b, ch] = MARK[p.group] || MARK.store;
  return (
    <View onClick={p.soon ? undefined : onClick} onMouseEnter={() => setHover(true)} onMouseLeave={() => setHover(false)}
      style={{ marginHorizontal: 6, paddingVertical: 6, paddingHorizontal: 4, alignItems: 'center', gap: 3, borderRadius: 3, opacity: p.soon ? 0.5 : 1,
        border: `1px solid ${p.selected ? '#ffffff' : hover && !p.soon ? 'rgba(255,255,255,0.6)' : 'transparent'}`,
        backgroundColor: p.selected ? 'rgba(255,255,255,0.28)' : hover && !p.soon ? 'rgba(255,255,255,0.14)' : 'transparent' }}>
      <View style={{ width: 30, height: 30, borderRadius: 5, alignItems: 'center', justifyContent: 'center', background: `linear-gradient(180deg, ${a}, ${b})`, border: '1px solid rgba(0,0,0,0.25)' }}>
        <Text style={{ fontSize: 15, color: '#ffffff' }}>{ch}</Text>
        {p.count ? <View style={{ position: 'absolute', right: -6, top: -5, minWidth: 16, height: 14, borderRadius: 7, paddingHorizontal: 3, backgroundColor: '#c8302a', alignItems: 'center', justifyContent: 'center' }}>
          <Text style={{ fontSize: 9, fontWeight: 700, color: '#ffffff' }}>{p.count > 999 ? '999+' : p.count}</Text></View> : null}
      </View>
      <Text style={{ fontSize: 11, color: '#ffffff', textAlign: 'center', ...fontOf(font) }}>{p.soon ? `${p.label} · ${tr('store.soon')}` : p.label}</Text>
    </View>
  );
}

export function StoreSide({ model }) {
  const t = useTheme();
  const font = useThemeFont();
  const groups = ['store', 'origin', 'picks'];
  return (
    <View style={{ width: 118, background: t.paneBg, borderEndWidth: 1, borderEndColor: t.frameEdge }}>
      <ScrollView style={{ flex: 1 }}>
        <View style={{ paddingVertical: 8, gap: 2 }}>
          {groups.map((g, i) => (
            <View key={g} style={{ gap: 2 }}>
              {i ? <View style={{ height: 1, marginVertical: 6, marginHorizontal: 12, backgroundColor: 'rgba(255,255,255,0.4)' }} /> : null}
              {model.pages.filter((p) => p.group === g).map((p) => <BigButton key={p.id} t={t} font={font} p={p} onClick={() => model.actions.go(p.id)} />)}
            </View>
          ))}
        </View>
      </ScrollView>
    </View>
  );
}

export function StoreToolbar({ model }) {
  const t = useTheme();
  const font = useThemeFont();
  const a = model.actions;
  return (
    <View style={{ height: 42, flexDirection: 'row', alignItems: 'center', gap: 8, paddingHorizontal: 8, background: t.toolbarBg, borderBottom: `1px solid ${t.separator}` }}>
      {model.inApp ? <View onClick={a.back}><RoundArrow size={26} dir={direction() === 'rtl' ? 'right' : 'left'} /></View> : null}
      <Text style={{ flex: 1, minWidth: 0, fontSize: 15, fontWeight: 700, color: t.paneHeadText, whiteSpace: 'nowrap', overflow: 'hidden', textOverflow: 'ellipsis', textAlign: 'start', ...fontOf(font) }}>{model.title}</Text>
      {model.refresh ? <View onClick={model.refresh.busy ? undefined : a.refresh} accessibilityLabel={model.refresh.title} style={{ opacity: model.refresh.busy ? 0.5 : 1 }}><RoundArrow size={22} dir="right" /></View> : null}
      {!model.inApp ? <View style={{ width: 200 }}><TextInput value={model.query} onChange={a.search} placeholder={model.searchHint} size={12} /></View> : null}
    </View>
  );
}

export const storeSkin = { sidebar: StoreSide, toolbar: StoreToolbar };
