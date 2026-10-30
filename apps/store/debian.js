// debian.js — the Debian part of the Store. It reads our own index (lord.pkg, no root;
// server/src/pkg) and makes changes through the administration helper (pkg-update, pkg-install,
// pkg-remove, pkg-upgrade, pkg-install-file). Before changing anything it shows the plan: what is
// installed, upgraded or removed, and how much is downloaded and used. See docs/PACKAGES.md.
import { createElement as h, useEffect, useState } from 'react';
import {
  View, Text, Image, Button, EmptyState, GroupBox, useTheme, useStore, createStore, admin, formatBytes, formatDate,
  refreshApps, launchApp, useLauncherApps, LetterIcon, typeScale, spacing, direction, t, locale, errorText, statusText,
  Markdown, Toggle, LordOSLogo, session, lordosUpdate, restartNeeded, updatePrefs, setUpdatePrefs,
} from '@lordos/sdk';
import { Suggested } from './suggested.js';
import { Card, CardGrid, Facts, Pill, Shelf } from './ui.js';
import { sysPkgKey, snapshotDate } from './syspkg.js';

const available = () => typeof lord.pkg !== 'undefined';
const ready = () => available() && lord.pkg.available();
const lang2 = () => (locale.get().lang || 'en').split('-')[0];

// Shared state: generation (changes when the index or what is installed changes) and the running job.
const state = createStore({ gen: 0, job: null, msg: null });
const bump = () => state.set((s) => ({ ...s, gen: s.gen + 1 }));
admin.onEvent((text) => {
  if (text === 'pkg index' || text === 'pkg changed') { bump(); if (text === 'pkg changed') refreshApps(); }
});

// Runs a package action showing its progress ("progress <phase> <done> <total>"). The other LOG
// lines and the final OK are status code lines (server/src/pkg/pkg.c), shown with statusText().
function runJob(action, arg, title) {
  state.set((s) => ({ ...s, job: { title, phase: '', done: 0, total: 0, log: [] }, msg: null }));
  const onLog = (line) => {
    const m = /^progress (\w+) (\d+) (\d+)$/.exec(line);
    state.set((s) => ({ ...s, job: s.job && (m
      ? { ...s.job, phase: m[1], done: Number(m[2]), total: Number(m[3]) }
      : /^plan /.test(line) ? s.job : { ...s.job, log: [...s.job.log, statusText(line)].slice(-4) }) }));
  };
  return admin.run(action, arg, onLog).then(
    (ok) => { state.set((s) => ({ ...s, job: null, msg: { ok: true, text: statusText(ok) } })); bump(); refreshApps(); return true; },
    (e) => { state.set((s) => ({ ...s, job: null, msg: { ok: false, text: e.message } })); bump(); return false; },
  );
}

export const useDebian = () => {
  const s = useStore(state);
  const upgrades = ready() ? lord.pkg.upgrades() : [];
  return { ...s, upgrades };
};

// ---- pieces ----

// A rounded inset box (plan, progress), like GroupBox without rows.
const boxStyle = (th, extra) => ({ padding: spacing.xl, borderRadius: 10, backgroundColor: th.mode === 'dark' ? 'rgba(255,255,255,0.04)' : 'rgba(0,0,0,0.025)',
  border: `1px solid ${th.separator}`, gap: spacing.s, ...extra });

// A key/value row of an info box: the label at the start, the value right-aligned and dim.
const InfoRow = ({ th, label, value }) => h(View, { style: { minHeight: 34, flexDirection: 'row', alignItems: 'center', gap: spacing.m, paddingVertical: 4 } },
  h(Text, { style: { fontSize: typeScale.body, color: th.text, whiteSpace: 'nowrap', textAlign: 'start' } }, label),
  h(Text, { style: { flex: 1, minWidth: 0, fontSize: typeScale.body, color: th.textDim, textAlign: 'end', whiteSpace: 'nowrap', overflow: 'hidden' } }, value));

// A section header (13/600, sentence case) above an inset box.
const Header = ({ th, children }) => h(Text, { style: { fontSize: typeScale.body, fontWeight: 600, color: th.text, paddingHorizontal: spacing.xs, marginBottom: 6, textAlign: 'start' } }, children);
const titleStyle = (th) => ({ fontSize: typeScale.largeTitle, fontWeight: 600, color: th.text });

