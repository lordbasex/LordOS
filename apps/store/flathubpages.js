// flathubpages.js — the Store's Flathub section (docs/FLATPAK.md, phase 1 step 5): turning Flathub on (the `flatpak`
// package through the Store's package job, then the remote and the catalog), a home like other app centres' (a banner,
// the categories, the collections; docs/FLATPAK.md, "The Store's home"; with Flathub's rankings when they are on, E10,
// "Rankings from Flathub's API": ids only, shown with the catalog's names and icons) over the catalog as cards (Flathub's verified
// apps unless the setting shows the rest), a category's subcategories, each app's page with what it declares and what LordOS changes before it is
// installed, and (the second column's Installed, FlathubInstalled) the installed ones with Update, Remove and Open. Chromium and Electron apps install like any other: their
// helpers' sub-sandboxes are served by LordOS (docs/FLATPAK.md, "Sub-sandboxes"). The jobs and state are
// sdk/flathub.js's; the logic sdk/flathubmodel.js's.
import { createElement as h, memo, useCallback, useEffect, useMemo, useRef, useState } from 'react';
import {
  View, Text, Image, Button, GroupBox, Toggle, LetterIcon, useTheme, useStore, useLauncherApps, launchApp, admin, formatBytes, errorText, settings,
  flathubModel as M, typeScale, spacing, t,
} from '@lordos/sdk';
import { debian } from './debian.js';
import { ConfirmRemove } from './confirmremove.js';
import { Card, CardGrid, Facts, Featured, GLYPH, Pill, Shelf, Tag, nameColor, tint } from './ui.js';
import { MoreButton, usePaged } from './endless.js';
import { DisplayToolkit } from './fhlook.js';
import { SettingsPage, About as AboutText, Group, Row } from './sectionsettings.js';
import { flathub as F } from '@lordos/sdk';

const SHELF = 8;            // cards in a Discover or category shelf

const box = (th, extra) => ({ padding: spacing.xl, borderRadius: 12, backgroundColor: th.mode === 'dark' ? 'rgba(255,255,255,0.04)' : 'rgba(0,0,0,0.025)',
  border: `1px solid ${th.separator}`, gap: spacing.s, marginBottom: spacing.xl, ...extra });
const Line = ({ th, color, bold, children }) => h(Text, { style: { fontSize: typeScale.callout, color: color || th.textDim, lineHeight: 1.45, textAlign: 'start', fontWeight: bold ? 600 : 400 } }, children);
const Title = ({ th, children }) => h(Text, { style: { fontSize: typeScale.headline, fontWeight: 600, color: th.text, textAlign: 'start' } }, children);
const Header = ({ th, children }) => h(Text, { style: { fontSize: typeScale.body, fontWeight: 600, color: th.text, paddingHorizontal: spacing.xs, marginBottom: 6, textAlign: 'start' } }, children);

// The section is shown when this PC can have Flathub and the user turned it on.
export const useFlathub = () => {
  // redrawn when a job starts or ends, or the catalog or the installed apps change (whether Flatpak is there may
  // change then), never on a job's progress lines
  useStore(F.state, (x) => `${x.loaded}|${x.installed.length}|${x.job ? 1 : 0}|${x.busy}|${x.catalog.length}`);
  const p = M.prefsOf(useStore(settings, (st) => st.flathub));
  return { supported: F.supported(), on: p.on && F.available(), prefs: p };
};

// The launcher's entry of an installed Flatpak app (its exported icon and name), or null.
const useExported = (id) => useLauncherApps().find((a) => a.kind === 'flatpak' && a.flatpakId === id) || null;

// An app's icon: the catalog's cached PNG (decoded off the interface's thread), the exported one, or its initial.
export function FlathubIcon({ a, size }) {
  const exported = useExported(a.id);
  if (a.icon) return h(Image, { src: `thumb:${Math.min(256, size * 2)}:${a.icon}`, style: { width: size, height: size, objectFit: 'contain' } });
  if (exported) return h(exported.Icon, { size });
  return h(LetterIcon, { size, name: a.name || a.id });
}

const catLabel = (a) => {
  const main = String(a.categories || '').split(';').find((c) => ['AudioVideo', 'Development', 'Education', 'Game', 'Graphics', 'Network', 'Office', 'Science', 'System', 'Utility'].includes(c));
  return main ? t(`store.cat.${main}`) : '';
};
const openApp = (id) => launchApp(`flatpak:${id}`);

