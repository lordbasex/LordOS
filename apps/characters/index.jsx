// Emoji & Symbols: the window version of the system picker (❖.): emoji by category with search in
// Spanish, English, Portuguese and French, skin tones and recents, plus the Unicode symbols by block. A
// click picks one; "Copy" puts it on the clipboard.
import {
  View, Text, Button, Segmented, ScrollView, SidebarItem, Toolbar, ToolbarSpacer, Hairline, SearchField, EmptyState, useTheme, clipboard, emoji,
  EmojiGrid, EmojiToneBar, typeScale, t as tr,
} from '@lordos/sdk';
import { useMemo, useState } from 'react';

export function Icon({ size: s }) {
  return (
    <View style={{ width: s, height: s, borderRadius: s * 0.225, background: 'linear-gradient(160deg, #5ac8fa, #5856d6)',
      boxShadow: `0 ${s * 0.04}px ${s * 0.12}px rgba(0,0,0,0.35)`, overflow: 'hidden' }}>
      {/* our own smiley (a circle and its features) and a symbol */}
      <View style={{ position: 'absolute', left: s * 0.14, top: s * 0.16, width: s * 0.5, height: s * 0.5, borderRadius: s * 0.25, backgroundColor: '#ffd60a' }} />
      <View style={{ position: 'absolute', left: s * 0.27, top: s * 0.3, width: s * 0.06, height: s * 0.09, borderRadius: s * 0.03, backgroundColor: '#3a2a00' }} />
      <View style={{ position: 'absolute', left: s * 0.45, top: s * 0.3, width: s * 0.06, height: s * 0.09, borderRadius: s * 0.03, backgroundColor: '#3a2a00' }} />
      <View style={{ position: 'absolute', left: s * 0.27, top: s * 0.47, width: s * 0.24, height: s * 0.07, borderRadius: s * 0.04, backgroundColor: '#3a2a00' }} />
      <Text style={{ position: 'absolute', left: s * 0.56, top: s * 0.46, fontSize: s * 0.36, fontWeight: 700, color: '#ffffff' }}>Ω</Text>
    </View>
  );
}

// symbol blocks (named in characters.block.<id>)
const SYMBOL_BLOCKS = [
  ['arrows', 0x2190, 0x21ff], ['math', 0x2200, 0x22ff], ['technical', 0x2300, 0x23ff], ['currency', 0x20a0, 0x20c0], ['letterlike', 0x2100, 0x214f],
  ['numberForms', 0x2150, 0x218b], ['punctuation', 0x2010, 0x205e], ['superSub', 0x2070, 0x209c], ['enclosed', 0x2460, 0x24ff], ['boxDrawing', 0x2500, 0x257f],
  ['blockElements', 0x2580, 0x259f], ['shapes', 0x25a0, 0x25ff], ['miscSymbols', 0x2600, 0x26ff], ['dingbats', 0x2700, 0x27bf], ['suppArrows', 0x27f0, 0x27ff],
  ['braille', 0x2800, 0x28ff], ['greek', 0x391, 0x3c9], ['latin1', 0xa1, 0xff], ['cjkSymbols', 0x3000, 0x303f], ['halfwidth', 0xff01, 0xff5e],
];

// Inspector of the picked character (an empty state until there is one), with a hairline on its
// leading edge.
const PREVIEW_W = 200;
function Preview({ ch, th }) {
  const edge = <Hairline edge="start" />;
  if (!ch) {
    return (
      <View style={{ width: PREVIEW_W, backgroundColor: th.sidebarBg }}>
        {edge}
        <EmptyState icon="☺" title={tr('characters.pickTitle')} hint={tr('characters.pickHint')} style={{ padding: 16 }} />
      </View>
    );
  }
  const cps = [...ch].map((c) => `U+${c.codePointAt(0).toString(16).toUpperCase().padStart(4, '0')}`).join(' ');
  const name = emoji.name(ch);
  return (
    <View style={{ width: PREVIEW_W, alignItems: 'center', gap: 8, paddingHorizontal: 16, paddingTop: 24, backgroundColor: th.sidebarBg }}>
      {edge}
      <View style={{ width: 128, height: 128, borderRadius: 14, alignItems: 'center', justifyContent: 'center', marginBottom: 6,
        backgroundColor: th.mode === 'dark' ? 'rgba(255,255,255,0.06)' : '#ffffff', border: `1px solid ${th.separator}` }}>
        <Text style={{ fontSize: 72, color: th.text, lineHeight: 110 }}>{ch}</Text>
      </View>
      {name ? <Text style={{ fontSize: typeScale.headline, fontWeight: 600, color: th.text, textAlign: 'center' }}>{name}</Text> : null}
      <Text style={{ fontSize: typeScale.caption, color: th.textDim, textAlign: 'center', direction: 'ltr' }}>{cps}</Text>
      <Button primary label={tr('characters.copy')} onClick={() => clipboard.set(ch)} style={{ marginTop: 8 }} />
    </View>
  );
}

