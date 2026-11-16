// What Settings shows for a theme it doesn't know by id (0.8.9): the drawn picture of its theme.json "sketch" in
// Appearance → Desktop style, and its own choices (theme.json "options") under it while it is the current theme. Both
// come from sdk/thememeta.js, which checks theme.json; Lord Flow and Lord Classic declare neither and keep their own
// pictures (StyleThumb in index.jsx) and pages.
import { View, useLocale, useSettings, Toggle, Segmented, themeOptionValue, themeOptionPatch } from '@lordos/sdk';
import { Section, SettingRow } from './ui.jsx';

// The picture: a rail at the start, a row of pills on top, a framed group of two windows; or a menu bar, a dock or a
// taskbar. Drawn with views, like the other pictures here.
export function SketchThumb({ sketch, accent, dark }) {
  const glass = dark ? 'rgba(30,30,36,0.85)' : 'rgba(255,255,255,0.82)';
  const win = dark ? '#2c2c30' : '#ffffff';
  const left = sketch.rail ? 16 : 6;
  const top = (sketch.menubar ? 8 : 4) + (sketch.pills ? 10 : 0);
  const bottom = sketch.taskbar ? 13 : sketch.dock ? 14 : 5;
  return (
    <View style={{ width: 132, height: 82, borderRadius: 7, overflow: 'hidden', direction: 'ltr',
      background: dark ? 'linear-gradient(160deg, #2b2d4a, #121218)' : 'linear-gradient(160deg, #cfe0ff, #eef1f7)' }}>
      {sketch.menubar ? <View style={{ position: 'absolute', left: 0, right: 0, top: 0, height: 6, backgroundColor: glass }} /> : null}
      {sketch.rail ? (
        <View style={{ position: 'absolute', left: 3, top: 3, bottom: 3, width: 10, borderRadius: 3, backgroundColor: glass, alignItems: 'center', paddingTop: 4, gap: 3 }}>
          {[0, 1, 2].map((i) => <View key={i} style={{ width: 5, height: 5, borderRadius: 1.5, backgroundColor: i ? '#8e8e93' : accent }} />)}
        </View>
      ) : null}
      {sketch.pills ? (
        <View style={{ position: 'absolute', left, top: sketch.menubar ? 9 : 4, flexDirection: 'row', gap: 3 }}>
          {[accent, glass, glass].map((c, i) => <View key={i} style={{ width: 16, height: 6, borderRadius: 3, backgroundColor: c }} />)}
        </View>
      ) : null}
      <View style={{ position: 'absolute', left, right: 5, top, bottom, borderRadius: 5, ...(sketch.frame ? { border: `1px solid ${accent}` } : null) }}>
        <View style={{ position: 'absolute', left: 4, top: 4, bottom: 4, width: '58%', borderRadius: 3, backgroundColor: win, boxShadow: '0 1px 3px rgba(0,0,0,0.3)' }}>
          <View style={{ marginLeft: 4, marginTop: 4, height: 3, borderRadius: 2, backgroundColor: accent, width: 22 }} />
        </View>
        <View style={{ position: 'absolute', right: 4, top: 4, bottom: 4, width: '32%', borderRadius: 3, backgroundColor: win, boxShadow: '0 1px 3px rgba(0,0,0,0.3)' }} />
      </View>
      {sketch.dock ? <View style={{ position: 'absolute', left: 40, right: 40, bottom: 3, height: 8, borderRadius: 3, backgroundColor: glass }} /> : null}
      {sketch.taskbar ? <View style={{ position: 'absolute', left: 0, right: 0, bottom: 0, height: 10, backgroundColor: glass }} /> : null}
    </View>
  );
}

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

// The bundled themes Settings draws and lays out by their id.
export const knownTheme = (id) => id === 'default' || id === 'classic';