// memo: a list that grows by a page doesn't draw the cards it already showed again (onOpen must be stable)
export const FlathubCard = memo(function FlathubCard({ a, onOpen }) {
  // only whether this app is installed: a job's progress doesn't redraw every card
  const installed = useStore(F.state, (s) => s.installed.some((x) => x.id === a.id));
  return h(Card, {
    icon: h(FlathubIcon, { a, size: 56 }), name: a.name, tag: 'Flathub', kind: [catLabel(a), a.developer].filter(Boolean).join(' · '), summary: a.summary,
    note: a.verified ? `✓ ${t('flathub.verifiedShort')}` : t('flathub.unverifiedShort'),
    action: installed ? h(Pill, { label: t('store.open'), onClick: () => openApp(a.id) }) : h(Pill, { label: t('store.get'), onClick: () => onOpen(a.id) }),
    onClick: () => onOpen(a.id),
  });
});

// The running job: what, its progress and Flatpak's last line; then its outcome (only the outcome for app forId when
// given: '' for "Update Flathub apps").
export function JobProgress({ forId }) {
  const th = useTheme();
  const s = useStore(F.state);
  if (s.job) {
    const j = s.job;
    return h(View, { style: box(th, { gap: 6 }) },
      h(Text, { style: { fontSize: 13, fontWeight: 600, color: th.text, textAlign: 'start' } }, t(`flathub.job.${j.kind}`, { name: j.name })),
      h(View, { style: { height: 6, borderRadius: 3, backgroundColor: th.control, direction: 'ltr' } },
        h(View, { style: { width: `${j.pct == null ? 4 : Math.max(4, j.pct)}%`, height: 6, borderRadius: 3, backgroundColor: th.accent } })),
      j.line ? h(Text, { style: { fontSize: 11, color: th.textDim, whiteSpace: 'nowrap', overflow: 'hidden', textAlign: 'start' } }, j.line) : null,
      h(View, { style: { flexDirection: 'row', justifyContent: 'flex-end' } }, h(Button, { small: true, label: t('store.cancel'), onClick: F.cancelJob })));
  }
  if (s.busy) return h(Line, { th }, t(`flathub.busy.${s.busy}`));
  if (s.msg && (forId === undefined || s.msg.id === forId)) return h(View, { style: { marginBottom: spacing.m } }, h(Line, { th, color: s.msg.ok ? '#30d158' : '#ff453a' }, s.msg.text));
  return null;
}

// ---- turning Flathub on ----

// The package plan for flatpak (what it adds and its size), or { ok: false }.
function flatpakPlan() {
  try { return typeof lord !== 'undefined' && lord.pkg ? lord.pkg.plan({ install: ['flatpak'] }) : { ok: false }; } catch (e) { return { ok: false, error: e.code || null }; }
}

function TurnOn() {
  const th = useTheme();
  const s = useStore(F.state);
  const deb = useStore(debian.state);
  const [plan, setPlan] = useState(null);
  const [step, setStep] = useState('idle');       // idle → plan → package → flathub
  const installedPkg = F.available();
  const isAdmin = admin.available();
  const start = () => {
    if (installedPkg) { setStep('flathub'); F.turnOn().then(() => setStep('idle')); return; }
    setPlan(flatpakPlan());
    setStep('plan');
  };
  const confirm = () => {
    if (!plan || !plan.ok || !isAdmin || deb.job) return;
    const extra = (plan.items || []).some((i) => i.action === 'remove' && !i.requested);
    setStep('package');
    debian.runJob('pkg-install', extra ? 'flatpak --allow-remove' : 'flatpak', t('flathub.setup.packageJob')).then((ok) => {
      if (!ok || !F.available()) { setStep('idle'); return; }
      setStep('flathub');
      F.turnOn().then(() => setStep('idle'));
    });
  };
  const sizes = plan && plan.ok ? { download: plan.download || 0, disk: plan.installedDelta || 0, count: (plan.items || []).filter((i) => i.action === 'install').length } : null;
  return h(View, { style: box(th) },
    h(Title, { th }, t('flathub.setup.title')),
    h(Line, { th, color: th.text }, t('flathub.setup.what')),
    h(Line, { th }, t('flathub.setup.rules')),
    h(Line, { th }, t('flathub.setup.verified')),
    !installedPkg ? h(Line, { th }, t('flathub.setup.package')) : null,
    step === 'plan' && sizes ? h(Line, { th, color: th.text, bold: true }, t('flathub.setup.sizes', { download: formatBytes(sizes.download), disk: formatBytes(sizes.disk), count: sizes.count })) : null,
    step === 'plan' && plan && !plan.ok ? h(Line, { th, color: '#ff453a' }, (plan.error && errorText(plan.error)) || t('store.plan.impossible')) : null,
    step === 'plan' && sizes && !isAdmin ? h(Line, { th, color: '#ff9f0a' }, t('store.debian.needAdmin')) : null,
    step === 'package' ? h(debian.Progress) : null,
    step === 'idle' && deb.msg && !deb.msg.ok ? h(debian.Progress) : null,
    step === 'flathub' || s.busy ? h(JobProgress) : null,
    step === 'idle' && s.msg && !s.msg.ok ? h(Line, { th, color: '#ff453a' }, s.msg.text) : null,
    h(View, { style: { flexDirection: 'row', gap: spacing.s, justifyContent: 'flex-end' } },
      step === 'plan' ? h(Button, { label: t('store.cancel'), onClick: () => setStep('idle') }) : null,
      step === 'idle' ? h(Button, { primary: true, label: t('flathub.setup.turnOn'), pending: true, onClick: start }) : null,
      step === 'plan' && sizes ? h(Button, { primary: true, label: t('flathub.setup.install'), disabled: !isAdmin || !!deb.job, onClick: confirm }) : null));
}

