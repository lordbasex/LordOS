// What Settings shows for a theme it doesn't know by id (0.8.9): its own choices (theme.json "options") under
// Appearance → Desktop style while it is the current theme, checked by sdk/thememeta.js. The drawn picture of its
// theme.json "sketch" is the SDK's (sdk/stylethumb.jsx DesktopStyleThumb).
import { useLocale, useSettings, Toggle, Segmented, themeOptionValue, themeOptionPatch } from '@lordos/sdk';
import { Section, SettingRow } from './ui.jsx';

// The current theme's own choices, named by itself (its name is the section's title).
export function ThemeOptions({ theme }) {
  const { t: tr } = useLocale();
  const [s, set] = useSettings();
  if (!theme || !theme.options || !theme.options.length) return null;
  return (
    <Section title={theme.name}>
      {theme.options.map((o) => {
        const v = themeOptionValue(s, theme.id, o);
        return (
          <SettingRow key={o.key} label={tr(o.label)} hint={o.hint ? tr(o.hint) : undefined}>
            {o.toggle
              ? <Toggle small on={v} onChange={(on) => set(themeOptionPatch(s, theme.id, o.key, on))} />
              : <Segmented small appearance="neutral" value={v} onChange={(x) => set(themeOptionPatch(s, theme.id, o.key, x))} options={o.choices.map(([c, l]) => [c, tr(l)])} />}
          </SettingRow>
        );
      })}
    </Section>
  );
}

