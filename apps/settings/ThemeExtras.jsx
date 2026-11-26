// What Settings shows for a theme it doesn't know by id (0.8.9): its own choices (theme.json "options") under
// Appearance → Desktop style while it is the current theme, checked by sdk/thememeta.js. The drawn picture of its
// theme.json "sketch" is the SDK's (sdk/stylethumb.jsx DesktopStyleThumb).
import { useLocale, useSettings, Toggle, Segmented, Button, themeOptionValue, themeOptionPatch, themeLabel, displayAppsOn, displayAppsStored, setDisplayAppsOn, forgetDisplayApps,
  useStyleParts, partOn, partPatch } from '@lordos/sdk';
import { Section, SettingRow } from './ui.jsx';

// A theme's own choices, named by itself (its name is the section's title, unless `title` is given: the theme's page in
// the gallery, ThemeGallery.jsx). A community theme's own keys (theme-<id>.…) are its texts: themeLabel shows them on one
// line and cut at a fixed length (sdk/thememeta.js).
export function ThemeOptions({ theme, title }) {
  const { t: tr } = useLocale();
  const [s, set] = useSettings();
  if (!theme || ((!theme.options || !theme.options.length) && !theme.displayApps)) return null;
  return (
    <Section title={title || theme.name}>
      {(theme.options || []).map((o) => {
        const v = themeOptionValue(s, theme.id, o);
        return (
          <SettingRow key={o.key} label={themeLabel(tr, o.label)} hint={o.hint ? themeLabel(tr, o.hint, 'hint') : undefined}>
            {o.toggle
              ? <Toggle small on={v} onChange={(on) => set(themeOptionPatch(s, theme.id, o.key, on))} />
              : <Segmented small appearance="neutral" value={v} onChange={(x) => set(themeOptionPatch(s, theme.id, o.key, x))} options={o.choices.map(([c, l]) => [c, themeLabel(tr, l, 'choice')])} />}
          </SettingRow>
        );
      })}
      {/* (0.8.9.5) the apps used on each display (sdk/displayapps.js): kept on this computer, turned off or cleared here */}
      {theme.displayApps ? (
        <SettingRow label={tr('shell.displayApps.remember')} hint={tr('shell.displayApps.rememberHint')}>
          <Toggle small on={displayAppsOn(s)} onChange={(on) => setDisplayAppsOn(on)} />
        </SettingRow>
      ) : null}
      {theme.displayApps ? (
        <SettingRow label={tr('shell.displayApps.clear')} hint={tr('shell.displayApps.clearHint')}>
          <Button small label={tr('shell.displayApps.clearButton')} disabled={!displayAppsStored(s)} onClick={forgetDisplayApps} />
        </SettingRow>
      ) : null}
    </Section>
  );
}


// (0.8.9.6.2) What the style in use brings beyond its frames (a style pack and app skins, docs/THEMES-GUIDE.md): a switch
// per part, so someone can keep a theme's frames and use LordOS's icons, pointer, sounds, font or app layouts. Only the
// parts this style brings are listed; a style that brings none shows nothing.
export function ThemeParts() {
  const { t: tr } = useLocale();
  const [s, set] = useSettings();
  const parts = useStyleParts();
  if (!parts.length) return null;
  return (
    <Section title={tr('settings.themeParts.title')}>
      {parts.map((p) => (
        <SettingRow key={p} label={tr(`settings.themeParts.${p}`)} hint={p === 'layouts' ? tr('settings.themeParts.layoutsHint') : undefined}>
          <Toggle small on={partOn(s, p)} onChange={(on) => set(partPatch(s, p, on))} />
        </SettingRow>
      ))}
    </Section>
  );
}