// ---- the section's page ----

// The installed apps with Remove and Open (the section's Installed), those whose name or id has the search's words.
function InstalledList({ onOpen, query = '' }) {
  const th = useTheme();
  const s = useStore(F.state);
  const [asking, setAsking] = useState(null);           // Remove asks first (ConfirmRemove)
  const q = String(query || '').trim().toLowerCase();
  const list = s.installed.map((x) => ({ x, a: F.catalogApp(x.id) || { id: x.id, name: x.id, summary: '', icon: '' } }))
    .filter(({ x, a }) => !q || q.split(/\s+/).every((w) => `${a.name} ${x.id}`.toLowerCase().includes(w)));
  if (!s.installed.length) return h(Line, { th }, t('store.installed.noneFlathub'));
  if (!list.length) return h(Line, { th }, t('store.debian.none'));
  const busy = !!s.job;
  return h(View, { style: { marginBottom: spacing.xl } },
    h(Header, { th }, t('flathub.installedTitle', { count: s.installed.length })),
    h(GroupBox, null, ...list.map(({ x, a }) => {
      if (asking === x.id) return h(ConfirmRemove, { key: x.id, name: a.name, onNo: () => setAsking(null), onYes: () => { setAsking(null); if (!busy) F.uninstall({ ...x, name: a.name }); } });
      return h(View, { key: x.id, style: { flexDirection: 'row', alignItems: 'center', gap: spacing.m, paddingVertical: spacing.s + 2 } },
        h(FlathubIcon, { a, size: 32 }),
        h(View, { style: { flex: 1, minWidth: 0, gap: 2 } },
          h(Text, { onClick: () => onOpen(x.id), style: { fontSize: typeScale.body, fontWeight: 600, color: th.text, textAlign: 'start', whiteSpace: 'nowrap', overflow: 'hidden' } }, a.name),
          h(Text, { style: { fontSize: typeScale.caption, color: th.textDim, textAlign: 'start', whiteSpace: 'nowrap', overflow: 'hidden' } },
            x.version ? t('store.version', { version: x.version }) : x.id)),
        h(Button, { small: true, label: t('flathub.remove'), busy, pending: true, onClick: () => !busy && setAsking(x.id) }),
        h(Button, { small: true, label: t('store.open'), onClick: () => openApp(x.id) }));
    })));
}

// ---- the home's pieces (Bazaar-like: a banner, the categories, the collections, then every app) ----

// The banner (M.banner): Flathub's app of the day when the rankings have it, else the app released most recently, each
// said as such. Name, icon and summary from the catalog either way.
function Spotlight({ b, onOpen }) {
  if (!b) return null;
  const a = b.app;
  return h(Featured, {
    icon: h(FlathubIcon, { a, size: 96 }), glow: nameColor(a.name), kicker: t(b.kind === 'day' ? 'flathub.home.appOfTheDay' : 'flathub.home.spotlight'),
    title: a.name, text: a.summary,
    action: h(Pill, { big: true, onDark: true, label: t('store.get'), onClick: () => onOpen(a.id) }), onClick: () => onOpen(a.id),
  });
}

