// ai.js — the Store's Artificial Intelligence category (0.8.5): LordOS's curated list of AI apps
// (apt/catalog.json, bundled at build time). An app whose maker has a signed APT repository
// ("delivery": "direct") comes from that repository: the first install adds it, with the maker's key
// whose fingerprint the catalog pins (pkg-source-add, server/src/pkg/sources_admin.c), over HTTPS. The
// others come from LordOS's own signed repository (lordos.org/apt), built or copied by LordOS from
// verified sources (docs/UPDATES.md, "Mirrored apps (catalog)"). Installing goes through the package
// engine like any Debian package: the plan first, then pkg-install.
import { createElement as h, useState } from 'react';
import { View, Text, Image, Button, Toggle, LetterIcon, useTheme, useStore, useSystemInfo, admin, locale, formatBytes, typeScale, spacing, t } from '@lordos/sdk';
import catalog from '../../apt/catalog.json';
import { debian } from './debian.js';
import { Card, CardGrid, Featured, FeaturedSkeleton, Pill, Shelf, nameColor } from './ui.js';
import { AdminRescue, isNotAdmin } from './rescue.js';
import { catalogFields, search } from './search.js';

const lang2 = () => (locale.get().lang || 'en').split('-')[0];
// the kernel's machine name as Debian names the architecture
const DEB_ARCH = { x86_64: 'amd64', aarch64: 'arm64' };
const summary = (a) => (a.summary && (a.summary[lang2()] || a.summary.en)) || '';
const installed = (pkg) => { try { return !!(lord.pkg.info(pkg, {}) || {}).installed; } catch (e) { return false; } };
const offered = (pkg) => { try { return !!lord.pkg.info(pkg, {}); } catch (e) { return false; } };

// The icon of a catalog app: the package's own icon once it is in the lists, else its initial.
function AiIcon({ a, size }) {
  let icon = null;
  try { icon = offered(a.package) ? lord.pkg.icon(a.package) : null; } catch (e) { icon = null; }
  return icon ? h(Image, { src: icon, style: { width: size, height: size, objectFit: 'contain' } }) : h(LetterIcon, { size, name: a.name.replace(/\.[a-z]+$/i, '') });
}

const kindOf = (a) => t(`store.ai.kind.${a.kind}`);
const sizeOf = (a) => { try { const i = lord.pkg.info(a.package, {}); return i && i.installedSize ? formatBytes(i.installedSize) : ''; } catch (e) { return ''; } };
const fitsHere = (a, here) => !here || !a.arches || a.arches.includes(here);

function AiCard({ a, here, onOpen }) {
  const fits = fitsHere(a, here);
  const inst = installed(a.package);
  return h(Card, { icon: h(AiIcon, { a, size: 56 }), name: a.name, kind: `${kindOf(a)} · ${a.vendor || ''}`, summary: summary(a), dim: !fits,
    note: !fits ? t('store.ai.notForArch', { arch: here }) : sizeOf(a),
    action: fits ? h(Pill, { label: inst ? t('store.ai.view') : t('store.get'), onClick: () => onOpen(a.package) }) : null,
    onClick: () => fits && onOpen(a.package) });
}

// An app from its maker's repository that isn't in the package lists yet: say whose repository is
// added, then add it and fetch the lists (both admin actions), and show the usual details.
function AddSource({ a, onDone }) {
  const th = useTheme();
  const [busy, setBusy] = useState(false);
  const [err, setErr] = useState('');
  const [refused, setRefused] = useState(false);
  const add = () => {
    setBusy(true);
    setErr('');
    setRefused(false);
    admin.run('pkg-source-add', a.id).then(() => admin.run('pkg-update')).then(onDone, (e) => { setErr(e.message); setRefused(isNotAdmin(e)); }).finally(() => setBusy(false));
  };
  return h(View, { style: { gap: spacing.l, maxWidth: 640 } },
    h(Text, { style: { fontSize: typeScale.largeTitle, fontWeight: 600, color: th.text, textAlign: 'start' } }, a.name),
    h(Text, { style: { fontSize: typeScale.body, color: th.text, lineHeight: 1.45, textAlign: 'start' } },
      t('store.ai.sourceIntro', { name: a.name, vendor: a.vendor || '', url: (a.source && a.source.url) || '' })),
    h(Text, { style: { fontSize: 11, color: th.textDim, fontFamily: 'monospace', direction: 'ltr', textAlign: 'start' } },
      `${(a.source && a.source.url) || ''}\n${(a.source && a.source.fingerprint) || ''}`),
    err ? h(Text, { style: { fontSize: typeScale.caption, color: '#ff453a', textAlign: 'start' } }, err) : null,
    refused ? h(AdminRescue) : null,
    h(View, { style: { flexDirection: 'row', gap: spacing.s } },
      h(Button, { primary: true, label: busy ? t('store.ai.adding') : t('store.ai.addSource'), disabled: busy || !admin.available(), onClick: add })));
}

