// skins/settings.jsx — Bluebell's skin for Settings, in the manner of a control panel of 2001: a task pane on the
// side and, before a section is open, a page of four categories, each a big icon and its sections as links
// (docs/THEMES-GUIDE.md, "App skins → Settings"). Each section's page is Settings' own; the skin only asks Settings to
// open one (model.actions.open), to come back to the categories (home) or to search.
import { View, Text, ScrollView, TextInput, useTheme, useThemeFont, t as tr } from '@lordos/sdk';
import { useState } from 'react';

const fontOf = (font) => (font ? { fontFamily: font } : null);

function Link({ t, font, label, onClick, size = 12, bold, icon }) {
  const [hover, setHover] = useState(false);
  return (
    <View onClick={onClick} onMouseEnter={() => setHover(true)} onMouseLeave={() => setHover(false)} style={{ flexDirection: 'row', alignItems: 'center', gap: 6, minHeight: 18 }}>
      {icon}
      <View style={{ flexShrink: 1 }}>
        <Text style={{ fontSize: size, fontWeight: bold ? 700 : 400, color: hover ? t.accent : t.paneLink, textAlign: 'start', ...fontOf(font) }}>{label}</Text>
        {hover ? <View style={{ height: 1, backgroundColor: t.accent }} /> : null}
      </View>
    </View>
  );
}

function PaneBox({ t, font, title, children }) {
  return (
    <View style={{ marginTop: 12, marginHorizontal: 12, borderTopLeftRadius: 4, borderTopRightRadius: 4, overflow: 'hidden', boxShadow: '0 1px 3px rgba(0,0,0,0.18)' }}>
      <View style={{ height: 25, justifyContent: 'center', paddingHorizontal: 10, background: t.paneHead }}>
        <Text style={{ fontSize: 12, fontWeight: 700, color: t.paneHeadText, whiteSpace: 'nowrap', overflow: 'hidden', textOverflow: 'ellipsis', textAlign: 'start', ...fontOf(font) }}>{title}</Text>
      </View>
      <View style={{ paddingHorizontal: 10, paddingVertical: 8, gap: 5, backgroundColor: t.paneBody, borderWidth: 1, borderTopWidth: 0, borderColor: t.paneEdge }}>{children}</View>
    </View>
  );
}

export function SettingsPane({ model }) {
  const t = useTheme();
  const font = useThemeFont();
  const a = model.actions;
  const Icon = model.SectionIcon;
  const here = model.groups.find((g) => g.sections.some((x) => x.selected));
  return (
    <View style={{ width: 220, background: t.paneBg }}>
      <View style={{ paddingHorizontal: 12, paddingTop: 12 }}>
        <TextInput value={model.query} onChange={a.search} placeholder={tr('settings.search')} size={12} />
      </View>
      <ScrollView style={{ flex: 1 }}>
        <View style={{ paddingBottom: 14 }}>
          <PaneBox t={t} font={font} title={tr('settings.skin.pick')}>
            {model.section ? <Link t={t} font={font} bold label={tr('settings.skin.pick')} onClick={a.home} /> : null}
            {model.groups.map((g) => <Link key={g.id} t={t} font={font} label={g.label} onClick={() => a.open(g.sections[0].id)} />)}
          </PaneBox>
          {here ? (
            <PaneBox t={t} font={font} title={here.label}>
              {here.sections.map((x) => (
                <Link key={x.id} t={t} font={font} label={x.label} bold={x.selected} onClick={() => a.open(x.id)} icon={<Icon color={x.color} glyph={x.glyph} size={16} />} />
              ))}
            </PaneBox>
          ) : null}
          {model.query && !model.groups.length ? <Text style={{ margin: 14, fontSize: 12, color: t.text, ...fontOf(font) }}>{tr('settings.noResults')}</Text> : null}
        </View>
      </ScrollView>
      <View onClick={a.users} style={{ height: 40, flexDirection: 'row', alignItems: 'center', gap: 8, paddingHorizontal: 12, borderTop: `1px solid ${t.paneEdge}` }}>
        <View style={{ width: 24, height: 24, borderRadius: 4, background: 'linear-gradient(180deg, #ffd36b, #e88d1c)', border: '1px solid #ffffff' }} />
        <Text style={{ flex: 1, fontSize: 12, fontWeight: 700, color: '#ffffff', whiteSpace: 'nowrap', overflow: 'hidden', textOverflow: 'ellipsis', textAlign: 'start', ...fontOf(font) }}>{model.user.name}</Text>
      </View>
    </View>
  );
}

export function SettingsHome({ model }) {
  const t = useTheme();
  const font = useThemeFont();
  const a = model.actions;
  const Icon = model.SectionIcon;
  return (
    <View style={{ flex: 1, backgroundColor: t.windowBg }}>
      <ScrollView style={{ flex: 1 }}>
        <View style={{ padding: 24, gap: 18 }}>
          <Text style={{ fontSize: 20, fontWeight: 700, color: t.paneHeadText, textAlign: 'start', ...fontOf(font) }}>{tr('settings.skin.pick')}</Text>
          <View style={{ height: 1, backgroundColor: t.separator }} />
          <View style={{ flexDirection: 'row', flexWrap: 'wrap', gap: 18 }}>
            {model.groups.map((g) => {
              const lead = g.sections[0];
              return (
                <View key={g.id} style={{ width: 300, flexDirection: 'row', gap: 12 }}>
                  <View onClick={() => a.open(lead.id)} style={{ width: 52, height: 52, borderRadius: 8, alignItems: 'center', justifyContent: 'center', backgroundColor: t.paneBody, border: `1px solid ${t.separator}` }}>
                    <Icon color={lead.color} glyph={lead.glyph} size={36} />
                  </View>
                  <View style={{ flex: 1, gap: 4 }}>
                    <Link t={t} font={font} bold size={13} label={g.label} onClick={() => a.open(lead.id)} />
                    {g.sections.map((x) => <Link key={x.id} t={t} font={font} label={x.label} onClick={() => a.open(x.id)} />)}
                  </View>
                </View>
              );
            })}
          </View>
        </View>
      </ScrollView>
    </View>
  );
}

export const settingsSkin = { sidebar: SettingsPane, home: SettingsHome };