// The main categories as tiles (glyph, name, how many apps), each opening its page.
function CategoryTiles({ counts, onCategory }) {
  const th = useTheme();
  const cats = M.MAIN_CATS.filter((c) => counts[c]);
  if (!cats.length) return null;
  return h(Shelf, { title: t('flathub.home.categories') },
    h(View, { style: { flexDirection: 'row', flexWrap: 'wrap', gap: spacing.s } },
      ...cats.map((c) => h(Tile, { key: c, th, c, n: counts[c], onClick: () => onCategory(c) })),
      // empty, zero-height cells with the tiles' own flex: the last row's tiles keep the first row's width instead of
      // stretching over the rest of it (five: a row holds at most six tiles at the Store's widest)
      ...[0, 1, 2, 3, 4].map((i) => h(View, { key: `pad-${i}`, style: { ...TILE_FLEX, height: 0 } }))));
}
const TILE_FLEX = { flexGrow: 1, flexBasis: 160, minWidth: 140 };
function Tile({ th, c, n, onClick }) {
  const [hover, setHover] = useState(false);
  return h(View, {
    onClick, onMouseEnter: () => setHover(true), onMouseLeave: () => setHover(false),
    style: { ...TILE_FLEX, height: 52, paddingHorizontal: spacing.m, borderRadius: 10, flexDirection: 'row', alignItems: 'center', gap: spacing.s,
      backgroundColor: hover ? tint(th.accent, 0.16) : tint(th.accent, 0.08), border: `1px solid ${tint(th.accent, 0.22)}` },
  },
  h(Text, { style: { fontSize: 18, color: th.accent, width: 22, textAlign: 'center' } }, GLYPH[c] || '•'),
  h(View, { style: { flex: 1, minWidth: 0 } },
    h(Text, { style: { fontSize: typeScale.callout, fontWeight: 650, color: th.text, whiteSpace: 'nowrap', overflow: 'hidden', textAlign: 'start' } }, t(`store.cat.${c}`)),
    h(Text, { style: { fontSize: typeScale.caption, color: th.textDim, textAlign: 'start' } }, String(n))));
}

// A category's subcategories as chips: All, then each one that has apps (with how many).
function SubChips({ sub, counts, total, onSub }) {
  const th = useTheme();
  const subs = Object.keys(counts);
  if (!subs.length) return null;
  const chip = (s, label, n) => h(View, { key: s || 'all', onClick: () => onSub(s),
    style: { height: 28, paddingHorizontal: 12, borderRadius: 14, flexDirection: 'row', alignItems: 'center', gap: 6, backgroundColor: sub === s ? th.accent : th.hover } },
  h(Text, { style: { fontSize: 12, fontWeight: 600, color: sub === s ? '#fff' : th.text, whiteSpace: 'nowrap' } }, label),
  h(Text, { style: { fontSize: 11, color: sub === s ? 'rgba(255,255,255,0.8)' : th.textDim } }, String(n)));
  return h(View, { style: { flexDirection: 'row', flexWrap: 'wrap', gap: 6, marginBottom: spacing.l } },
    chip(null, t('store.sub.all'), total), ...subs.map((s) => chip(s, t(`flathub.sub.${s}`), counts[s])));
}

// The collections as shelves of SHELF cards, each with "See all" to its own page.
function CollectionRows({ rows, onOpen, onCollection }) {
  return h(View, { style: { gap: 26 } }, ...rows.map((r) => h(Shelf, {
    key: r.key, title: t(`flathub.coll.${r.key}`), more: t('store.seeAll', { count: r.apps.length }), onMore: r.apps.length > SHELF ? () => onCollection(r.key) : null,
  }, h(CardGrid, null, ...r.apps.slice(0, SHELF).map((a) => h(FlathubCard, { key: a.id, a, onOpen }))))));
}

