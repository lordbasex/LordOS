// flathubpages.js — the Store's Flathub section (docs/FLATPAK.md, phase 1 step 5): turning Flathub on (the `flatpak`
// package through the Store's package job, then the remote and the catalog), the catalog as cards (Flathub's verified
// apps unless the setting shows the rest), each app's page with what it declares and what LordOS changes before it is
// installed, and the installed ones with Update and Remove. Chromium and Electron apps install like any other: their
// helpers' sub-sandboxes are served by LordOS (docs/FLATPAK.md, "Sub-sandboxes"). The jobs and state are
// sdk/flathub.js's; the logic sdk/flathubmodel.js's.
import { createElement as h, useEffect, useMemo, useState } from 'react';
import {
  View, Text, Image, Button, GroupBox, Toggle, LetterIcon, useTheme, useStore, useLauncherApps, launchApp, admin, formatBytes, errorText, settings,
  flathubModel as M, typeScale, spacing, t,
} from '@lordos/sdk';
import { debian } from './debian.js';
import { Card, CardGrid, Facts, Pill, Shelf, Tag } from './ui.js';
import { flathub as F } from '@lordos/sdk';

const PAGE = 60;            // cards shown at first, and how many "Show more" adds
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

export function FlathubCard({ a, onOpen }) {
  // only whether this app is installed: a job's progress doesn't redraw every card
  const installed = useStore(F.state, (s) => s.installed.some((x) => x.id === a.id));
  return h(Card, {
    icon: h(FlathubIcon, { a, size: 56 }), name: a.name, tag: 'Flathub', kind: [catLabel(a), a.developer].filter(Boolean).join(' · '), summary: a.summary,
    note: a.verified ? '' : t('flathub.unverifiedShort'),
    action: installed ? h(Pill, { label: t('store.open'), onClick: () => openApp(a.id) }) : h(Pill, { label: t('store.get'), onClick: () => onOpen(a.id) }),
    onClick: () => onOpen(a.id),
  });
}

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

function InstalledList({ onOpen }) {
  const th = useTheme();
  const s = useStore(F.state);
  if (!s.installed.length) return null;
  const busy = !!s.job;
  return h(View, { style: { marginBottom: spacing.xl } },
    h(Header, { th }, t('flathub.installedTitle', { count: s.installed.length })),
    h(GroupBox, null, ...s.installed.map((x) => {
      const a = F.catalogApp(x.id) || { id: x.id, name: x.id, summary: '', icon: '' };
      return h(View, { key: x.id, style: { flexDirection: 'row', alignItems: 'center', gap: spacing.m, paddingVertical: spacing.s + 2 } },
        h(FlathubIcon, { a, size: 32 }),
        h(View, { style: { flex: 1, minWidth: 0, gap: 2 } },
          h(Text, { onClick: () => onOpen(x.id), style: { fontSize: typeScale.body, fontWeight: 600, color: th.text, textAlign: 'start', whiteSpace: 'nowrap', overflow: 'hidden' } }, a.name),
          h(Text, { style: { fontSize: typeScale.caption, color: th.textDim, textAlign: 'start', whiteSpace: 'nowrap', overflow: 'hidden' } },
            x.version ? t('store.version', { version: x.version }) : x.id)),
        h(Button, { small: true, label: t('flathub.remove'), busy, pending: true, onClick: () => !busy && F.uninstall({ ...x, name: a.name }) }),
        h(Button, { small: true, label: t('store.open'), onClick: () => openApp(x.id) }));
    })));
}

