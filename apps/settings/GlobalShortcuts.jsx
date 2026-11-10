// Settings → Keyboard: the global shortcuts Linux apps were given (org.freedesktop.portal.GlobalShortcuts,
// sdk/globalshortcuts.jsx). Each one can get another key or none, and an app can be forgotten: its open
// sessions lose their keys now and its next session asks again.
import { useTheme, Button, KeyComboField, useGlobalShortcuts, checkGlobalShortcut, setGlobalShortcut, forgetGlobalShortcuts } from '@lordos/sdk';
import { Section, SettingRow, Actions } from './ui.jsx';

export function GlobalShortcutsSettings({ tr }) {
  const t = useTheme();
  const apps = useGlobalShortcuts();
  if (!apps.length) {
    return (
      <Section title={tr('settings.kb.gshort.title')} footer={tr('settings.kb.gshort.hint')}>
        <SettingRow label={tr('settings.kb.gshort.empty')} />
      </Section>
    );
  }
  return apps.map((g, n) => (
    <Section key={g.app} title={tr('settings.kb.gshort.app', { app: g.name })} footer={n === apps.length - 1 ? tr('settings.kb.gshort.hint') : undefined}>
      {g.items.map((it) => (
        <SettingRow key={it.id} label={it.description || it.id}>
          <KeyComboField th={t} value={it.trigger} check={checkGlobalShortcut(g.app, it.id)} onChange={(c) => setGlobalShortcut(g.app, it.id, c)} />
        </SettingRow>
      ))}
      <Actions>
        <Button small label={tr('settings.kb.gshort.forget')} onClick={() => forgetGlobalShortcuts(g.app)} />
      </Actions>
    </Section>
  ));
}