// The section's page. With no search, category or collection, the home: the banner, the categories, the collections,
// then every app; a category shows its subcategories over its apps; a collection ("See all") its apps in its order.
export function FlathubPage({ query, onOpen, category = null, sub = null, coll = null, onCategory = () => {}, onSub = () => {}, onCollection = () => {} }) {
  const th = useTheme();
  // what the page itself shows; a job's progress is JobProgress's own (it changes several times a second)
  const catalog = useStore(F.state, (x) => x.catalog);
  const loaded = useStore(F.state, (x) => x.loaded);
  const working = useStore(F.state, (x) => !!x.busy || !!x.job);
  const s = { catalog, loaded };
  const fh = useFlathub();
  const showAll = fh.prefs.showAll;
  // Flathub's rankings (E10): ids only, empty when off, offline or failed
  const ranks = useStore(F.state, (x) => x.ranks);
  useEffect(() => { if (fh.on) { if (!s.loaded) F.loadCatalog(); F.refreshInstalled(); } }, [fh.on]);
  const q = String(query || '').trim();
  const mode = q ? 'search' : coll && M.collectionOk(coll) ? 'coll' : category ? 'cat' : 'home';
  useEffect(() => { if (fh.on && fh.prefs.rankings && (mode === 'home' || mode === 'coll')) F.loadRanks(); }, [fh.on, fh.prefs.rankings, mode]);
  const subKey = mode === 'cat' && M.subOk(category, sub) ? sub : null;
  // "now" for the release dates: once per catalog read
  const now = useMemo(() => Date.now(), [s.catalog]);
  const list = useMemo(() => (mode === 'coll' ? M.collection(s.catalog, coll, { showAll, now, ranks })
    : M.filterCatalog(s.catalog, { query, showAll, category: mode === 'home' ? null : category, sub: subKey })),
  [s.catalog, query, showAll, category, subKey, mode, coll, now, ranks]);
  const rows = useMemo(() => (mode === 'home' ? M.homeRows(s.catalog, { showAll, now, ranks }) : []), [s.catalog, showAll, now, mode, ranks]);
  const spot = useMemo(() => (mode === 'home' ? M.banner(s.catalog, { showAll, now, ranks }) : null), [s.catalog, showAll, now, mode, ranks]);
  const counts = useMemo(() => (mode === 'home' ? M.categoryCounts(s.catalog, showAll) : {}), [s.catalog, showAll, mode]);
  const subCounts = useMemo(() => (mode === 'cat' ? M.subcategoryCounts(s.catalog, category, showAll) : {}), [s.catalog, category, showAll, mode]);
  const catTotal = useMemo(() => (mode === 'cat' && subKey ? M.filterCatalog(s.catalog, { showAll, category }).length : list.length), [s.catalog, showAll, category, subKey, list, mode]);
  // the next page comes when the list is scrolled near its end (endless.js)
  const { limit, more } = usePaged(list.length, `${query}|${showAll}|${category}|${subKey}|${mode === 'coll' ? coll : ''}`);
  const openRef = useRef(onOpen);
  openRef.current = onOpen;
  const open = useCallback((id) => openRef.current(id), []);
  void working;
  if (!fh.supported) return h(Line, { th }, t('flathub.notSupported'));
  if (!fh.on) return h(View, null, h(TurnOn));
  // (2026-11-23) straight on the banner: what Flathub is, its switches, Refresh and Turn off are in its Settings
  // (FlathubSettings, the second column's item under Installed); Refresh is also the title bar's update icon
  return h(View, null,
    h(JobProgress),
    s.loaded && mode === 'home' ? h(View, { style: { gap: 26, marginBottom: 26 } },
      h(Spotlight, { b: spot, onOpen: open }),
      h(CategoryTiles, { counts, onCategory }),
      h(CollectionRows, { rows, onOpen: open, onCollection })) : null,
    s.loaded && mode === 'cat' ? h(SubChips, { sub: subKey, counts: subCounts, total: catTotal, onSub }) : null,
    s.loaded ? h(Header, { th }, mode === 'search' ? t('flathub.results', { count: list.length })
      : t('flathub.catalogTitle', { count: list.length })) : null,
    s.loaded && !list.length ? h(Line, { th }, t('flathub.noResults')) : null,
    h(CardGrid, null, ...list.slice(0, limit).map((a) => h(FlathubCard, { key: a.id, a, onOpen: open }))),
    h(MoreButton, { left: list.length - limit, onMore: more }));
}

// The section's Installed (its second column's first item, 2026-11-23): Update all, then the installed apps with Remove
// and Open. The shop (All, the categories) no longer lists them: its cards say Open for an installed app.
export function FlathubInstalled({ query, onOpen }) {
  const th = useTheme();
  const fh = useFlathub();
  const count = useStore(F.state, (x) => x.installed.length);
  const job = useStore(F.state, (x) => !!x.job);
  useEffect(() => { if (fh.on) { if (!F.state.get().loaded) F.loadCatalog(); F.refreshInstalled(); } }, [fh.on]);
  useEffect(() => (fh.on ? F.watchInstalled() : undefined), [fh.on]);      // installs made elsewhere show too
  if (!fh.supported) return h(Line, { th }, t('flathub.notSupported'));
  if (!fh.on) return h(View, null, h(TurnOn));
  return h(View, null,
    count ? h(View, { style: { flexDirection: 'row', alignItems: 'center', gap: spacing.s, marginBottom: spacing.l } },
      h(Text, { style: { flex: 1, minWidth: 0, fontSize: typeScale.callout, color: th.textDim, textAlign: 'start' } }, t('flathub.updates.text', { count })),
      h(Button, { small: true, label: t('flathub.updates.button'), disabled: job, onClick: () => F.updateAll() })) : null,
    h(JobProgress),
    h(InstalledList, { onOpen, query }));
}