export function FlathubPage({ query, onOpen, category = null }) {
  const th = useTheme();
  // what the page itself shows; a job's progress is JobProgress's own (it changes several times a second)
  const catalog = useStore(F.state, (x) => x.catalog);
  const loaded = useStore(F.state, (x) => x.loaded);
  const working = useStore(F.state, (x) => !!x.busy || !!x.job);
  const s = { catalog, loaded };
  const fh = useFlathub();
  const [limit, setLimit] = useState(PAGE);
  useEffect(() => { setLimit(PAGE); }, [query, fh.prefs.showAll, category]);
  useEffect(() => { if (fh.on) { if (!s.loaded) F.loadCatalog(); F.refreshInstalled(); } }, [fh.on]);
  const list = useMemo(() => M.filterCatalog(s.catalog, { query, showAll: fh.prefs.showAll, category }), [s.catalog, query, fh.prefs.showAll, category]);
  const hidden = useMemo(() => (fh.prefs.showAll ? 0 : s.catalog.length - s.catalog.filter((a) => a.verified).length), [s.catalog, fh.prefs.showAll]);
  if (!fh.supported) return h(Line, { th }, t('flathub.notSupported'));
  if (!fh.on) return h(View, null, h(TurnOn));
  return h(View, null,
    h(Text, { style: { fontSize: typeScale.body, color: th.textDim, lineHeight: 1.45, maxWidth: 680, marginBottom: spacing.l, textAlign: 'start' } }, t('flathub.subtitle')),
    h(View, { style: { flexDirection: 'row', alignItems: 'center', gap: spacing.s, marginBottom: spacing.l } },
      h(Toggle, { on: fh.prefs.showAll, onChange: (v) => F.setPrefs({ showAll: v }) }),
      h(Text, { style: { flex: 1, minWidth: 0, fontSize: typeScale.callout, color: th.text, textAlign: 'start' } },
        fh.prefs.showAll ? t('flathub.showAllOn') : t('flathub.showAllOff', { count: hidden })),
      h(Button, { small: true, label: t('flathub.refresh'), busy: working, onClick: () => F.loadCatalog({ fetch: true }) }),
      h(Button, { small: true, label: t('flathub.turnOff'), pending: true, onClick: F.turnOff })),
    h(JobProgress),
    h(InstalledList, { onOpen }),
    s.loaded ? h(Header, { th }, query ? t('flathub.results', { count: list.length }) : t('flathub.catalogTitle', { count: list.length })) : null,
    s.loaded && !list.length ? h(Line, { th }, t('flathub.noResults')) : null,
    h(CardGrid, null, ...list.slice(0, limit).map((a) => h(FlathubCard, { key: a.id, a, onOpen }))),
    list.length > limit ? h(View, { style: { alignItems: 'center', marginTop: spacing.l } },
      h(Button, { label: t('flathub.showMore', { count: list.length - limit }), onClick: () => setLimit(limit + PAGE) })) : null);
}

// How many shown Flathub apps each category has (the Store's second column)
export function useFlathubCounts() {
  const catalog = useStore(F.state, (x) => x.catalog);
  const showAll = useStore(settings, (st) => M.prefsOf(st.flathub).showAll);
  return useMemo(() => M.categoryCounts(catalog, showAll), [catalog, showAll]);
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
  if (!fh.on || !s.loaded) return null;
  const list = M.filterCatalog(s.catalog, { query, showAll: fh.prefs.showAll, category: cat });
  if (!list.length) return null;
  return h(Shelf, { title: t('flathub.shelf'), more: all ? '' : t('store.seeAll', { count: list.length }), onMore: all || list.length <= SHELF ? null : () => setAll(true) },
    h(CardGrid, null, ...list.slice(0, all ? 240 : SHELF).map((a) => h(FlathubCard, { key: a.id, a, onOpen }))));
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

export function FlathubDetail({ id }) {
  const th = useTheme();
  const s = useStore(F.state);
  const fh = useFlathub();
  const exported = useExported(id);
  const inst = s.installed.find((x) => x.id === id) || null;
  const a = F.catalogApp(id) || (inst || exported ? { id, branch: inst ? inst.branch : 'stable', name: exported ? exported.name : id, summary: '', icon: '', license: '', developer: '', verified: false, unknown: true } : null);
  const ref = a ? M.refOf(a.id, a.branch) : null;
  const [meta, setMeta] = useState(null);
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
  const buttons = inst
    ? [h(Button, { key: 'rm', label: t('flathub.remove'), disabled: busy, onClick: () => !busy && F.uninstall({ ...inst, name: a.name }) }),
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
    mine || (!s.job && s.msg && s.msg.id === a.id) ? h(JobProgress, { forId: a.id }) : null,
    h(Facts, { items: [
      [t('flathub.fact.developer'), a.developer],
      [t('flathub.fact.license'), a.license],
      [t('flathub.fact.version'), inst && inst.version],
      [t('flathub.fact.branch'), a.branch],
    ] }),
    h(View, null,
      h(Header, { th }, t('flathub.perm.title')),
      h(Permissions, { meta })),
    M.appNote(a.id) ? h(Line, { th, color: th.text }, t(`flathub.note.${M.appNote(a.id)}`)) : null,
    h(Line, { th }, t('flathub.howItRuns')),
    h(Text, { style: { fontSize: typeScale.caption, color: th.textDim, direction: 'ltr', textAlign: 'start' } }, a.id));
}