export default function Characters() {
  const th = useTheme();
  const [tab, setTab] = useState('emoji');
  const [q, setQ] = useState('');
  const [group, setGroup] = useState(() => (emoji.recents().length ? 'recent' : 'smileys'));
  const [block, setBlock] = useState(0);
  const [tone, setTone] = useState(emoji.tone());
  const [pick, setPick] = useState(null);
  const groups = emoji.groups();
  const list = useMemo(() => (q.trim() ? emoji.search(q, 400) : group === 'recent' ? emoji.recents() : groups.find((g) => g.id === group)?.list || []), [q, group]);
  const chars = useMemo(() => {
    const [, a, b] = SYMBOL_BLOCKS[block];
    const out = [];
    for (let c = a; c <= b; c++) out.push(String.fromCodePoint(c));
    return out;
  }, [block]);
  const choose = (ch) => { setPick(ch); if (tab === 'emoji') emoji.remember(ch.replace(/[\u{1F3FB}-\u{1F3FF}]/gu, '')); };
  const side = tab === 'emoji'
    ? [['recent', tr('emoji.group.recent')], ...groups.map((g) => [g.id, tr(`emoji.group.${g.id}`)])]
    : SYMBOL_BLOCKS.map((b, i) => [i, tr(`characters.block.${b[0]}`)]);
  return (
    <View style={{ flex: 1 }}>
      <Toolbar>
        <Segmented appearance="neutral" value={tab} onChange={(v) => { setTab(v); setPick(null); }}
          options={[['emoji', tr('characters.tab.emoji')], ['symbols', tr('characters.tab.symbols')]]} />
        <ToolbarSpacer />
        {tab === 'emoji' && <EmojiToneBar tone={tone} onTone={(n) => { setTone(n); emoji.setTone(n); }} th={th} />}
        {tab === 'emoji' && <SearchField value={q} onChange={setQ} placeholder={tr('emoji.search')} width={180} />}
      </Toolbar>
      <View style={{ flex: 1, flexDirection: 'row' }}>
        <View style={{ width: 180, backgroundColor: th.sidebarBg }}>
          <ScrollView style={{ flex: 1, paddingHorizontal: 10, paddingTop: 10 }}>
            {side.map(([id, label]) => {
              const on = tab === 'emoji' ? (!q.trim() && group === id) : block === id;
              return <SidebarItem key={id} label={label} selected={on} onClick={() => { if (tab === 'emoji') { setQ(''); setGroup(id); } else setBlock(id); }} />;
            })}
            <View style={{ height: 10 }} />
          </ScrollView>
          <Hairline edge="end" />
        </View>
        <View style={{ flex: 1, minWidth: 0, direction: 'ltr' }}>
          {tab === 'emoji' ? (
            list.length ? (
              <ScrollView style={{ flex: 1, padding: 12 }}>
                <EmojiGrid list={list} sel={-1} picked={pick} onPick={choose} tone={tone} size={44} th={th} wrap />
              </ScrollView>
            ) : <EmptyState title={group === 'recent' && !q.trim() ? tr('emoji.noRecent') : tr('emoji.none')} />
          ) : (
            <ScrollView style={{ flex: 1, padding: 12 }}>
              <View style={{ flexDirection: 'row', flexWrap: 'wrap' }}>
                {chars.map((ch) => (
                  <View key={ch} onClick={() => choose(ch)} style={{ width: 44, height: 44, alignItems: 'center', justifyContent: 'center', borderRadius: 8,
                    backgroundColor: pick === ch ? th.accent : 'transparent' }}>
                    <Text style={{ fontSize: 20, color: pick === ch ? '#fff' : th.text }}>{ch}</Text>
                  </View>
                ))}
              </View>
            </ScrollView>
          )}
        </View>
        <Preview ch={pick} th={th} />
      </View>
    </View>
  );
}
