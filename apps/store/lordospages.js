// lordospages.js — Store → Apps LordOS, LordOS's own store (2026-11-23): the shop's cards of LordOS's installable apps
// and components (assets/store/lordos-apps.json, read by lordosmodel.js), an item's page and the rows the Installed
// pages show. Nothing installs differently from before: a package of LordOS's repository opens the usual package page
// (debian.Details: its plan, then pkg-install through the package engine), Disk Formats its catalog page with the
// drivers' licenses first (ai.js AiDetail), and the voices are chosen in Settings → AI, where the page sends the person.
// The .lpk part of the section (installed packages, files in the home folder) stays in index.jsx.
import { createElement as h, useState } from 'react';
import { View, Text, Button, useTheme, useStore, launchApp, wm, scanVoices, typeScale, spacing, t } from '@lordos/sdk';
import DATA from '../../assets/store/lordos-apps.json';
import { nativeItems, nameKey, summaryKey, isInstalled, actionOf, searchNative, inCategory, NATIVE_KIND, rowId } from './lordosmodel.js';
import { debian } from './debian.js';
import { AiDetail, NotListed } from './ai.js';
import { Glyph } from './suggested.js';
import { Card, CardGrid, Pill, Shelf } from './ui.js';

export const NATIVE = nativeItems(DATA);
export const nativeItem = (id) => NATIVE.find((a) => a.id === id) || null;

const pkgInstalled = (p) => { try { return !!(lord.pkg.info(p, {}) || {}).installed; } catch (e) { return false; } };
const pkgOffered = (p) => { try { return !!lord.pkg.info(p, {}); } catch (e) { return false; } };
const voicesInstalled = () => {
  try { const v = scanVoices(); return Object.entries(v).some(([k, r]) => k !== 'own' && r && (r.standard || r.hd)); } catch (e) { return false; }
};
export const PROBE = { pkg: pkgInstalled, voices: voicesInstalled };

// The items and which are installed, again after every install or removal (debian.js's state).
export function useNative() {
  useStore(debian.state, (s) => s.gen);
  const installed = new Set(NATIVE.filter((a) => isInstalled(a, PROBE)).map((a) => a.id));
  return { items: NATIVE, installed };
}

export const NativeIcon = ({ a, size = 56 }) => h(Glyph, { kind: a.icon, size });

// Open: an app (with its arguments: Settings on a section), or one of the Store's pages (go).
export function openNative(a, go) {
  const o = a && a.open;
  if (!o) return;
  if (o.page) go && go(o.page);
  else if (o.args) wm.open(o.app, { args: { ...o.args, nonce: Date.now() } });
  else launchApp(o.app);
}

function NativeCard({ a, inst, onOpen, go }) {
  const act = actionOf(a, inst);
  return h(Card, {
    icon: h(NativeIcon, { a }), name: t(nameKey(a)), tag: t('store.kind.native'), summary: t(summaryKey(a)),
    note: inst ? t('store.installedBadge') : '',
    action: h(Pill, { label: act === 'open' ? t('store.open') : act === 'view' ? t('store.ai.view') : t('store.get'),
      onClick: () => (act === 'open' ? openNative(a, go) : onOpen(a.id)) }),
    onClick: () => onOpen(a.id),
  });
}

// The shop's cards (the search narrows them; cat: one category of the second column, once it has them).
export function NativeShop({ query, cat = null, onOpen, go }) {
  const th = useTheme();
  const { items, installed } = useNative();
  const shown = searchNative(inCategory(items, cat), query, t);
  // straight on the cards: what the section is lives in its Settings (sectionsettings.js LordOSSettings)
  return h(View, { style: { gap: spacing.l } },
    h(debian.Progress),
    shown.length ? h(CardGrid, null, ...shown.map((a) => h(NativeCard, { key: a.id, a, inst: installed.has(a.id), onOpen, go })))
      : h(Text, { style: { fontSize: typeScale.body, color: th.textDim, textAlign: 'start' } }, t('store.debian.none')));
}

// Discover's search (every system): LordOS's own items that match, as their own shelf.
export function NativeResults({ query, onOpen, go }) {
  const { items, installed } = useNative();
  const hits = searchNative(items, query, t);
  if (!String(query || '').trim() || !hits.length) return null;
  return h(Shelf, { title: t('store.page.lordos') }, h(CardGrid, null, ...hits.map((a) => h(NativeCard, { key: a.id, a, inst: installed.has(a.id), onOpen, go }))));
}
export const nativeHits = (query) => (String(query || '').trim() ? searchNative(NATIVE, query, t).length : 0);

// The installed items as launcher-like entries (the Installed pages' rows: kind "native", installedmodel.js counts
// them as LordOS's).
export function nativeRows(items, installed) {
  return items.filter((a) => installed.has(a.id)).map((a) => ({
    id: rowId(a), kind: NATIVE_KIND, name: t(nameKey(a)), comment: t(summaryKey(a)), native: a,
    Icon: ({ size }) => h(NativeIcon, { a, size: size || 32 }),
  }));
}

// A settings item's page (the voices): what it is, and Settings, where it is installed and removed.
function SettingsItem({ a, inst }) {
  const th = useTheme();
  const name = t(nameKey(a));
  return h(View, { style: { gap: spacing.xl, maxWidth: 680 } },
    h(View, { style: { flexDirection: 'row', gap: spacing.l, alignItems: 'flex-start' } },
      h(View, { style: { borderRadius: 22, overflow: 'hidden', boxShadow: '0 6px 14px rgba(0,0,0,0.22)' } }, h(NativeIcon, { a, size: 96 })),
      h(View, { style: { flex: 1, minWidth: 0, gap: spacing.xs } },
        h(Text, { style: { fontSize: typeScale.largeTitle, fontWeight: 600, color: th.text, textAlign: 'start' } }, name),
        h(Text, { style: { fontSize: typeScale.body, color: th.textDim, lineHeight: 1.35, textAlign: 'start' } }, t(summaryKey(a))),
        h(View, { style: { flexDirection: 'row', flexWrap: 'wrap', alignItems: 'center', gap: spacing.s, marginTop: spacing.s } },
          h(Button, { primary: true, label: t('store.lordos.openSettings'), onClick: () => openNative(a) }),
          inst ? h(Text, { style: { fontSize: typeScale.caption, fontWeight: 500, color: '#30d158', marginLeft: spacing.xs } }, t('store.installedBadge')) : null))),
    h(Text, { style: { fontSize: typeScale.body, color: th.text, lineHeight: 1.45, textAlign: 'start' } }, t('store.lordos.inSettings', { name })));
}

// An item's page: the usual one for its kind, with the item's own icon, name and summary.
export function NativeDetail({ id, go }) {
  const a = nativeItem(id);
  const [, setTick] = useState(0);
  const { installed } = useNative();
  if (!a) return h(Text, null, t('store.debian.notFound', { name: id }));
  const inst = installed.has(a.id);
  if (a.kind === 'settings') return h(SettingsItem, { a, inst });
  const look = { icon: h(NativeIcon, { a, size: 96 }), title: t(nameKey(a)), summary: t(summaryKey(a)), open: a.open ? () => openNative(a, go) : null };
  if (a.kind === 'catalog') return h(AiDetail, { pkg: a.package, look });
  // a package of LordOS's repository the lists don't have yet (fetched before it was published): fetch them again
  if (!pkgOffered(a.package)) return h(NotListed, { a: { name: look.title }, onDone: () => setTick((x) => x + 1) });
  return h(debian.Details, { name: a.package, look });
}