function PkgIcon({ app, name, size = 48 }) {
  const icon = app?.icon || (available() ? lord.pkg.icon(name) : null);
  if (icon) return h(Image, { src: icon, style: { width: size, height: size, objectFit: 'contain' } });
  return h(LetterIcon, { size, name: app?.name || name });
}

// A package's name as the Updates list and the plans show it: LordOS's data packages in words
// (syspkg.js), every other package by its name.
const pkgLabel = (name) => { const k = sysPkgKey(name); return k ? t(`${k}.name`) : name; };
// The line under it: for a data package, what it is and its date (in the user's region when the
// version is a date); otherwise the versions and the package's summary.
function pkgLine(u) {
  const k = sysPkgKey(u.name);
  if (!k) return `${u.from} → ${u.to} · ${u.summary}`;
  const d = snapshotDate(u.to);
  return t(`${k}.summary`, { date: d ? formatDate(d, 'numeric') : u.to });
}

function Progress() {
  const th = useTheme();
  const { job, msg } = useStore(state);
  if (!job && !msg) return null;
  if (!job) return h(Text, { style: { fontSize: 12, color: msg.ok ? '#30d158' : '#ff453a', marginBottom: 10 } }, msg.text);
  const f = job.total ? Math.min(1, job.done / job.total) : 0;
  const phase = job.phase ? t(`store.phase.${job.phase}`) : t('store.working');
  return h(View, { style: boxStyle(th, { gap: 6, marginBottom: 4 }) },
    h(Text, { style: { fontSize: 13, fontWeight: 600, color: th.text } }, `${job.title} — ${phase}`),
    h(View, { style: { height: 6, borderRadius: 3, backgroundColor: th.control, direction: 'ltr' } },
      h(View, { style: { width: `${Math.round(f * 100)}%`, height: 6, borderRadius: 3, backgroundColor: th.accent } })),
    job.phase === 'download' && job.total ? h(Text, { style: { fontSize: 11, color: th.textDim } }, `${formatBytes(job.done)} / ${formatBytes(job.total)}`) : null,
    ...job.log.map((l, i) => h(Text, { key: i, style: { fontSize: 11, color: th.textDim, whiteSpace: 'nowrap', overflow: 'hidden' } }, l)));
}

// Summary of a plan: what happens and how much space it takes.
function PlanView({ plan }) {
  const th = useTheme();
  if (!plan.ok) return h(Text, { style: { fontSize: 12, color: '#ff453a' } }, (plan.error && errorText(plan.error)) || t('store.plan.impossible'));
  const by = (a) => plan.items.filter((i) => i.action === a);
  const line = (label, list) => (list.length ? h(Text, { style: { fontSize: 12, color: th.text, lineHeight: 1.4 } },
    `${label}: ${list.slice(0, 14).map((i) => pkgLabel(i.name)).join(', ')}${list.length > 14 ? ` ${t('store.plan.more', { count: list.length - 14 })}` : ''}`) : null);
  const delta = plan.installedDelta;
  return h(View, { style: { gap: 4 } },
    line(t('store.plan.install', { count: by('install').length }), by('install')),
    line(t('store.plan.upgrade', { count: by('upgrade').length }), by('upgrade')),
    line(t('store.plan.remove', { count: by('remove').length }), by('remove')),
    h(Text, { style: { fontSize: 12, color: th.textDim } }, t('store.plan.sizes', {
      download: formatBytes(plan.download), disk: `${delta < 0 ? '−' : '+'}${formatBytes(Math.abs(delta))}`,
    })));
}

// ---- package page ----