// A LordOS-built app that isn't in the package lists yet (they were fetched before the catalog had it):
// fetching them again (pkg-update) brings it, instead of the package page's "not found".
function NotListed({ a, onDone }) {
  const th = useTheme();
  const [busy, setBusy] = useState(false);
  const [err, setErr] = useState('');
  const [refused, setRefused] = useState(false);
  const update = () => {
    setBusy(true);
    setErr('');
    setRefused(false);
    admin.run('pkg-update').then(onDone, (e) => { setErr(e.message); setRefused(isNotAdmin(e)); }).finally(() => setBusy(false));
  };
  return h(View, { style: { gap: spacing.l, maxWidth: 640 } },
    h(Text, { style: { fontSize: typeScale.largeTitle, fontWeight: 600, color: th.text, textAlign: 'start' } }, a.name),
    h(Text, { style: { fontSize: typeScale.body, color: th.text, lineHeight: 1.45, textAlign: 'start' } }, t('store.ai.notListed', { name: a.name })),
    err ? h(Text, { style: { fontSize: typeScale.caption, color: '#ff453a', textAlign: 'start' } }, err) : null,
    refused ? h(AdminRescue) : null,
    h(View, { style: { flexDirection: 'row', gap: spacing.s } },
      h(Button, { primary: true, label: busy ? t('store.ai.updating') : t('store.ai.updateLists'), disabled: busy || !admin.available(), onClick: update })));
}

// An item of the Commercial group made of several parts: what is in it, each with its license and source, before anything
// is installed. Nothing here installs: the person goes on to the usual package page.
function Authorize({ a, onAllow }) {
  const th = useTheme();
  const here = useHere();
  const [more, setMore] = useState(false);
  const drivers = a.drivers.filter((d) => !d.arches || d.arches.includes(here));
  const extras = more ? drivers.flatMap((d) => d.optional || []) : [];
  return h(View, { style: { gap: spacing.m, maxWidth: 680 } },
    h(Text, { style: { fontSize: typeScale.largeTitle, fontWeight: 600, color: th.text, textAlign: 'start' } }, a.name),
    h(Text, { style: { fontSize: typeScale.caption, fontWeight: 600, color: th.textDim, textAlign: 'start' } }, t('store.formats.label')),
    h(Text, { style: { fontSize: typeScale.body, color: th.text, lineHeight: 1.45, textAlign: 'start' } }, t('store.formats.intro')),
    ...drivers.map((d) => h(View, { key: d.name, style: { gap: 2 } },
      h(Text, { style: { fontSize: typeScale.body, fontWeight: 600, color: th.text, textAlign: 'start' } }, `${d.name} · ${d.license}`),
      h(Text, { style: { fontSize: typeScale.callout, color: th.textDim, textAlign: 'start' } }, t(`store.formats.${d.id}`)),
      h(Text, { style: { fontSize: 11, color: th.textDim, fontFamily: 'monospace', direction: 'ltr', textAlign: 'start' } }, d.source),
      d.optional ? h(View, { style: { flexDirection: 'row', alignItems: 'center', gap: spacing.s, marginTop: 4 } },
        h(Toggle, { value: more, onChange: setMore }),
        h(Text, { style: { flex: 1, fontSize: typeScale.callout, color: th.text, textAlign: 'start' } }, t('store.formats.optional'))) : null)),
    h(View, { style: { flexDirection: 'row', gap: spacing.s } }, h(Button, { primary: true, label: t('store.formats.allow'), onClick: () => onAllow(extras) })));
}

