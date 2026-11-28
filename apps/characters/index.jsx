// Emoji & Symbols: the window version of the system picker (❖.): emoji by category with search in
// Spanish, English, Portuguese, French and Italian, skin tones and recents, plus the Unicode symbols by block. A
// click picks one; "Copy" puts it on the clipboard.
import {
  View, Text, Button, Segmented, ScrollView, SidebarItem, Toolbar, ToolbarSpacer, Hairline, SearchField, EmptyState, useTheme, clipboard, emoji,
  EmojiGrid, EmojiToneBar, typeScale, t as tr, useSidebar, SidebarToggle, SidebarOverlay, SkinRegion, useSkinned, commandBar, pickCommand, listPane, pickItem,
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
function Preview({ ch, th, narrow }) {
  const edge = <Hairline edge="start" />;
  // a narrow window: the picked character in a strip under the grid (nothing until there is one)
  if (narrow) {
    if (!ch) return null;
    const cps = [...ch].map((c) => `U+${c.codePointAt(0).toString(16).toUpperCase().padStart(4, '0')}`).join(' ');
    const name = emoji.name(ch);
    return (
      <View style={{ flexDirection: 'row', alignItems: 'center', gap: 12, paddingHorizontal: 12, paddingVertical: 8, backgroundColor: th.sidebarBg }}>
        <Hairline edge="top" />
        <View style={{ width: 52, height: 52, borderRadius: 10, alignItems: 'center', justifyContent: 'center',
          backgroundColor: th.mode === 'dark' ? 'rgba(255,255,255,0.06)' : '#ffffff', border: `1px solid ${th.separator}` }}>
          <Text style={{ fontSize: 30, color: th.text, lineHeight: 44 }}>{ch}</Text>
        </View>
        <View style={{ flex: 1, minWidth: 0, gap: 2 }}>
          {name ? <Text style={{ fontSize: typeScale.headline, fontWeight: 600, color: th.text, textAlign: 'start', whiteSpace: 'nowrap', overflow: 'hidden', textOverflow: 'ellipsis' }}>{name}</Text> : null}
          <Text style={{ fontSize: typeScale.caption, color: th.textDim, textAlign: 'start', direction: 'ltr' }}>{cps}</Text>
        </View>
        <Button primary label={tr('characters.copy')} onClick={() => clipboard.set(ch)} />
      </View>
    );
  }
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
  const sb = useSidebar(640);
  const groups = emoji.groups();
  const list = useMemo(() => (q.trim() ? emoji.search(q, 400) : group === 'recent' ? emoji.recents() : groups.find((g) => g.id === group)?.list || []), [q, group]);
  const chars = useMemo(() => {
    const [, a, b] = SYMBOL_BLOCKS[block];
    const out = [];
    for (let c = a; c <= b; c++) out.push(String.fromCodePoint(c));
    return out.filter(emoji.drawable);         // no missing glyph boxes (#56)
  }, [block]);
  const choose = (ch) => { setPick(ch); if (tab === 'emoji') emoji.remember(ch.replace(/[\u{1F3FB}-\u{1F3FF}]/gu, '')); };
  const side = tab === 'emoji'
    ? [['recent', tr('emoji.group.recent')], ...groups.map((g) => [g.id, tr(`emoji.group.${g.id}`)])]
    : SYMBOL_BLOCKS.map((b, i) => [i, tr(`characters.block.${b[0]}`)]);
  // picking a category in a narrow window closes the sidebar over the grid
  const sidebar = (
    <ScrollView resetKey={tab} style={{ flex: 1, paddingHorizontal: 10, paddingTop: 10 }}>
      {side.map(([id, label]) => {
        const on = tab === 'emoji' ? (!q.trim() && group === id) : block === id;
        return <SidebarItem key={id} label={label} selected={on} onClick={() => { if (tab === 'emoji') { setQ(''); setGroup(id); } else setBlock(id); if (sb.narrow) sb.close(); }} />;
      })}
      <View style={{ height: 10 }} />
    </ScrollView>
  );
  // (0.8.9.6.2) app skins (docs/THEMES-GUIDE.md, "App skins → the shared shapes"): the two tabs as a command bar and the
  // groups or blocks as a list pane; the grid, the search and the copying stay Characters'
  const skinned = useSkinned('characters');
  const sk = skinned ? (() => {
    const bar = commandBar({ commands: [['emoji', 'characters.tab.emoji'], ['symbols', 'characters.tab.symbols']].map(([id, k]) => ({ id, label: tr(k), active: tab === id })) });
    const pane = listPane({ title: tr(tab === 'emoji' ? 'characters.tab.emoji' : 'characters.tab.symbols'),
      items: side.map(([id, label]) => ({ key: String(id), label, selected: tab === 'emoji' ? (!q.trim() && group === id) : block === id })) });
    return {
      toolbar: { app: 'characters', ...bar, actions: { command: (id) => { const c = pickCommand(bar, id); if (c) { setTab(c); setPick(null); } } } },
      sidebar: { app: 'characters', ...pane, actions: { select: (key) => { const k = pickItem(pane, key); if (k == null) return; if (tab === 'emoji') { setQ(''); setGroup(k); } else setBlock(Number(k)); } } },
    };
  })() : null;
  return (
    <View style={{ flex: 1 }}>
      <SkinRegion app="characters" region="toolbar" model={sk && sk.toolbar}>
      <Toolbar>
        <SidebarToggle shown={sb.shown} onClick={sb.toggle} />
        <Segmented appearance="neutral" value={tab} onChange={(v) => { setTab(v); setPick(null); }}
          options={[['emoji', tr('characters.tab.emoji')], ['symbols', tr('characters.tab.symbols')]]} />
        <ToolbarSpacer />
        {tab === 'emoji' && <EmojiToneBar tone={tone} onTone={(n) => { setTone(n); emoji.setTone(n); }} th={th} size={sb.narrow ? 18 : 22} />}
        {tab === 'emoji' && <SearchField value={q} onChange={setQ} placeholder={tr('emoji.search')}
          width={sb.narrow ? undefined : 180} style={sb.narrow ? { flex: 1, minWidth: 80, maxWidth: 180 } : null} />}
      </Toolbar>
      </SkinRegion>
      <View style={{ flex: 1, flexDirection: 'row' }}>
        {sb.shown && !sb.narrow ? <SkinRegion app="characters" region="sidebar" model={sk && sk.sidebar}><View style={{ width: 180, backgroundColor: th.sidebarBg }}>{sidebar}<Hairline edge="end" /></View></SkinRegion> : null}
        <View style={{ flex: 1, minWidth: 0 }}>
          <View style={{ flex: 1, minWidth: 0, direction: 'ltr' }}>
            {tab === 'emoji' ? (
              list.length ? (
                <ScrollView resetKey={`${group}|${q}`} style={{ flex: 1, padding: 12 }}>
                  <EmojiGrid list={list} sel={-1} picked={pick} onPick={choose} tone={tone} size={44} th={th} wrap />
                </ScrollView>
              ) : <EmptyState title={group === 'recent' && !q.trim() ? tr('emoji.noRecent') : tr('emoji.none')} />
            ) : (
              <ScrollView resetKey={block} style={{ flex: 1, padding: 12 }}>
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
          {sb.narrow ? <Preview ch={pick} th={th} narrow /> : null}
        </View>
        {sb.narrow ? null : <Preview ch={pick} th={th} />}
        {sb.shown && sb.narrow ? <SidebarOverlay width={200} onClose={sb.close}>{sidebar}</SidebarOverlay> : null}
      </View>
    </View>
  );
}
