// Settings → Desktop & Dock: the desktops (sdk/desktops.js, docs/DESKTOPS.md), the same in every theme.
import { View, Text, Button, useSettings, Toggle, useStore, desktopList, setDesktopCount, themes, typeScale, useTheme, trackpad } from '@lordos/sdk';
import { Section, SettingRow } from './ui.jsx';

const MAX = 9;

export function DesktopsSection({ tr }) {
  const t = useTheme();
  const [s, set] = useSettings();
  const n = useStore(desktopList).length;
  const pad = { ...(s.trackpad || {}) };
  return (
    <Section title={tr('settings.dock.desktops.title')} footer={tr('settings.dock.desktops.hint')}>
      <SettingRow label={tr('settings.dock.desktops.count')}>
        <View style={{ flexDirection: 'row', alignItems: 'center', gap: 8 }}>
          <Button small label="−" onClick={() => setDesktopCount(n - 1)} disabled={n <= 1} />
          <Text style={{ minWidth: 20, textAlign: 'center', fontSize: typeScale.body, color: t.text }}>{String(n)}</Text>
          <Button small label="+" onClick={() => setDesktopCount(n + 1)} disabled={n >= MAX} />
        </View>
      </SettingRow>
      {themes.current() === 'default' && (
        <SettingRow label={tr('settings.dock.desktops.arrows')} hint={tr('settings.dock.desktops.arrowsHint')}>
          <Toggle small on={s.desktopArrows !== false} onChange={(v) => set({ desktopArrows: v })} />
        </SettingRow>
      )}
      <SettingRow label={tr('settings.dock.desktops.swipe')}>
        <Toggle small on={pad.swipe4 !== false} onChange={(v) => trackpad.set({ swipe4: v })} />
      </SettingRow>
    </Section>
  );
}