// Flathub apps → Settings (2026-11-23, the second column's item under Installed): what Flathub is, the verified-only
// switch (with how many apps it hides), Flathub's rankings, refreshing the catalog and turning Flathub off.
export function FlathubSettings() {
  const th = useTheme();
  const fh = useFlathub();
  const catalog = useStore(F.state, (x) => x.catalog);
  const working = useStore(F.state, (x) => !!x.busy || !!x.job);
  const hidden = useMemo(() => (fh.prefs.showAll ? 0 : catalog.length - catalog.filter((a) => a.verified).length), [catalog, fh.prefs.showAll]);
  if (!fh.supported) return h(Line, { th }, t('flathub.notSupported'));
  if (!fh.on) return h(View, null, h(AboutText, null, t('flathub.subtitle')), h(View, { style: { height: spacing.xl } }), h(TurnOn));
  return h(SettingsPage, null,
    h(AboutText, null, t('flathub.subtitle')),
    h(JobProgress),
    h(Group, null,
      h(Row, { label: fh.prefs.showAll ? t('flathub.showAllOn') : t('flathub.showAllOff', { count: hidden }),
        control: h(Toggle, { on: fh.prefs.showAll, onChange: (v) => F.setPrefs({ showAll: v }) }) }),
      h(Row, { label: fh.prefs.rankings ? t('flathub.rankingsOn') : t('flathub.rankingsOff'), control: h(Toggle, { on: fh.prefs.rankings, onChange: F.setRankings }) })),
    h(Group, null,
      h(Row, { label: t('store.refresh.flathub'), control: h(Button, { small: true, label: t('flathub.refresh'), busy: working, onClick: () => F.loadCatalog({ fetch: true }) }) }),
      h(Row, { label: t('store.settings.flathubOff'), control: h(Button, { small: true, label: t('flathub.turnOff'), pending: true, onClick: F.turnOff }) })));
}

// How many shown Flathub apps each category has (the Store's second column)
export function useFlathubCounts() {
  const catalog = useStore(F.state, (x) => x.catalog);
  const showAll = useStore(settings, (st) => M.prefsOf(st.flathub).showAll);
  return useMemo(() => M.categoryCounts(catalog, showAll), [catalog, showAll]);
}
// How many Flathub apps are shown in all (the second column's "All"; an app in two categories is one app)
export function useFlathubTotal() {
  const catalog = useStore(F.state, (x) => x.catalog);
  const showAll = useStore(settings, (st) => M.prefsOf(st.flathub).showAll);
  return useMemo(() => M.shownCount(catalog, showAll), [catalog, showAll]);
}

// Discover's search: the Flathub apps that match, as their own shelf (when Flathub is on).
export function FlathubResults({ query, onOpen, onMore }) {
  const s = useStore(F.state);
  const fh = useFlathub();
  useEffect(() => { if (fh.on && !s.loaded) F.loadCatalog(); }, [fh.on]);
  if (!fh.on || !s.loaded) return null;
  const list = M.filterCatalog(s.catalog, { query, showAll: fh.prefs.showAll });
  if (!list.length) return null;
  return h(Shelf, { title: t('flathub.shelf'), more: t('store.seeAll', { count: list.length }), onMore },
    h(CardGrid, null, ...list.slice(0, SHELF).map((a) => h(FlathubCard, { key: a.id, a, onOpen }))));
}
export function FlathubCategory({ cat, query, onOpen }) {
  const s = useStore(F.state);
  const fh = useFlathub();
  const [all, setAll] = useState(false);
  useEffect(() => { if (fh.on && !s.loaded) F.loadCatalog(); }, [fh.on]);
  const list = useMemo(() => (fh.on && s.loaded ? M.filterCatalog(s.catalog, { query, showAll: fh.prefs.showAll, category: cat }) : []),
    [fh.on, s.loaded, s.catalog, query, fh.prefs.showAll, cat]);
  // "See all": every app, a page at a time as the list is scrolled (endless.js); it used to stop at 240
  const { limit, more } = usePaged(all ? list.length : 0, `${cat}|${query}|${fh.prefs.showAll}|${all}`);
  const openRef = useRef(onOpen);
  openRef.current = onOpen;
  const open = useCallback((id) => openRef.current(id), []);
  if (!list.length) return null;
  return h(Shelf, { title: t('flathub.shelf'), more: all ? '' : t('store.seeAll', { count: list.length }), onMore: all || list.length <= SHELF ? null : () => setAll(true) },
    h(CardGrid, null, ...list.slice(0, all ? limit : SHELF).map((a) => h(FlathubCard, { key: a.id, a, onOpen: open }))),
    all ? h(MoreButton, { left: list.length - limit, onMore: more }) : null);
}