function Details({ name, onBack, onRemoved }) {
  const th = useTheme();
  const { gen, job } = useStore(state);
  const [plan, setPlan] = useState(null);
  const info = ready() ? lord.pkg.info(name, { lang: lang2() }) : null;
  useEffect(() => setPlan(null), [name, gen]);
  const launcher = useLauncherApps();
  if (!info) return h(Text, { style: { color: th.textDim } }, t('store.debian.notFound', { name }));
  const app = info.app;
  const isAdmin = admin.available();
  // the matching installed desktop app (for "Open")
  const desk = app && launcher.find((a) => a.kind === 'linux' && (`${a.desktopId}.desktop` === app.id || a.desktopId === app.id));
  const ask = (p) => setPlan(p);
  const doInstall = () => ask({ kind: 'install', p: lord.pkg.plan({ install: [name] }) });
  const doRemove = () => ask({ kind: 'remove', p: lord.pkg.plan({ remove: [name] }) });
  const confirm = () => {
    const extra = plan.p.items.some((i) => i.action === 'remove' && !i.requested);
    if (plan.kind === 'install') runJob('pkg-install', extra ? `${name} --allow-remove` : name, t('store.job.install', { name: app?.name || name }));
    else runJob('pkg-remove', extra ? `${name} --cascade` : name, t('store.job.remove', { name: app?.name || name })).then((ok) => { if (ok && onRemoved) onRemoved(name); });
    setPlan(null);
  };
  const desc = (info.appDescription || info.description || '').trim();
  const busy = !!job;
  const primary = !info.installed
    ? h(Button, { primary: true, label: t('store.install'), disabled: !isAdmin || busy, onClick: () => isAdmin && !job && doInstall() })
    : desk ? h(Button, { primary: true, label: t('store.open'), onClick: () => launchApp(desk.id) }) : null;
  return h(View, { style: { gap: spacing.xl } },
    onBack ? h(Button, { plain: true, small: true, label: `‹ ${t('store.back')}`, onClick: onBack, style: { alignSelf: 'flex-start', [direction() === 'rtl' ? 'marginRight' : 'marginLeft']: -6 } }) : null,
    h(Progress),
    // header: icon, name, summary and the actions under them (App Store style)
    h(View, { style: { flexDirection: 'row', gap: spacing.l, alignItems: 'flex-start' } },
      h(View, { style: { borderRadius: 22, overflow: 'hidden', boxShadow: '0 6px 14px rgba(0,0,0,0.22)' } }, h(PkgIcon, { app, name, size: 96 })),
      h(View, { style: { flex: 1, minWidth: 0, gap: spacing.xs } },
        h(Text, { style: { ...titleStyle(th), textAlign: 'start' } }, app?.name || name),
        h(Text, { style: { fontSize: typeScale.body, color: th.textDim, lineHeight: 1.35, textAlign: 'start' } }, app?.summary || info.summary),
        h(View, { style: { flexDirection: 'row', flexWrap: 'wrap', alignItems: 'center', gap: spacing.s, marginTop: spacing.s } },
          primary,
          info.upgradable && h(Button, { label: t('store.upgrade'), disabled: !isAdmin || busy, onClick: () => isAdmin && !job && doInstall() }),
          info.installed && !info.essential && h(Button, { label: t('store.remove'), disabled: !isAdmin || busy, onClick: () => isAdmin && !job && doRemove() }),
          info.installed ? h(Text, { style: { fontSize: typeScale.caption, fontWeight: 500, color: '#30d158', marginLeft: spacing.xs } }, t('store.installedBadge')) : null))),
    !isAdmin && h(Text, { style: { fontSize: typeScale.caption, color: '#ff9f0a', textAlign: 'start' } }, t('store.debian.needAdmin')),
    h(Facts, { items: [
      [t('store.info.version'), info.version],
      [t('store.info.size'), formatBytes(info.installedSize)],
      [t('store.info.category'), catOf(app || {}) ? t(`store.cat.${catOf(app)}`) : ''],
      [t('store.info.suite'), info.suite ? `Debian ${info.suite}` : ''],
    ] }),
    plan && h(View, { style: boxStyle(th) },
      h(Text, { style: { fontSize: typeScale.headline, fontWeight: 600, color: th.text, textAlign: 'start' } }, t(plan.kind === 'install' ? 'store.plan.titleInstall' : 'store.plan.titleRemove')),
      h(PlanView, { plan: plan.p }),
      h(View, { style: { flexDirection: 'row', gap: spacing.s, justifyContent: 'flex-end', marginTop: spacing.m } },
        h(Button, { label: t('store.cancel'), onClick: () => setPlan(null) }),
        plan.p.ok && h(Button, { primary: true, label: t('store.plan.confirm'), onClick: confirm }))),
    desc ? h(View, null,
      h(Header, { th }, t('store.info.description')),
      h(Text, { style: { fontSize: typeScale.body, color: th.text, lineHeight: 1.45, maxWidth: 620, paddingHorizontal: spacing.xs, textAlign: 'start' } }, desc.slice(0, 1600))) : null,
    h(View, null,
      h(Header, { th }, t('store.info.title')),
      h(GroupBox, null,
        h(InfoRow, { th, label: t('store.info.package'), value: name }),
        info.installed ? h(InfoRow, { th, label: t('store.info.installed'), value: info.installed }) : null,
        info.homepage ? h(InfoRow, { th, label: t('store.info.homepage'), value: info.homepage }) : null)));
}

