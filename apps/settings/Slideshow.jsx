// Settings → Wallpaper → the slideshow (0.8.9): shown only for a theme that declares "wallpaper": { "slideshow": true }
// in its theme.json (sdk/thememeta.js; Lord Workspaces). The rules are in sdk/slideshowmodel.js; the theme draws it. Its
// place is the bottom of Settings → Wallpaper (./WallpaperGallery.jsx), whose collection pages also offer Use as slideshow.
import { useLocale, useSettings, useTheme, Toggle, slideshowPrefs, slideshowPatch, slideshowCategories, withDownloadedStills, SLIDESHOW_EVERY, WALLPAPER_CATEGORIES, tOptional } from '@lordos/sdk';
import { Section, SettingRow, PopUp } from './ui.jsx';

export function SlideshowSettings({ displays }) {
  const t = useTheme();
  const { t: tr } = useLocale();
  const [s, set] = useSettings();
  const p = slideshowPrefs(s);
  // the theme's categories, and the catalog's still collections with two pictures or more on this computer (0.8.9.6.2)
  const cats = slideshowCategories(withDownloadedStills(t.wallpapers), WALLPAPER_CATEGORIES);
  if (!cats.length) return null;
  const category = cats.includes(p.category) ? p.category : cats[0];
  const catName = (c) => tOptional(`settings.wallpaperCategory.${c}`) || tr('settings.wallpaperCategory.other');
  const change = (x) => set(slideshowPatch(s, x));
  return (
    <Section footer={tr('settings.slideshow.hint')}>
      <SettingRow label={tr('settings.slideshow.title')}>
        <Toggle small on={p.on} onChange={(on) => change({ on, category })} />
      </SettingRow>
      {p.on ? (
        <>
          <SettingRow label={tr('settings.slideshow.category')}>
            <PopUp value={category} onChange={(c) => change({ category: c })} options={cats.map((c) => [c, catName(c)])} />
          </SettingRow>
          <SettingRow label={tr('settings.slideshow.every')}>
            <PopUp value={p.every} onChange={(v) => change({ every: Number(v) })} options={SLIDESHOW_EVERY.map((v) => [v, tr(`settings.slideshow.every${v}`)])} />
          </SettingRow>
          {displays.length > 1 ? (
            <SettingRow label={tr('settings.slideshow.perDisplay')} hint={tr('settings.slideshow.perDisplayHint')}>
              <Toggle small on={p.perDisplay} onChange={(perDisplay) => change({ perDisplay })} />
            </SettingRow>
          ) : null}
        </>
      ) : null}
    </Section>
  );
}