// Store → Updates: Flathub's apps are updated together (Flatpak finds what changed), and the runtimes no app uses any
// more are removed after.
export function FlathubUpdates() {
  const th = useTheme();
  const s = useStore(F.state);
  const fh = useFlathub();
  useEffect(() => { if (fh.on) F.refreshInstalled(); }, [fh.on]);
  useEffect(() => (fh.on ? F.watchInstalled() : undefined), [fh.on]);      // installs made elsewhere show too
  if (!fh.on || !s.installed.length) return null;
  return h(View, { style: box(th) },
    h(Title, { th }, t('flathub.updates.title')),
    h(Line, { th }, t('flathub.updates.text', { count: s.installed.length })),
    h(JobProgress, { forId: '' }),
    h(View, { style: { flexDirection: 'row', justifyContent: 'flex-end' } },
      h(Button, { primary: true, label: t('flathub.updates.button'), disabled: !!s.job, onClick: () => F.updateAll() })));
}

// Store → Installed: removing a Flatpak app (the launcher's entry, "flatpak:<id>").
export const removeInstalled = (a) => {
  const x = F.installedApp(a.flatpakId) || { id: a.flatpakId, branch: 'stable' };
  return F.uninstall({ ...x, name: a.name });
};
export const flathubJob = () => F.state.get().job;

// ---- an app's page ----

const LEVEL = { never: '#ff453a', limited: '#ff9f0a', info: null };

function Permissions({ meta }) {
  const th = useTheme();
  if (!meta) return h(Line, { th }, t('flathub.perm.loading'));
  if (meta.error) return h(Line, { th, color: '#ff453a' }, t('flathub.perm.failed', { reason: meta.error }));
  const rows = M.permissionRows(meta);
  if (!rows.length) return h(GroupBox, null, h(View, { style: { paddingVertical: spacing.s + 2 } }, h(Line, { th, color: th.text }, t('flathub.perm.none'))));
  return h(GroupBox, null, ...rows.map((r, i) => h(View, { key: `${r.perm}${i}`, style: { flexDirection: 'row', gap: spacing.m, paddingVertical: spacing.s + 2, alignItems: 'flex-start' } },
    h(View, { style: { width: 8, height: 8, borderRadius: 4, marginTop: 6, backgroundColor: LEVEL[r.level] || th.textDim, flexShrink: 0 } }),
    h(View, { style: { flex: 1, minWidth: 0, gap: 2 } },
      h(Text, { style: { fontSize: typeScale.body, color: th.text, lineHeight: 1.4, textAlign: 'start' } }, t(`flathub.perm.${r.perm}`, r.args)),
      r.change ? h(Text, { style: { fontSize: typeScale.caption, color: LEVEL[r.level] || th.textDim, lineHeight: 1.4, textAlign: 'start' } }, t(`flathub.change.${r.change}`)) : null))));
}

// What the app says about itself (AppStream's description, in the catalog's language): shut to about six lines, with
// "Show more" when there is more. Nothing when it has none.
function About({ id, catalog }) {
  const th = useTheme();
  const [open, setOpen] = useState(false);
  const text = useMemo(() => F.description(id), [id, catalog]);     // read again after each catalog load (language)
  useEffect(() => setOpen(false), [id]);
  const v = M.descriptionView(text, { open });
  if (!v.paras.length) return null;
  return h(View, null,
    h(Header, { th }, t('flathub.about.title')),
    h(View, { style: { gap: spacing.s, paddingHorizontal: spacing.xs } },
      ...v.paras.map((p, i) => h(Text, { key: `${i}`, style: { fontSize: typeScale.body, color: th.text, lineHeight: 1.45, textAlign: 'start',
        ...(p.clamp ? { numberOfLines: p.clamp, textOverflow: 'ellipsis' } : {}) } }, p.text))),
    v.long ? h(Button, { plain: true, small: true, label: open ? t('flathub.about.less') : t('flathub.about.more'), onClick: () => setOpen(!open),
      style: { alignSelf: 'flex-start', marginTop: spacing.xs } }) : null);
}

