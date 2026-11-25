// origincol.js — an origin's second column (LordOS apps, GNU/Linux apps (Debian), Flathub apps, Web apps (PWA)):
// "Installed" at the top, "Settings" under it (the system's own settings, 2026-11-23), then its
// categories (All, the shop's home, and each category with apps), the way the Store's own sidebar puts Installed
// among its pages (2026-11-23). In a narrow window the same choices are chips over the page. The values are the
// Store's `sub`: null (All), a category, or MINE (installedmodel.js). Scene test: server/tests/store_origin_scene.mjs.
import { createElement as h } from 'react';
import { View, Text, ScrollView, SidebarItem, SectionTitle, Hairline, useTheme, t } from '@lordos/sdk';
import { GLYPH } from './ui.js';
import { MINE, SETTINGS } from './installedmodel.js';

// cats: the categories in order, counts: how many apps each (only those with apps are shown), total: All's count,
// mine: how many apps of this system are installed (shown when > 0). With no category that has apps (Apps LordOS
// while its list is short, lordosmodel.js) there is no "Categories" heading: Installed, then All (the shop).
export function OriginColumn({ sub, cats, counts, total, mine, onPick }) {
  const th = useTheme();
  return h(View, { style: { width: 190, backgroundColor: th.sidebarBg } },
    h(Hairline, { edge: 'end' }),
    h(ScrollView, { style: { flex: 1, paddingHorizontal: 10, paddingTop: 4 } },
      h(View, { style: { height: 10 } }),
      h(SidebarItem, { icon: GLYPH.installed, label: t('store.page.installed'), detail: mine || null, selected: sub === MINE, onClick: () => onPick(MINE) }),
      h(SidebarItem, { icon: GLYPH.settings, label: t('store.sub.settings'), selected: sub === SETTINGS, onClick: () => onPick(SETTINGS) }),
      cats.some((c) => counts[c]) ? h(SectionTitle, null, t('store.section.categories')) : h(View, { style: { height: 4 } }),
      h(SidebarItem, { label: t('store.sub.all'), detail: total || null, selected: !sub, onClick: () => onPick(null) }),
      ...cats.filter((c) => counts[c]).map((c) => h(SidebarItem, {
        key: c, icon: GLYPH[c], label: t(`store.cat.${c}`), detail: counts[c], selected: sub === c, onClick: () => onPick(c),
      })),
      h(View, { style: { height: 12 } })));
}

// The narrow window's chips: Installed and Settings first, then All and the categories.
export function OriginChips({ sub, cats, counts, onPick }) {
  const th = useTheme();
  const chip = (c, label) => h(View, {
    key: c || 'all', onClick: () => onPick(c),
    style: { height: 26, paddingHorizontal: 12, borderRadius: 13, justifyContent: 'center', backgroundColor: sub === c ? th.accent : th.hover },
  }, h(Text, { style: { fontSize: 12, fontWeight: 600, color: sub === c ? '#fff' : th.text } }, label));
  return h(View, { style: { flexDirection: 'row', flexWrap: 'wrap', gap: 6, marginBottom: 14 } },
    chip(MINE, t('store.page.installed')),
    chip(SETTINGS, t('store.sub.settings')),
    chip(null, t('store.sub.all')),
    ...cats.filter((c) => counts[c]).map((c) => chip(c, t(`store.cat.${c}`))));
}

// The general Installed page's filters as chips that wrap (All, each system with apps, Deactivated), each with its
// count; `filters` is installedmodel.js's installedFilters().
export function FilterChips({ value, filters, onChange }) {
  const th = useTheme();
  return h(View, { style: { flexDirection: 'row', flexWrap: 'wrap', gap: 6, marginBottom: 14 } },
    ...filters.map(([k, n]) => {
      const on = value === k;
      return h(View, {
        key: k, onClick: () => onChange(k),
        style: { height: 28, paddingHorizontal: 12, borderRadius: 14, flexDirection: 'row', alignItems: 'center', gap: 6, backgroundColor: on ? th.accent : th.hover },
      },
      h(Text, { style: { fontSize: 12, fontWeight: 600, color: on ? '#fff' : th.text, whiteSpace: 'nowrap' } }, t(FILTER_LABEL[k] || 'store.off.filterAll')),
      h(Text, { style: { fontSize: 11, color: on ? 'rgba(255,255,255,0.8)' : th.textDim } }, String(n)));
    }));
}
const FILTER_LABEL = {
  all: 'store.off.filterAll', lordos: 'store.page.lordos', debian: 'store.src.debian', flathub: 'store.page.flathub', web: 'store.page.webapps',
  off: 'store.off.filterOff',
};