// The item is in and an optional part isn't: the same choice as on the page before installing, with its own plan.
function MoreDrivers({ a }) {
  const th = useTheme();
  const here = useHere();
  const { job } = useStore(debian.state);
  const [plan, setPlan] = useState(null);
  const missing = a.drivers.filter((d) => !d.arches || d.arches.includes(here)).flatMap((d) => d.optional || []).filter((p) => !installed(p));
  if (!missing.length) return null;
  const confirm = () => {
    const extra = plan.items.some((i) => i.action === 'remove' && !i.requested);
    debian.runJob('pkg-install', [...missing, ...(extra ? ['--allow-remove'] : [])].join(' '), t('store.job.install', { name: a.name }));
    setPlan(null);
  };
  return h(View, { style: { gap: spacing.s, maxWidth: 680 } },
    h(View, { style: { flexDirection: 'row', alignItems: 'center', gap: spacing.m } },
      h(Text, { style: { flex: 1, fontSize: typeScale.callout, color: th.text, textAlign: 'start' } }, t('store.formats.optional')),
      h(Button, { label: t('store.install'), disabled: !admin.available() || !!job, onClick: () => setPlan(lord.pkg.plan({ install: missing })) })),
    plan ? h(debian.PlanView, { plan }) : null,
    plan ? h(View, { style: { flexDirection: 'row', gap: spacing.s, justifyContent: 'flex-end' } },
      h(Button, { label: t('store.cancel'), onClick: () => setPlan(null) }),
      plan.ok ? h(Button, { primary: true, label: t('store.plan.confirm'), onClick: confirm }) : null) : null);
}

const useHere = () => { const info = useSystemInfo(60000); return DEB_ARCH[info.arch] || info.arch || ''; };
// The catalog's sections: "ai" (entries without a section), "commercial" (free of charge, no
// source code: the page says so before anything is installed), "remote" (third-party remote access:
// Tailscale's private network; its system service is outside the per-app firewall) and
// "suites" (featured open source suites from other projects, such as ArtCraft).
const sectionOf = (a) => a.section || 'ai';
export const catalogApps = (section) => (catalog.apps || []).filter((a) => !section || sectionOf(a) === section);
export const catalogApp = (pkg) => (catalog.apps || []).find((x) => x.package === pkg);

// A catalog app's page: add its maker's repository first, fetch the lists when LordOS's own package
// isn't in them yet, else the usual package page.
export function AiDetail({ pkg }) {
  useStore(locale);
  const [, setTick] = useState(0);
  const [allowed, setAllowed] = useState(null);        // the optional packages chosen, once allowed
  const a = catalogApp(pkg);
  const direct = a && a.source && a.source.delivery === 'direct';
  if (a?.drivers && !installed(pkg) && !allowed) return h(Authorize, { a, onAllow: setAllowed });
  if (direct && !offered(pkg)) return h(AddSource, { a, onDone: () => setTick((x) => x + 1) });
  if (a && !direct && !offered(pkg)) return h(NotListed, { a, onDone: () => setTick((x) => x + 1) });
  // removing an app from its maker's repository also removes that repository, unless another
  // installed catalog app uses the same one (Claude Code and Claude Desktop share a key, not a repo)
  const onRemoved = () => {
    if (!direct) return;
    const sameRepo = (catalog.apps || []).some((x) => x.id !== a.id && x.source && x.source.url === a.source.url && installed(x.package));
    if (!sameRepo) admin.run('pkg-source-remove', a.id).catch(() => {});
  };
  const details = h(debian.Details, { name: pkg, onRemoved, extras: allowed || [] });
  return a?.drivers && installed(pkg) ? h(View, { style: { gap: spacing.l } }, h(MoreDrivers, { a }), details) : details;
}

// The Artificial Intelligence page: the catalog as cards (the search narrows it).
// A catalog section as cards (the search narrows it): Artificial Intelligence, or Commercial.
export function AiPage({ query, onOpen, onPage, section = 'ai' }) {
  const th = useTheme();
  useStore(locale);
  const here = useHere();
  const apps = searchCatalog(catalogApps(section), query);
  const missing = apps.some((a) => !offered(a.package) && !(a.source && a.source.delivery === 'direct'));
  return h(View, { style: { gap: spacing.l } },
    h(Text, { style: { fontSize: typeScale.body, color: th.textDim, lineHeight: 1.45, maxWidth: 680, textAlign: 'start' } }, t(section === 'commercial' ? 'store.commercial.intro' : section === 'remote' ? 'store.remote.intro' : section === 'suites' ? 'store.suites.intro' : 'store.ai.intro')),
    missing ? h(Text, { style: { fontSize: typeScale.caption, color: '#ff9f0a', textAlign: 'start' } }, t('store.ai.refresh')) : null,
    h(debian.Progress),
    h(CardGrid, null, ...apps.map((a) => h(AiCard, { key: a.id, a, here, onOpen }))),
    // the graphics drivers stay where the hardware is detected (Store → For this computer)
    section === 'commercial' ? h(View, { style: { flexDirection: 'row', alignItems: 'center', gap: spacing.m, marginTop: spacing.s } },
      h(Text, { style: { flex: 1, fontSize: typeScale.callout, color: th.textDim, lineHeight: 1.4, textAlign: 'start' } }, t('store.commercial.drivers')),
      h(Button, { small: true, label: t('store.commercial.openDrivers'), onClick: () => onPage && onPage('suggested') })) : null);
}