export function FlathubDetail({ id }) {
  const th = useTheme();
  const s = useStore(F.state);
  const fh = useFlathub();
  const exported = useExported(id);
  const inst = s.installed.find((x) => x.id === id) || null;
  const a = F.catalogApp(id) || (inst || exported ? { id, branch: inst ? inst.branch : 'stable', name: exported ? exported.name : id, summary: '', icon: '', license: '', developer: '', verified: false, unknown: true } : null);
  const ref = a ? M.refOf(a.id, a.branch) : null;
  const [meta, setMeta] = useState(null);
  const [asking, setAsking] = useState(false);           // Remove asks first (ConfirmRemove)
  useEffect(() => {
    setMeta(null);
    let live = true;
    if (fh.on && ref) F.info(ref).then((m) => { if (live) setMeta(m); });
    return () => { live = false; };
  }, [ref, fh.on]);
  useEffect(() => { if (fh.on) { if (!s.loaded) F.loadCatalog(); F.refreshInstalled(); } }, [fh.on]);
  if (!fh.on) return h(View, null, h(TurnOn));
  if (!a) return h(Line, { th }, s.loaded ? t('flathub.gone') : t('flathub.busy.catalog'));
  const busy = !!s.job;
  const mine = s.job && s.job.id === a.id;
  const lic = M.licenseView(a.license);
  const buttons = inst
    ? [h(Button, { key: 'rm', label: t('flathub.remove'), disabled: busy, onClick: () => !busy && setAsking(true) }),
      h(Button, { key: 'up', label: t('flathub.update'), disabled: busy, onClick: () => !busy && F.update({ ...inst, name: a.name }) }),
      h(Button, { key: 'open', primary: true, label: t('store.open'), onClick: () => openApp(a.id) })]
    : [h(Button, { key: 'in', primary: true, label: t('store.install'), disabled: busy || !meta || !!meta.error,
      onClick: () => !busy && meta && !meta.error && F.install(a) })];
  return h(View, { style: { gap: spacing.xl } },
    h(View, { style: { flexDirection: 'row', gap: spacing.l, alignItems: 'center' } },
      h(FlathubIcon, { a, size: 96 }),
      h(View, { style: { flex: 1, minWidth: 0, gap: 4 } },
        h(Text, { style: { fontSize: typeScale.largeTitle, fontWeight: 600, color: th.text, textAlign: 'start' } }, a.name),
        a.summary ? h(Text, { style: { fontSize: typeScale.callout, color: th.textDim, lineHeight: 1.4, textAlign: 'start' } }, a.summary) : null,
        h(View, { style: { flexDirection: 'row', alignItems: 'center', gap: spacing.s, marginTop: 2 } },
          h(Tag, { label: 'Flathub' }),
          h(Text, { style: { fontSize: typeScale.caption, color: a.verified ? '#30d158' : '#ff9f0a', textAlign: 'start' } },
            a.verified ? t('flathub.verified', { developer: a.developer || a.id }) : t('flathub.unverified')))),
      h(View, { style: { flexDirection: 'row', gap: spacing.s, alignItems: 'center' } }, ...buttons)),
    asking && inst ? h(ConfirmRemove, { name: a.name, onNo: () => setAsking(false), onYes: () => { setAsking(false); if (!s.job) F.uninstall({ ...inst, name: a.name }); } }) : null,
    mine || (!s.job && s.msg && s.msg.id === a.id) ? h(JobProgress, { forId: a.id }) : null,
    a.unknown ? null : h(About, { id: a.id, catalog: s.catalog }),
    h(Facts, { items: [
      [t('flathub.fact.developer'), a.developer],
      [t('flathub.fact.license'), lic.kind === 'proprietary' ? t('flathub.license.proprietary') : lic.text,
        lic.url ? { url: lic.url, title: `${t('flathub.license.open')}: ${lic.url}` } : null],
      [t('flathub.fact.version'), inst && inst.version],
      [t('flathub.fact.branch'), a.branch],
    ] }),
    h(DisplayToolkit, { meta }),
    h(View, null,
      h(Header, { th }, t('flathub.perm.title')),
      h(Permissions, { meta })),
    M.appNote(a.id) ? h(Line, { th, color: th.text }, t(`flathub.note.${M.appNote(a.id)}`)) : null,
    h(Line, { th }, t('flathub.howItRuns')),
    h(Text, { style: { fontSize: typeScale.caption, color: th.textDim, direction: 'ltr', textAlign: 'start' } }, a.id));
}