// ---- app grid ----

// The category a card names: the first of the Store's categories the app is in.
const catOf = (a) => (a.categories || []).find((c) => CATS.includes(c));

// A Debian app's card: Get (the app page and its plan), Open when it is installed and has a window,
// View otherwise.
function DebCard({ a, onOpen }) {
  const launcher = useLauncherApps();
  const app = a.app || a;
  const name = a.package || a.name;
  const desk = a.installed && launcher.find((l) => l.kind === 'linux' && app.id && (`${l.desktopId}.desktop` === app.id || l.desktopId === app.id));
  const cat = catOf(app);
  const action = desk ? h(Pill, { label: t('store.open'), onClick: () => launchApp(desk.id) })
    : h(Pill, { label: a.installed ? t('store.ai.view') : t('store.get'), onClick: () => onOpen(name) });
  return h(Card, { icon: h(PkgIcon, { app, name, size: 56 }), name: app.name || name, kind: cat ? t(`store.cat.${cat}`) : '',
    summary: app.summary || a.summary || '', note: a.size ? formatBytes(a.size) : '', action, onClick: () => onOpen(name) });
}

const Grid = ({ items, onOpen }) => h(CardGrid, null, ...items.map((a) => h(DebCard, { key: `${a.package || a.name}:${a.id || ''}`, a, onOpen })));

// A drawn bag, monochrome, for the Store's empty states.
function StoreGlyph() {
  const th = useTheme();
  const c = th.textDim;
  return h(View, { style: { width: 48, height: 48, marginBottom: 4 } },
    h(View, { style: { position: 'absolute', left: 15, top: 2, width: 18, height: 16, borderRadius: 9, border: `3px solid ${c}` } }),
    h(View, { style: { position: 'absolute', left: 4, top: 12, width: 40, height: 34, borderRadius: 7, backgroundColor: c } }));
}

export const CATS = ['AudioVideo', 'Development', 'Education', 'Game', 'Graphics', 'Network', 'Office', 'Science', 'System', 'Utility'];
const categoryCounts = () => (ready() ? lord.pkg.categories() : {});

// No index yet (or no engine): say so, with the button that fetches the lists.
function NoIndex() {
  if (!available()) return h(EmptyState, { mascot: 'confused', title: t('store.debian.unavailable'), style: { minHeight: 360 } });
  return h(View, { style: { gap: 12 } }, h(Progress),
    h(EmptyState, { icon: h(StoreGlyph), title: t('store.debian.noIndexTitle'), hint: t('store.debian.noIndex'), style: { minHeight: 380 },
      action: h(Button, { primary: true, label: t('store.debian.update'), onClick: () => runJob('pkg-update', '', t('store.job.update')) }) }));
}

// Search results: apps as cards, other packages as rows.
function Results({ q, onOpen }) {
  const th = useTheme();
  const res = lord.pkg.search(q, { limit: 60, lang: lang2() });
  const apps = res.filter((r) => r.app), others = res.filter((r) => !r.app);
  return h(View, { style: { gap: spacing.l } },
    h(Progress),
    apps.length ? h(Shelf, { title: t('store.debian.results', { q }) }, h(Grid, { items: apps.map((r) => ({ ...r, package: r.name })), onOpen })) : null,
    others.length ? h(View, { style: { marginTop: spacing.s } },
      h(Header, { th }, t('store.debian.otherPackages')),
      h(GroupBox, null, ...others.slice(0, 30).map((r) => h(View, { key: r.name, onClick: () => onOpen(r.name),
        style: { flexDirection: 'row', alignItems: 'center', gap: spacing.m, minHeight: 44, paddingVertical: 6 } },
        h(View, { style: { flex: 1, minWidth: 0, gap: 2 } },
          h(Text, { style: { fontSize: typeScale.body, fontWeight: 600, color: th.text, whiteSpace: 'nowrap', overflow: 'hidden', textAlign: 'start' } }, r.name),
          h(Text, { style: { fontSize: typeScale.callout, color: th.textDim, whiteSpace: 'nowrap', overflow: 'hidden', textAlign: 'start' } }, r.summary)),
        r.installed ? h(Text, { style: { fontSize: typeScale.caption, fontWeight: 500, color: '#30d158' } }, t('store.installedBadge')) : null,
        h(Text, { style: { fontSize: 15, color: th.textDim } }, direction() === 'rtl' ? '‹' : '›'))))) : null,
    res.length === 0 ? h(EmptyState, { icon: h(StoreGlyph), title: t('store.debian.none'), hint: t('store.debian.noneHint'), style: { minHeight: 300 } }) : null);
}