// The catalog apps that match a search, best first (search.js: any word, joined or apart, case and
// accents ignored; names, ids, package names and keywords count more than summaries).
export const searchCatalog = (apps, query) => search(apps, query || '', (a) => catalogFields(a, lang2()));

// Discover's search: the catalog apps that match, from every section, above the package results
// (they aren't in the package lists until their maker's repository is added).
export function CatalogResults({ query, onOpen }) {
  useStore(locale);
  const here = useHere();
  const apps = searchCatalog(catalogApps(), query);
  if (!apps.length) return null;
  return h(Shelf, { title: t('store.search.catalog') },
    h(CardGrid, null, ...apps.slice(0, 8).map((a) => h(AiCard, { key: a.id, a, here, onOpen }))));
}

// The banner LordOS publishes in its repository (apt/featured/featured.json and banner.png, signed with
// the indexes and verified by the package engine; lord.pkg.featured()), or null.
function published() {
  try { return typeof lord.pkg.featured === 'function' ? lord.pkg.featured() : null; } catch (e) { return null; }
}
const loc = (v) => (v && typeof v === 'object' ? v[lang2()] || v.en || '' : typeof v === 'string' ? v : '');

// For Discover: the banner, always (the published one; without package lists yet its skeleton;
// without a published one, a catalog app not installed here, a different one each day), then the AI
// shelf.
export function AiDiscover({ onOpen, onOpenDeb, onMore }) {
  useStore(locale);
  const here = useHere();
  const apps = catalogApps('ai').filter((a) => fitsHere(a, here));
  const fresh = apps.filter((a) => !installed(a.package));
  const pool = fresh.length ? fresh : apps;
  let ready = false;
  try { ready = lord.pkg.available(); } catch (e) { ready = false; }
  const pub = ready ? published() : null;
  let banner;
  if (pub && pub.data) {
    const d = pub.data;
    const a = d.catalog ? (catalog.apps || []).find((x) => x.id === d.catalog) : null;
    const pkg = a ? a.package : typeof d.package === 'string' ? d.package : null;
    const open = () => pkg && (a ? onOpen(pkg) : onOpenDeb(pkg));
    const colors = Array.isArray(d.colors) && d.colors.every((c) => /^#[0-9a-f]{6}$/i.test(c)) ? d.colors : null;
    banner = pub.image || !ready
      ? h(Featured, { image: pub.image, colors, kicker: loc(d.kicker), title: loc(d.title) || (a && a.name) || pkg || '', text: loc(d.text) || (a ? summary(a) : ''),
        action: pkg ? h(Pill, { big: true, onDark: true, label: installed(pkg) ? t('store.ai.view') : t('store.get'), onClick: open }) : null, onClick: open })
      : h(FeaturedSkeleton);
  } else if (!ready) {
    banner = h(FeaturedSkeleton);
  } else if (pool.length) {
    const f = pool[Math.floor(Date.now() / 86400000) % pool.length];
    banner = h(Featured, { icon: h(AiIcon, { a: f, size: 96 }), glow: nameColor(f.name.replace(/\.[a-z]+$/i, '')),
      kicker: t('store.featured', { category: t('store.page.ai') }), title: f.name, text: summary(f),
      action: h(Pill, { big: true, onDark: true, label: installed(f.package) ? t('store.ai.view') : t('store.get'), onClick: () => onOpen(f.package) }),
      onClick: () => onOpen(f.package) });
  }
  return h(View, { style: { gap: 26 } },
    banner || null,
    apps.length ? h(Shelf, { title: t('store.page.ai'), more: t('store.seeAll', { count: apps.length }), onMore },
      h(CardGrid, null, ...apps.slice(0, 4).map((a) => h(AiCard, { key: a.id, a, here, onOpen })))) : null);
}
