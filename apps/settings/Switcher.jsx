// Settings → Keyboard: the app switcher (sdk/appswitcher.jsx, sdk/switchermodel.js), the same in every theme.
import { useSettings, Toggle, Segmented, TextInput, themes, runningApps, switcherPrefs, validColor, useLauncherApps } from '@lordos/sdk';
import { useState } from 'react';
import { Section, SettingRow } from './ui.jsx';

function ColorRow({ label, value, onChange }) {
  const [text, setText] = useState(value);
  return (
    <SettingRow label={label}>
      <TextInput value={text} onChange={(v) => { setText(v); if (validColor(v)) onChange(v); }} placeholder="#000000" />
    </SettingRow>
  );
}

export function SwitcherSettings({ tr }) {
  const [s, set] = useSettings();
  const p = switcherPrefs(s.switcher);
  const put = (patch) => set({ switcher: { ...p, ...patch } });
  const opts = (key, ids) => ids.map((id) => ({ value: id, label: tr(`settings.kb.switcher.${key}.${id}`) }));
  const open = runningApps();
  const apps = useLauncherApps();
  const rows = [...open.map((g) => ({ key: g.key, name: g.name })),
    ...p.excluded.filter((k) => !open.some((g) => g.key === k)).map((k) => ({ key: k, name: apps.find((a) => a.id === k)?.name || k }))];
  const classic = themes.current() !== 'default';
  const metaInClassic = classic && (p.keys === 'meta' || p.keys === 'both');
  return (
    <>
      <Section title={tr('settings.kb.switcher.title')} footer={metaInClassic ? `${tr('settings.kb.switcher.hint')} ${tr('settings.kb.switcher.hintOverview')}` : tr('settings.kb.switcher.hint')}>
        <SettingRow label={tr('settings.kb.switcher.keys')}><Segmented value={p.keys} onChange={(v) => put({ keys: v })} options={opts('keys', ['theme', 'meta', 'alt', 'both'])} /></SettingRow>
        <SettingRow label={tr('settings.kb.switcher.scope')}><Segmented value={p.scope} onChange={(v) => put({ scope: v })} options={opts('scope', ['all', 'desktop', 'display'])} /></SettingRow>
        <SettingRow label={tr('settings.kb.switcher.size')}><Segmented value={p.size} onChange={(v) => put({ size: v })} options={opts('size', ['s', 'm', 'l'])} /></SettingRow>
        <SettingRow label={tr('settings.kb.switcher.skin')}><Segmented value={p.skin} onChange={(v) => put({ skin: v })} options={opts('skin', ['system', 'glass', 'solid', 'contrast', 'custom'])} /></SettingRow>
        {p.skin === 'custom' && <ColorRow label={tr('settings.kb.switcher.customBg')} value={p.customBg} onChange={(v) => put({ customBg: v })} />}
        {p.skin === 'custom' && <ColorRow label={tr('settings.kb.switcher.customAccent')} value={p.customAccent} onChange={(v) => put({ customAccent: v })} />}
        <SettingRow label={tr('settings.kb.switcher.numbers')}><Toggle value={p.numbers} onChange={(v) => put({ numbers: v })} /></SettingRow>
        <SettingRow label={tr('settings.kb.switcher.windows')}><Toggle value={p.windows} onChange={(v) => put({ windows: v })} /></SettingRow>
        <SettingRow label={tr('settings.kb.switcher.hideMinimized')}><Toggle value={p.hideMinimized} onChange={(v) => put({ hideMinimized: v })} /></SettingRow>
      </Section>
      {rows.length > 0 && (
        <Section title={tr('settings.kb.switcher.excluded')} footer={tr('settings.kb.switcher.excludedHint')}>
          {rows.map((r) => (
            <SettingRow key={r.key} label={r.name}>
              <Toggle value={!p.excluded.includes(r.key)} onChange={(v) => put({ excluded: v ? p.excluded.filter((k) => k !== r.key) : [...p.excluded, r.key] })} />
            </SettingRow>
          ))}
        </Section>
      )}
    </>
  );
}