// Discover: what the page above it brings (`top`: the featured app and the AI shelf), the
// suggestions for this computer, then a shelf per category with its first apps and "See all".
function Discover({ query, onOpen, onCategory, top }) {
  const { gen } = useStore(state);
  void gen;
  const q = query.trim();
  if (!ready()) return h(View, { style: { gap: 26 } }, top, h(NoIndex));
  if (q) return h(Results, { q, onOpen });
  const counts = categoryCounts();
  return h(View, { style: { gap: 26 } },
    h(Progress),
    top,
    ...CATS.filter((c) => counts[c]).map((c) => h(Shelf, { key: c, title: t(`store.cat.${c}`), more: t('store.seeAll', { count: counts[c] }), onMore: () => onCategory(c) },
      h(Grid, { items: lord.pkg.apps({ category: c, limit: 4, lang: lang2() }), onOpen }))));
}

// Store → For this computer: the hardware suggestions on their own page.
function SuggestedPage() {
  return h(View, { style: { gap: spacing.l } }, h(Progress), h(Suggested, { deb: { state, runJob, PlanView, boxStyle }, page: true }));
}

// One category: all its apps (the search narrows them).
function Category({ cat, query, onOpen }) {
  const { gen } = useStore(state);
  void gen;
  if (!ready()) return h(NoIndex);
  const q = query.trim().toLowerCase();
  const list = lord.pkg.apps({ category: cat, limit: 2000, lang: lang2() })
    .filter((a) => !q || `${a.name} ${a.summary || ''} ${a.package}`.toLowerCase().includes(q));
  return h(View, { style: { gap: spacing.l } },
    h(Progress),
    list.length ? h(Grid, { items: list.slice(0, q ? 200 : 400), onOpen })
      : h(EmptyState, { icon: h(StoreGlyph), title: t('store.debian.none'), hint: t('store.debian.noneHint'), style: { minHeight: 300 } }));
}

// LordOS itself (docs/UPDATES.md): "LordOS <version> is available", its release notes and Update.
function LordOSCard({ up }) {
  const th = useTheme();
  const { job } = useStore(state);
  const [plan, setPlan] = useState(null);
  const [all, setAll] = useState(false);
  const isAdmin = admin.available();
  const notes = (up.notes || '').trim();
  const lines = notes.split('\n');
  const long = lines.length > 14;
  const confirm = () => {
    const extra = plan.items.some((i) => i.action === 'remove' && !i.requested);
    runJob('pkg-install', extra ? 'lordos --allow-remove' : 'lordos', t('store.system.updateJob', { version: up.version }));
    setPlan(null);
  };
  return h(View, { style: boxStyle(th, { gap: spacing.m }) },
    h(View, { style: { flexDirection: 'row', alignItems: 'center', gap: spacing.l } },
      h(LordOSLogo, { size: 56 }),
      h(View, { style: { flex: 1, minWidth: 0, gap: 2 } },
        h(Text, { style: { fontSize: typeScale.title, fontWeight: 600, color: th.text, textAlign: 'start' } }, t('store.system.available', { version: up.version })),
        h(Text, { style: { fontSize: typeScale.callout, color: th.textDim, textAlign: 'start' } },
          t('store.system.availableDetail', { installed: up.installed, size: formatBytes(up.size || 0) }))),
      h(Button, { primary: true, label: t('store.system.update'), disabled: !isAdmin || !!job,
        onClick: () => isAdmin && !job && setPlan(lord.pkg.plan({ install: ['lordos'] })) })),
    !isAdmin && h(Text, { style: { fontSize: typeScale.caption, color: '#ff9f0a', textAlign: 'start' } }, t('store.debian.needAdmin')),
    plan && h(View, { style: { gap: spacing.s } },
      h(PlanView, { plan }),
      h(Text, { style: { fontSize: typeScale.caption, color: th.textDim, lineHeight: 1.4, textAlign: 'start' } }, t('store.system.keepWorking')),
      h(View, { style: { flexDirection: 'row', gap: spacing.s, justifyContent: 'flex-end' } },
        h(Button, { label: t('store.cancel'), onClick: () => setPlan(null) }),
        plan.ok && h(Button, { primary: true, label: t('store.system.update'), onClick: confirm }))),
    notes ? h(View, { style: { gap: spacing.xs } },
      h(Hr, { th }),
      h(Text, { style: { fontSize: typeScale.body, fontWeight: 600, color: th.text, textAlign: 'start' } }, t('store.system.whatsNew')),
      h(Markdown, { source: all || !long ? notes : lines.slice(0, 14).join('\n') }),
      long ? h(Button, { plain: true, small: true, label: all ? t('store.system.less') : t('store.system.more'), onClick: () => setAll(!all), style: { alignSelf: 'flex-start' } }) : null)
      : h(Text, { style: { fontSize: typeScale.callout, color: th.textDim, textAlign: 'start' } }, t('store.system.noNotes')));
}

