// Settings → Clipboard: saved history, limits, privacy (sdk/clipboard.js).
import {
  View, Text, Button, Toggle, Stepper, TextInput, useTheme, useLocale, useSettings, useStore, clipboard, clipboardHistory, openClipboardPanel,
  spacing, typeScale,
} from '@lordos/sdk';
import { useState } from 'react';
import { Stack, Section, SettingRow, Note, Actions } from './ui.jsx';

const DEFAULTS = { save: true, max: 500, maxMB: 200, expireDays: 30, ignoreApps: [], clearOnLogout: false };

export function ClipboardSettings() {
  const t = useTheme();
  const { t: tr } = useLocale();
  const [s, set] = useSettings();
  const c = { ...DEFAULTS, ...(s.clipboard || {}) };
  const put = (patch) => set({ clipboard: { ...c, ...patch } });
  const list = useStore(clipboardHistory);
  const [app, setApp] = useState('');
  const origins = [...new Set(list.map((x) => x.origin).filter((o) => o && !['lordos', 'terminal', 'linux'].includes(o) && !c.ignoreApps.includes(o)))].slice(0, 8);
  const addApp = () => { if (app && !c.ignoreApps.includes(app)) put({ ignoreApps: [...c.ignoreApps, app] }); setApp(''); };
  return (
    <Stack>
      <Section>
        <SettingRow label={tr('settings.clipboard.save')} hint={tr('settings.clipboard.saveHint')}>
          <Toggle small on={c.save} onChange={(save) => put({ save })} />
        </SettingRow>
        <SettingRow label={tr('settings.clipboard.clearOnLogout')}>
          <Toggle small on={c.clearOnLogout} onChange={(clearOnLogout) => put({ clearOnLogout })} />
        </SettingRow>
      </Section>
      <Section>
        <SettingRow label={tr('settings.clipboard.max')}>
          <Stepper value={c.max} min={50} max={5000} step={50} onChange={(max) => put({ max })} width={60} />
        </SettingRow>
        <SettingRow label={tr('settings.clipboard.maxMB')} hint={tr('settings.clipboard.maxMBHint')}>
          <Stepper value={c.maxMB} min={20} max={2000} step={20} onChange={(maxMB) => put({ maxMB })} format={(v) => `${v} MB`} width={70} />
        </SettingRow>
        <SettingRow label={tr('settings.clipboard.expire')} hint={tr('settings.clipboard.expireHint')}>
          <Stepper value={c.expireDays} min={0} max={365} step={1} onChange={(expireDays) => put({ expireDays })}
            format={(v) => (v ? tr('settings.clipboard.days', { count: v }) : tr('settings.clipboard.never'))} width={120} />
        </SettingRow>
      </Section>
      <Section title={tr('settings.clipboard.ignore')} footer={tr('settings.clipboard.ignoreHint')}>
        {c.ignoreApps.map((a) => (
          <SettingRow key={a} label={a} labelStyle={{ fontFamily: 'monospace', fontSize: 12 }}>
            <Button small label="−" onClick={() => put({ ignoreApps: c.ignoreApps.filter((x) => x !== a) })} />
          </SettingRow>
        ))}
        {origins.length > 0 && (
          <View style={{ flexDirection: 'row', flexWrap: 'wrap', gap: 6, paddingVertical: spacing.s }}>
            {origins.map((o) => <Button small key={o} label={`+ ${o}`} onClick={() => put({ ignoreApps: [...c.ignoreApps, o] })} />)}
          </View>
        )}
        <View style={{ flexDirection: 'row', alignItems: 'center', gap: spacing.s, paddingVertical: spacing.s }}>
          <TextInput value={app} onChange={(v) => setApp(v.replace(/[^A-Za-z0-9._-]/g, '').slice(0, 63))} placeholder={tr('settings.clipboard.appId')}
            onSubmit={addApp} style={{ flex: 1 }} />
          <Button small label={tr('settings.clipboard.add')} onClick={addApp} />
        </View>
      </Section>
      <Section footer={tr('settings.clipboard.historyHint')}>
        <SettingRow label={tr('settings.clipboard.history', { count: list.length })}>
          <Button small label={tr('settings.clipboard.open')} onClick={() => openClipboardPanel('full')} />
          <Button small destructive label={tr('settings.clipboard.clearAll')} onClick={() => clipboard.clear({ keepPinned: false })} />
        </SettingRow>
      </Section>
      <Note>{tr('settings.clipboard.privacy')}</Note>
    </Stack>
  );
}