const Hr = ({ th }) => h(View, { style: { height: 1, backgroundColor: th.separator, marginVertical: spacing.xs } });

// "Restart required": an installed update takes effect after a restart. Never restarts on its own.
function RestartCard({ need }) {
  const th = useTheme();
  const [later, setLater] = useState(false);
  if (later) return null;
  const canRestart = typeof session.canPower === 'function' && session.canPower();
  const kernel = need.packages.some((p) => p.startsWith('linux-image'));
  const what = need.packages.includes('lordos') ? t('store.restart.lordos') : kernel ? t('store.restart.kernel') : t('store.restart.other');
  return h(View, { style: boxStyle(th, { flexDirection: 'row', alignItems: 'center', gap: spacing.l, borderColor: th.accent }) },
    h(View, { style: { width: 36, height: 36, borderRadius: 18, backgroundColor: th.accent, alignItems: 'center', justifyContent: 'center' } },
      h(Text, { style: { fontSize: 20, color: '#fff', lineHeight: 36 } }, '⟳')),
    h(View, { style: { flex: 1, minWidth: 0, gap: 2 } },
      h(Text, { style: { fontSize: typeScale.body, fontWeight: 600, color: th.text, textAlign: 'start' } }, t('store.restart.title')),
      h(Text, { style: { fontSize: typeScale.callout, color: th.textDim, lineHeight: 1.4, textAlign: 'start' } },
        `${what} ${canRestart ? t('store.restart.save') : t('store.restart.how')}`)),
    h(Button, { label: t('store.restart.later'), onClick: () => setLater(true) }),
    canRestart ? h(Button, { primary: true, label: t('store.restart.now'), onClick: () => session.restart() }) : null);
}

function Updates() {
  const th = useTheme();
  const { job, gen } = useStore(state);
  const [plan, setPlan] = useState(null);
  const [prefs, setPrefs] = useState(updatePrefs);
  void gen;
  if (!ready()) {
    return available()
      ? h(EmptyState, { icon: h(StoreGlyph), title: t('store.debian.noIndexTitle'), hint: t('store.debian.noIndex'), style: { minHeight: 380 },
        action: h(Button, { primary: true, label: t('store.debian.update'), onClick: () => !job && runJob('pkg-update', '', t('store.job.update')) }) })
      : h(EmptyState, { mascot: 'confused', title: t('store.debian.unavailable'), style: { minHeight: 360 } });
  }
  const ups = lord.pkg.upgrades();
  const own = lordosUpdate(lang2());
  const others = ups.filter((u) => u.name !== 'lordos');
  const need = !job ? restartNeeded() : null;
  const auto = (on) => { setUpdatePrefs({ autoCheck: on }); setPrefs(updatePrefs()); };
  return h(View, { style: { gap: 12 } },
    h(View, { style: { flexDirection: 'row', alignItems: 'center', gap: 8 } },
      h(View, { style: { flex: 1 } }),
      h(Button, { label: t('store.debian.update'), disabled: !!job, onClick: () => !job && runJob('pkg-update', '', t('store.job.update')) }),
      ups.length ? h(Button, { primary: !own?.available, label: t('store.updates.all'), disabled: !!job,
        onClick: () => !job && setPlan(lord.pkg.plan({ upgrade: true })) }) : null),
    h(Progress),
    need ? h(RestartCard, { need }) : null,
    plan && h(View, { style: boxStyle(th) },
      h(PlanView, { plan }),
      h(View, { style: { flexDirection: 'row', gap: 8, justifyContent: 'flex-end', marginTop: 4 } },
        h(Button, { label: t('store.cancel'), onClick: () => setPlan(null) }),
        plan.ok && h(Button, { primary: true, label: t('store.plan.confirm'), onClick: () => {
          const extra = plan.items.some((i) => i.action === 'remove');
          runJob('pkg-upgrade', extra ? '--allow-remove' : '', t('store.job.upgrade')); setPlan(null);
        } }))),
    own?.available ? h(LordOSCard, { up: own }) : null,
    own && !own.available && !ups.length ? h(Text, { style: { fontSize: typeScale.callout, color: th.textDim, textAlign: 'start' } },
      t('store.system.current', { version: own.installed })) : null,
    ups.length === 0 ? h(EmptyState, { mascot: 'celebrate', title: t('store.updates.none'), style: { minHeight: 260 } }) : null,
    others.length ? h(View, { style: { marginTop: spacing.s } },
      h(Header, { th }, t('store.updates.debian', { count: others.length })),
      h(GroupBox, null, ...others.map((u) => h(View, { key: u.name, style: { flexDirection: 'row', alignItems: 'center', gap: 12, paddingVertical: 8 } },
        h(PkgIcon, { name: u.name, size: 32 }),
        h(View, { style: { flex: 1, minWidth: 0, gap: 2 } },
          h(Text, { style: { fontSize: 13, fontWeight: 600, color: th.text, textAlign: 'start' } }, pkgLabel(u.name)),
          h(Text, { style: { fontSize: 11, color: th.textDim, whiteSpace: 'nowrap', overflow: 'hidden', textAlign: 'start' } }, pkgLine(u))),
        h(Text, { style: { fontSize: 11, color: th.textDim } }, formatBytes(u.size)))))) : null,
    h(View, { style: { flexDirection: 'row', alignItems: 'center', gap: spacing.m, marginTop: spacing.m, paddingHorizontal: spacing.xs } },
      h(View, { style: { flex: 1, minWidth: 0, gap: 2 } },
        h(Text, { style: { fontSize: typeScale.body, color: th.text, textAlign: 'start' } }, t('store.updates.auto')),
        h(Text, { style: { fontSize: typeScale.caption, color: th.textDim, lineHeight: 1.4, textAlign: 'start' } }, t('store.updates.autoHint'))),
      h(Toggle, { on: prefs.autoCheck, onChange: auto })));
}

// Sidebar footer: when the list was last checked.
function Status() {
  const th = useTheme();
  useStore(state);
  if (!ready()) return null;
  const at = lord.pkg.lastUpdate();
  return h(Text, { style: { fontSize: 11, color: th.textDim, lineHeight: 1.35 } },
    at ? t('store.debian.lastUpdate', { date: formatDate(new Date(at * 1000), 'short') }) : '');
}

// Debian package that brings an installed Linux app (for "Remove" in Installed).
function packageOfApp(a) {
  if (!ready()) return null;
  const want = new Set([a.desktopId, `${a.desktopId}.desktop`]);
  return lord.pkg.installed({}).find((p) => p.app && want.has(p.app.id))?.name || null;
}

export const debian = {
  available: ready,
  categoryCounts,
  Details,
  Discover,
  Category,
  SuggestedPage,
  Updates,
  Status,
  installFile: (path) => runJob('pkg-install-file', path, t('store.job.installFile')).then((ok) => { if (!ok) throw new Error(state.get().msg?.text || ''); }),
  removeApp(a) {
    const name = packageOfApp(a);
    if (name) runJob('pkg-remove', name, t('store.job.remove', { name: a.name }));
  },
  canRemove: (a) => !!packageOfApp(a),
  Progress,
};
