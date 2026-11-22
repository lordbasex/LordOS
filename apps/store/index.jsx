// Store: LordOS apps (.lpk packages, no root) and Debian apps (our own package engine,
// server/src/pkg; install and remove go through the administration helper).
import {
  View, Text, Image, useTheme, useLocale, useAppWindow, useStore, Button, ScrollView, SidebarItem, SectionTitle, SearchField, Hairline, EmptyState, GroupBox, Toolbar, ToolbarButton,
  packages, packageList, refreshPackages, useLauncherApps, launchApp, fs, formatBytes, wm, direction, typeScale, spacing, isSystemApp,
  Segmented, admin, canDeactivate, deactivateApp, activateApp, webAppEntry, settings, addListener,
  suggestions, unseenSuggestions, refreshSuggestions, useSidebar, SidebarToggle, SidebarOverlay,
} from '@lordos/sdk';
import { useEffect, useMemo, useRef, useState } from 'react';
import { AiDetail, AiDiscover, AiPage, CatalogCategory, CatalogResults, catalogApp, catalogApps, catalogCategoryCounts, searchCatalog } from './ai.js';
import { matches } from './search.js';
import { CATS, debian, useDebian } from './debian.js';
import { GLYPH } from './ui.js';
import { EndsContext, MoreButton, makeEnds, usePaged } from './endless.js';
import { WebAppsPage, WebCategory, WebDiscover, WebResults, webCatalog, webHits, webCategoryCounts } from './webapps.js';
import { WebAppDetail } from './webdetail.js';
import { FlathubDetail, FlathubPage, FlathubResults, FlathubUpdates, removeInstalled, useFlathub, useFlathubCounts } from './flathubpages.js';
import { flathub } from '@lordos/sdk';

// The user's Downloads folder relative to home ("Downloads/"), from xdg-user-dirs; without it, the
// first of the usual names that exists, or the home folder itself.
function downloadsDir() {
  let conf = '';
  try { conf = fs.read('.config/user-dirs.dirs'); } catch (e) { /* no xdg-user-dirs */ }
  const m = /^XDG_DOWNLOAD_DIR="\$HOME\/([^"]+)"/m.exec(conf);
  if (m) return `${m[1]}/`;
  for (const d of ['Downloads', 'Descargas', 'Transferências']) {
    try { fs.list(d); return `${d}/`; } catch (e) { /* not there */ }
  }
  return '';
}

// Trust level of a package from its signature (see docs/SIGNING.md).
const TRUST = {
  lordos: { color: '#30d158', glyph: '◆' },
  verified: { color: '#0a84ff', glyph: '✓' },
  signed: { color: '#8e8e93', glyph: '✎' },
  unsigned: { color: '#ff9f0a', glyph: '⚠' },
};
function TrustBadge({ sig }) {
  const { t: tr } = useLocale();
  const level = sig?.trust || 'unsigned';
  const c = TRUST[level] || TRUST.unsigned;
  const label = level === 'lordos' ? tr('store.trust.lordos')
    : level === 'verified' ? tr('store.trust.verified', { name: sig.signer })
    : level === 'signed' ? tr('store.trust.signed', { name: sig.signer })
    : tr('store.trust.unsigned');
  return (
    <View style={{ flexDirection: 'row', alignItems: 'center', gap: 6 }}>
      <View style={{ width: 16, height: 16, borderRadius: 8, backgroundColor: c.color, alignItems: 'center', justifyContent: 'center' }}>
        <Text style={{ fontSize: 10, color: '#fff', lineHeight: 16 }}>{c.glyph}</Text>
      </View>
      <Text style={{ fontSize: 11, color: c.color }}>{label}</Text>
    </View>
  );
}

export function Icon({ size: s }) {
  return (
    <View style={{ width: s, height: s, borderRadius: s * 0.225, background: 'linear-gradient(180deg, #4fb4ff, #1463d6)',
      boxShadow: `0 ${s * 0.04}px ${s * 0.12}px rgba(0,0,0,0.35)`, alignItems: 'center', justifyContent: 'center' }}>
      <View style={{ width: s * 0.56, height: s * 0.46, marginTop: s * 0.1, borderRadius: s * 0.08, backgroundColor: '#fff' }} />
      <View style={{ position: 'absolute', top: s * 0.2, left: s * 0.34, width: s * 0.32, height: s * 0.26, borderRadius: s * 0.16,
        border: `${Math.max(2, s * 0.06)}px solid #fff` }} />
      <View style={{ position: 'absolute', top: s * 0.5, left: s * 0.36, width: s * 0.28, height: s * 0.07, borderRadius: s * 0.035, backgroundColor: '#1d72e0' }} />
    </View>
  );
}

// A section header (13/600, sentence case) above an inset list, with an optional count.
function ListHeader({ title, count }) {
  const t = useTheme();
  return (
    <View style={{ flexDirection: 'row', alignItems: 'baseline', gap: spacing.s, paddingHorizontal: spacing.xs, marginBottom: 6 }}>
      <Text style={{ fontSize: typeScale.body, fontWeight: 600, color: t.text, textAlign: 'start' }}>{title}</Text>
      {count != null ? <Text style={{ fontSize: typeScale.caption, color: t.textDim }}>{count}</Text> : null}
    </View>
  );
}

// An inset list (GroupBox) with a section header; rows are AppRow.
function List({ title, count, children, empty, style }) {
  const t = useTheme();
  const rows = (Array.isArray(children) ? children.flat() : [children]).filter(Boolean);
  return (
    <View style={[{ marginBottom: spacing.xl }, style]}>
      {title ? <ListHeader title={title} count={count} /> : null}
      <GroupBox>
        {rows.length ? rows : <Text style={{ fontSize: typeScale.body, color: t.textDim, paddingVertical: 10, lineHeight: 1.4, textAlign: 'start' }}>{empty}</Text>}
      </GroupBox>
    </View>
  );
}

// Page title (large title) with an optional subtitle.
export function PageTitle({ title, subtitle, children }) {
  const t = useTheme();
  return (
    <View style={{ flexDirection: 'row', alignItems: 'center', gap: spacing.s, marginBottom: spacing.xl }}>
      <View style={{ flex: 1, minWidth: 0, gap: spacing.xs }}>
        <Text style={{ fontSize: typeScale.largeTitle, fontWeight: 600, color: t.text, textAlign: 'start' }}>{title}</Text>
        {subtitle ? <Text style={{ fontSize: typeScale.callout, color: t.textDim, lineHeight: 1.4, textAlign: 'start' }}>{subtitle}</Text> : null}
      </View>
      {children}
    </View>
  );
}

// An app row: icon, name, detail and actions at the end.
export function AppRow({ icon, name, detail, extra, children, onClick, below }) {
  const t = useTheme();
  return (
    <View onClick={onClick} style={{ paddingVertical: spacing.s + 2, gap: spacing.s }}>
      <View style={{ flexDirection: 'row', alignItems: 'center', gap: spacing.m }}>
        {icon}
        <View style={{ flex: 1, minWidth: 0, gap: 2 }}>
          <Text style={{ fontSize: typeScale.body, fontWeight: 600, color: t.text, whiteSpace: 'nowrap', overflow: 'hidden', textAlign: 'start' }}>{name}</Text>
          {detail ? <Text style={{ fontSize: typeScale.callout, color: t.textDim, whiteSpace: 'nowrap', overflow: 'hidden', textAlign: 'start' }}>{detail}</Text> : null}
          {extra ? <Text style={{ fontSize: typeScale.caption, color: t.textDim, textAlign: 'start' }}>{extra}</Text> : null}
        </View>
        <View style={{ flexDirection: 'row', alignItems: 'center', gap: spacing.s }}>{children}</View>
      </View>
      {below || null}
    </View>
  );
}

export function Message({ msg }) {
  if (!msg) return null;
  return <Text style={{ fontSize: typeScale.callout, color: msg.ok ? '#30d158' : '#ff453a', marginBottom: spacing.m, lineHeight: 1.4, textAlign: 'start' }}>{msg.text}</Text>;
}

// ---- LordOS apps (.lpk) ----

// Packages found in the home folder (Downloads, Desktop and the top level).
function findPackageFiles() {
  const out = [];
  for (const dir of ['', 'Descargas', 'Downloads', 'Escritorio', 'Desktop']) {
    let list = [];
    try { list = fs.list(dir || '.'); } catch (e) { continue; }
    for (const e of list) if (!e.dir && /\.(lpk|deb)$/i.test(e.name)) out.push({ path: `${fs.home()}/${dir ? `${dir}/` : ''}${e.name}`, name: e.name, size: e.size });
  }
  return out;
}

// Confirmation before installing a file: what the package says and what it asks for.
function InstallFile({ path, onDone }) {
  const t = useTheme();
  const { t: tr } = useLocale();
  const isDeb = /\.deb$/i.test(path);
  const [info, setInfo] = useState(null);
  const [msg, setMsg] = useState(null);
  const [busy, setBusy] = useState(false);
  useEffect(() => {
    setMsg(null);
    if (isDeb) { setInfo(null); return; }
    try { setInfo(packages.inspect(path)); } catch (e) { setInfo(null); setMsg({ ok: false, text: tr('store.lordos.invalid', { error: e.message }) }); }
  }, [path]);
  const m = info?.manifest;
  const lang = useLocale().lang;
  const loc = (v) => (v && typeof v === 'object' ? v[lang] || v[lang.split('-')[0]] || v.en || Object.values(v)[0] : v) || '';
  const installed = m && packageList.get().find((p) => p.id === m.id);
  const install = () => {
    if (isDeb) {
      setBusy(true);
      debian.installFile(path).then(() => { setMsg({ ok: true, text: tr('store.done.installed') }); onDone?.(); },
        (e) => setMsg({ ok: false, text: e.message })).finally(() => setBusy(false));
      return;
    }
    try {
      packages.install(path);
      setMsg({ ok: true, text: tr('store.lordos.installed', { name: loc(m.name) }) });
      onDone?.();
    } catch (e) { setMsg({ ok: false, text: e.message }); }
  };
  return (
    <View style={{ padding: spacing.xl, borderRadius: 12, backgroundColor: t.mode === 'dark' ? 'rgba(255,255,255,0.05)' : '#ffffff', border: `1px solid ${t.separator}`,
      boxShadow: '0 6px 20px rgba(0,0,0,0.18)', gap: spacing.m, marginBottom: spacing.xl }}>
      <View style={{ gap: 2 }}>
        <Text style={{ fontSize: typeScale.headline, fontWeight: 600, color: t.text, textAlign: 'start' }}>{tr('store.file.title')}</Text>
        <Text style={{ fontSize: typeScale.caption, color: t.textDim, whiteSpace: 'nowrap', overflow: 'hidden', direction: 'ltr', textAlign: 'start' }}>{path.replace(fs.home(), '~')}</Text>
      </View>
      {m && (
        <View style={{ flexDirection: 'row', gap: spacing.m, alignItems: 'flex-start' }}>
          <Icon size={44} />
          <View style={{ flex: 1, minWidth: 0, gap: spacing.xs }}>
            <Text style={{ fontSize: typeScale.body, fontWeight: 600, color: t.text, textAlign: 'start' }}>{loc(m.name)} {m.version}</Text>
            {m.description ? <Text style={{ fontSize: typeScale.callout, color: t.text, lineHeight: 1.4, textAlign: 'start' }}>{loc(m.description)}</Text> : null}
            <Text style={{ fontSize: typeScale.callout, color: t.textDim, textAlign: 'start' }}>{tr('store.file.size', { size: formatBytes(info.size), files: info.files })}</Text>
            <Text style={{ fontSize: typeScale.callout, color: t.textDim, lineHeight: 1.4, textAlign: 'start' }}>
              {tr('store.file.permissions', { list: (m.permissions || []).length ? m.permissions.join(', ') : tr('store.file.noPermissions') })}</Text>
            <TrustBadge sig={info.signature} />
            {installed && !msg?.ok && <Text style={{ fontSize: typeScale.callout, color: t.textDim, textAlign: 'start' }}>{tr('store.file.replaces', { version: installed.version })}</Text>}
          </View>
        </View>
      )}
      {isDeb && <Text style={{ fontSize: typeScale.callout, color: t.text, lineHeight: 1.4, textAlign: 'start' }}>{tr('store.file.deb')}</Text>}
      <Message msg={msg} />
      {!msg?.ok && <debian.Refused />}
      <View style={{ flexDirection: 'row', gap: spacing.s, justifyContent: 'flex-end' }}>
        <Button label={tr('store.close')} onClick={() => onDone?.(true)} />
        {(m || isDeb) && !msg?.ok && <Button primary busy={busy} label={busy ? tr('store.working') : tr('store.install')} onClick={() => !busy && install()} />}
      </View>
    </View>
  );
}

function LordOSPage({ query }) {
  const t = useTheme();
  const { t: tr } = useLocale();
  const list = useStore(packageList);
  const [files, setFiles] = useState(() => findPackageFiles().filter((f) => /\.lpk$/i.test(f.name)));
  const [pick, setPick] = useState(null);
  const [msg, setMsg] = useState(null);
  const shown = list.filter((p) => matches(query, [p.name, p.id], [p.description]));
  return (
    <View>
      <Text style={{ fontSize: typeScale.body, color: t.textDim, lineHeight: 1.45, maxWidth: 680, marginBottom: spacing.xl, textAlign: 'start' }}>{tr('store.lordos.subtitle')}</Text>
      {pick && <InstallFile path={pick} onDone={(cancel) => { if (cancel) setPick(null); setFiles(findPackageFiles().filter((f) => /\.lpk$/i.test(f.name))); }} />}
      <Message msg={msg} />
      <List title={tr('store.lordos.installedTitle')} count={shown.length || null} empty={tr('store.lordos.none')}>
        {shown.map((p) => (
          <AppRow key={p.id} icon={<p.Icon size={40} />} name={p.name} detail={p.description} extra={tr('store.version', { version: p.version })}
            below={<PkgManage pkg={p} onMsg={setMsg} />}>
            {!p.packaged && <Button small label={tr('store.remove')} onClick={() => { packages.remove(p.id); setMsg({ ok: true, text: tr('store.lordos.removed', { name: p.name }) }); }} />}
            <Button small label={tr('store.open')} onClick={() => launchApp(p.id)} />
          </AppRow>
        ))}
      </List>
      <List title={tr('store.lordos.filesTitle')} count={files.length || null} empty={tr('store.lordos.noFiles')}>
        {files.map((f) => (
          <AppRow key={f.path} icon={<Icon size={32} />} name={f.name} detail={`${f.path.replace(fs.home(), '~')} · ${formatBytes(f.size)}`}>
            <Button small label={tr('store.install')} onClick={() => setPick(f.path)} />
          </AppRow>
        ))}
      </List>
    </View>
  );
}

// Trust, "Modified by you", Restore original and Share my version (docs/SIGNING.md).
function PkgManage({ pkg, onMsg }) {
  const t = useTheme();
  const { t: tr } = useLocale();
  const [st, setSt] = useState(null);
  const [rev, setRev] = useState(0);
  useEffect(() => { try { setSt(lord.sign.status(pkg.id)); } catch (e) { setSt(null); } }, [pkg.id, pkg.version, rev]);
  if (!st || !st.installed) return null;
  const restore = () => {
    try { lord.sign.restore(pkg.id); refreshPackages(); setRev((v) => v + 1); onMsg({ ok: true, text: tr('store.restore.done', { name: pkg.name }) }); }
    catch (e) { onMsg({ ok: false, text: e.message }); }
  };
  const share = () => {
    try {
      const key = lord.sign.userKey();
      const base = `${pkg.id}@${st.version} by ${st.signer || tr('store.trust.unknownAuthor')}`;
      const manifest = { ...pkgManifest(pkg), id: pkg.id, version: `${st.version}+mine`, basedOn: base };
      const out = lord.sign.share(pkg.id, JSON.stringify(manifest, null, 2), `${downloadsDir()}${pkg.id}-mine.lpk`);
      onMsg({ ok: true, text: tr('store.share.done', { path: out.replace(fs.home(), '~'), key: key.public.slice(0, 16) }) });
    } catch (e) { onMsg({ ok: false, text: e.message }); }
  };
  return (
    <View style={{ [direction() === 'rtl' ? 'marginRight' : 'marginLeft']: 52, gap: 6 }}>
      {st.hasOriginal ? <TrustBadge sig={st} /> : <Text style={{ fontSize: 11, color: t.textDim }}>{tr('store.manage.noOriginal')}</Text>}
      {st.modified && (
        <View style={{ gap: 4 }}>
          <Text style={{ fontSize: 12, color: '#ff9f0a', fontWeight: 600 }}>{tr('store.manage.modified', { count: st.changedCount })}</Text>
          {st.changed.slice(0, 6).map((c) => (
            <Text key={c.path} style={{ fontSize: 11, color: t.textDim }}>
              {tr(`store.manage.kind.${c.kind}`)} {c.path}
            </Text>
          ))}
          <View style={{ flexDirection: 'row', gap: spacing.s, marginTop: spacing.xs }}>
            {st.hasOriginal && <Button small label={tr('store.manage.restore')} onClick={restore} />}
            <Button small label={tr('store.manage.share')} onClick={share} />
          </View>
        </View>
      )}
    </View>
  );
}
const pkgManifest = (pkg) => ({ name: pkg.name, description: pkg.description, icon: 'icon.png', size: pkg.size, minSize: pkg.minSize, permissions: pkg.permissions });

// ---- installed apps (all of them: LordOS, packages and Linux) ----

// Every installed app. A LordOS app LordOS can do without is deactivated here, and activated again
// (sdk/appsoffactions.js; the core keeps "System app — part of LordOS"); a Linux app a Debian package
// brought is removed after its plan (debian.RemoveSheet). "Deactivated" lists the apps turned off.
function InstalledPage({ query, filter: initial }) {
  const t = useTheme();
  const { t: tr } = useLocale();
  const apps = useLauncherApps({ all: true });
  const [filter, setFilter] = useState(initial === 'off' ? 'off' : 'all');
  const [removing, setRemoving] = useState(null);
  const [msg, setMsg] = useState(null);
  useEffect(() => { if (initial === 'off') setFilter('off'); }, [initial]);
  useStore(debian.state);
  const fhJob = useStore(flathub.state, (st) => st.job);
  const offCount = apps.filter((a) => a.off).length;
  const shown = apps.filter((a) => (filter === 'off' ? a.off : true) && matches(query, [a.name, a.id, a.genericName], [a.comment]));
  // hundreds of apps with Debian's and Flathub's: a page of rows at a time as the list is scrolled (endless.js)
  const { limit, more } = usePaged(shown.length, `${filter}|${query}`);
  // the kind, and a LordOS app's or package's own version (manifest "version")
  const kind = (a) => (a.version && a.kind !== 'linux' ? tr('store.kindVersion', { kind: tr(`store.kind.${a.kind}`), v: a.version }) : tr(`store.kind.${a.kind}`));
  const builtin = (a) => a.kind === 'lordos' && isSystemApp(a.id);
  const deactivate = (a) => {
    const r = deactivateApp(a.id);
    setMsg(r.ok ? { ok: true, text: tr('store.off.done', { name: a.name }) } : { ok: false, text: tr(`store.off.refused.${r.reason}`, { name: a.name }) });
  };
  const activate = (a) => { activateApp(a.id); setMsg({ ok: true, text: tr('store.off.activated', { name: a.name }) }); };
  return (
    <View>
      <View style={{ flexDirection: 'row', alignItems: 'center', gap: spacing.s, marginBottom: spacing.l }}>
        <Segmented small appearance="neutral" value={filter} onChange={setFilter}
          options={[['all', tr('store.off.filterAll')], ['off', offCount ? tr('store.off.filterOffCount', { count: offCount }) : tr('store.off.filterOff')]]} />
      </View>
      <debian.Progress />
      <Message msg={msg} />
      <List count={null} empty={filter === 'off' ? tr('store.off.none') : tr('store.debian.none')}>
        {shown.slice(0, limit).map((a) => (
          <AppRow key={a.id} icon={<View style={{ opacity: a.off ? 0.45 : 1 }}><a.Icon size={32} /></View>} name={a.name} detail={a.comment || a.genericName || ''}
            extra={a.off ? `${kind(a)} · ${tr('store.off.badge')}` : kind(a)}
            below={removing === a.id ? <debian.RemoveSheet app={a} onClose={() => setRemoving(null)} /> : null}>
            {/* a system app is part of LordOS: never removable here (docs/SIGNING.md, "Verified system code"); the ones
                LordOS can do without are deactivated instead (sdk/appsoffmodel.js) */}
            {builtin(a) && !canDeactivate(a.id) && <Text style={{ fontSize: typeScale.caption, color: t.textDim, whiteSpace: 'nowrap' }}>{tr('store.systemApp')}</Text>}
            {builtin(a) && canDeactivate(a.id) && (a.off
              ? <Button small primary label={tr('store.off.activate')} onClick={() => activate(a)} />
              : <Button small label={tr('store.off.deactivate')} onClick={() => deactivate(a)} />)}
            {a.kind === 'linux' && debian.canRemove(a) && <Button small label={tr('store.remove')} disabled={!admin.available()}
              onClick={() => setRemoving(removing === a.id ? null : a.id)} />}
            {a.kind === 'flatpak' && flathub.available() && <Button small label={tr('store.remove')} disabled={!!fhJob}
              onClick={() => !fhJob && removeInstalled(a).then((ok) => setMsg(ok ? { ok: true, text: tr('flathub.msg.uninstallDone', { name: a.name }) } : flathub.state.get().msg))} />}
            {!a.off && <Button small label={tr('store.open')} onClick={() => launchApp(a.id)} />}
          </AppRow>
        ))}
      </List>
      <MoreButton left={shown.length - limit} onMore={more} />
    </View>
  );
}

// ---- window ----

// Where the window is: a page ('discover', 'installed', 'updates', 'ai', 'lordos', …) or an origin ('src:debian', 'src:flathub',
// 'src:web') with `sub`, its category in the second column (null: all of it)
// and, over it, an app page (`app`: a Debian package; `ai`: true for a catalog app; `web`: true for a web app, its id).
// Back returns to the page.
// a small window (a Lord Workspaces tile): below SIDE_AT the pages and categories hide behind a toolbar button
const SIDE_AT = 720;
const SIDE_W = 230, SIDE_MIN = 190, SIDE_MAX = 380;
const PAGE_GLYPH = { flathub: GLYPH.flathub, suites: GLYPH.suites, remote: GLYPH.remote, commercial: GLYPH.commercial, suggested: GLYPH.suggested, discover: GLYPH.discover, installed: GLYPH.installed, updates: GLYPH.updates, ai: GLYPH.ai, webapps: GLYPH.webapps, lordos: GLYPH.lordos };

export default function Store() {
  const t = useTheme();
  const { t: tr } = useLocale();
  const { args } = useAppWindow();
  const [page, setPage] = useState('discover');
  const [sub, setSub] = useState(null);         // an origin page's category (its second column), or null for all
  const [app, setApp] = useState(null);
  const [query, setQuery] = useState('');
  const [file, setFile] = useState(null);
  const deb = useDebian();
  const fh = useFlathub();
  const launcher = useLauncherApps();
  // the pages of before (Settings, Search and links open them by name) are now an origin's pages; a category of
  // Discover's shelves is the Linux apps' one
  const go = (p, c = null) => {
    setApp(null);
    if (p === 'flathub') p = 'src:flathub';
    else if (p === 'webapps') p = 'src:web';
    else if (p.startsWith('cat:')) { c = p.slice(4); p = 'src:debian'; }
    setPage(p); setSub(c);
  };
  const side = useSidebar(SIDE_AT);
  // the sidebar's width: dragged at its edge between SIDE_MIN and SIDE_MAX, kept in settings (2026-11-20: long names
  // such as "Linux apps (Debian)" were cut)
  const savedW = useStore(settings, (st) => st.storeSideWidth);
  const [dragW, setDragW] = useState(null);
  const clampSide = (w) => Math.round(Math.max(SIDE_MIN, Math.min(SIDE_MAX, Number(w) || SIDE_W)));
  const sideW = clampSide(dragW ?? savedW ?? SIDE_W);
  const startSide = (e) => {
    const x0 = e.x, w0 = sideW, rtl = direction() === 'rtl';
    const at = (m) => clampSide(w0 + (rtl ? x0 - m.x : m.x - x0));
    const off = [addListener('pointermove', (m) => setDragW(at(m))),
      addListener('pointerup', (m) => { off.forEach((f) => f()); setDragW(null); settings.set((st) => ({ ...st, storeSideWidth: at(m) })); })];
  };
  // Each page, search and app opens at the top (#20); back from an app returns to where the list was.
  const scroll = useRef(null);
  // the long lists load their next page near the end of the scroll (endless.js)
  const ends = useMemo(() => makeEnds(scroll), []);
  const listAt = useRef(null);
  const listKey = `${page}|${sub}|${query}`;
  useEffect(() => { ends.reset(); }, [listKey, app]);
  useEffect(() => { ends.limits.clear(); }, [listKey]);      // a list is as long as it was only back from an app's page
  useEffect(() => {
    const at = listAt.current;
    if (app || !at) return;
    listAt.current = null;
    if (at.key === listKey) scroll.current?.scrollTo(at.y);
  }, [app]);
  useEffect(() => {
    if (args?.install) setFile(args.install);
    if (args?.page) go(args.page);
    if (args?.app) openAi(args.app);
    if (args?.webapp) openWeb(args.webapp);
    if (args?.search) { setQuery(args.search); go('discover'); }       // from Search
  }, [args?.nonce]);
  const sug = useStore(suggestions);
  useEffect(() => { refreshSuggestions(); }, []);                  // the hardware may have changed since the last look
  const unseen = page === 'suggested' ? 0 : unseenSuggestions(sug.list).length;    // new suggestions (the page's badge)
  // the Debian apps of each category, plus the catalog apps that declare it (Steam in Games)
  const counts = (() => {
    const c = { ...debian.categoryCounts() };
    for (const [k, n] of Object.entries(catalogCategoryCounts())) c[k] = (c[k] || 0) + n;
    return c;
  })();
  const src = page.startsWith('src:') ? page.slice(4) : null;
  const fhCounts = useFlathubCounts();
  const webCounts = useMemo(() => webCategoryCounts(), []);
  // an origin's second column: All, then its categories with how many apps each has
  const subCounts = src === 'debian' ? counts : src === 'flathub' ? fhCounts : src === 'web' ? webCounts : null;
  const subTotal = subCounts ? CATS.reduce((a, c) => a + (subCounts[c] || 0), 0) : 0;
  const remember = () => { if (!app) listAt.current = { key: listKey, y: scroll.current?.offset() || 0 }; };
  const openDeb = (name) => { remember(); setApp({ name, ai: false }); };
  const openAi = (name) => { remember(); setApp({ name, ai: true }); };
  const openWeb = (id) => { remember(); setApp({ name: id, web: true }); };
  const openFlathub = (id) => { remember(); setApp({ name: id, flathub: true }); };
  const srcTitle = { debian: tr('store.src.debian'), flathub: tr('store.page.flathub'), web: tr('store.page.webapps') };
  const pageTitle = src ? [srcTitle[src], sub ? tr(`store.cat.${sub}`) : null].filter(Boolean).join(' · ') : tr(`store.page.${page}`);
  const appTitle = app && (app.flathub ? flathub.catalogApp(app.name)?.name : app.web ? webCatalog().find((a) => a.id === app.name)?.name || webAppEntry(app.name)?.name
    : app.ai ? catalogApp(app.name)?.name : (lord.pkg?.info?.(app.name, {})?.app?.name)) || app?.name;
  const searchable = !app && page !== 'updates' && page !== 'suggested';
  const onSearch = (v) => { setQuery(v); if (v && !searchable) go('discover'); };
  let body;
  if (app) body = app.flathub ? <FlathubDetail id={app.name} /> : app.web ? <WebAppDetail id={app.name} /> : app.ai ? <AiDetail pkg={app.name} /> : <debian.Details name={app.name} />;
  else if (src === 'debian') {
    body = sub ? (
      <View style={{ gap: 26 }}>
        <CatalogCategory cat={sub} query={query} onOpen={openAi} />
        <debian.Category cat={sub} query={query} onOpen={openDeb} />
      </View>
    ) : <debian.Discover query={query} onOpen={openDeb} onCategory={(c) => go('src:debian', c)} top={null} hide={[]} />;
  }
  else if (src === 'flathub') body = <FlathubPage query={query} onOpen={openFlathub} category={sub} />;
  else if (src === 'web') body = sub ? <WebCategory cat={sub} query={query} onOpen={openWeb} /> : <WebAppsPage query={query} onOpen={openWeb} />;
  else if (page === 'discover') {
    // while searching, the catalog apps that match come first (#37), then the web apps that match
    // (and the package results leave them out)
    const hits = query.trim() ? searchCatalog(catalogApps(), query) : [];
    const web = query.trim() ? webHits(query) : 0;
    const fhHits = query.trim() && fh.on ? flathub.state.get().catalog.length : 0;
    const top = !query.trim() ? (
      <View style={{ gap: 26 }}>
        <AiDiscover onOpen={openAi} onOpenDeb={openDeb} onMore={() => go('ai')} />
        <WebDiscover onOpen={openWeb} onMore={() => go('webapps')} />
      </View>
    ) : hits.length || web || fhHits ? (
      <View style={{ gap: 26 }}>
        {hits.length ? <CatalogResults query={query} onOpen={openAi} /> : null}
        {web ? <WebResults query={query} onOpen={openWeb} /> : null}
        {fhHits ? <FlathubResults query={query} onOpen={openFlathub} onMore={() => go('flathub')} /> : null}
      </View>
    ) : null;
    body = <debian.Discover query={query} onOpen={openDeb} onCategory={(c) => go('src:debian', c)} top={top} hide={hits.map((a) => a.package)} />;
  }
  else if (page === 'suggested') body = <debian.SuggestedPage />;
  else if (page === 'ai') body = <AiPage query={query} onOpen={openAi} />;
  else if (page === 'commercial') body = <AiPage section="commercial" query={query} onOpen={openAi} onPage={go} />;
  else if (page === 'remote') body = <AiPage section="remote" query={query} onOpen={openAi} />;
  else if (page === 'suites') body = <AiPage section="suites" query={query} onOpen={openAi} />;
  else if (page === 'lordos') body = <LordOSPage query={query} />;
  else if (page === 'installed') body = <InstalledPage query={query} filter={args?.filter} />;
  else body = <View><FlathubUpdates /><debian.Updates /></View>;
  const item = (id, label, detail, glyph) => (
    <SidebarItem key={id} icon={glyph} label={label} detail={detail || null} selected={!app && page === id} onClick={() => { go(id); if (side.narrow) side.close(); }} />
  );
  const sidebar = (
      // above the column after it: its edge handle reaches 3 px over that column, which would otherwise take the press
      <View style={{ width: side.narrow ? undefined : sideW, flex: side.narrow ? 1 : undefined, backgroundColor: t.sidebarBg, zIndex: side.narrow ? 0 : 1 }}>
        <Hairline edge="end" />
        {side.narrow ? null : <View onMouseDown={startSide} style={{ position: 'absolute', top: 0, bottom: 0, zIndex: 2, ...(direction() === 'rtl' ? { left: -3 } : { right: -3 }), width: 7, cursor: 'ew-resize' }} />}
        <ScrollView style={{ flex: 1, paddingHorizontal: 10, paddingTop: 4 }}>
          <SectionTitle>{tr('store.section.store')}</SectionTitle>
          {item('discover', tr('store.page.discover'), null, PAGE_GLYPH.discover)}
          {item('suggested', tr('store.page.suggested'), unseen, PAGE_GLYPH.suggested)}
          {item('installed', tr('store.page.installed'), launcher.length, PAGE_GLYPH.installed)}
          {item('updates', tr('store.page.updates'), deb.upgrades?.length, PAGE_GLYPH.updates)}
          <SectionTitle>{tr('store.section.origin')}</SectionTitle>
          {item('src:debian', tr('store.src.debian'), null, GLYPH.debian)}
          {fh.supported ? item('src:flathub', tr('store.page.flathub'), null, PAGE_GLYPH.flathub) : null}
          {item('src:web', tr('store.page.webapps'), webCatalog().length, PAGE_GLYPH.webapps)}
          {item('lordos', tr('store.page.lordos'), null, PAGE_GLYPH.lordos)}
          {/* what comes later, said as such and not clickable */}
          <View style={{ opacity: 0.45 }}><SidebarItem icon={GLYPH.wine} label={tr('store.src.wine')} detail={tr('store.soon')} /></View>
          <View style={{ opacity: 0.45 }}><SidebarItem icon={GLYPH.android} label={tr('store.src.android')} detail={tr('store.soon')} /></View>
          <SectionTitle>{tr('store.section.picks')}</SectionTitle>
          {item('ai', tr('store.page.ai'), catalogApps('ai').length, PAGE_GLYPH.ai)}
          {catalogApps('commercial').length ? item('commercial', tr('store.page.commercial'), catalogApps('commercial').length, PAGE_GLYPH.commercial) : null}
          {catalogApps('remote').length ? item('remote', tr('store.page.remote'), catalogApps('remote').length, PAGE_GLYPH.remote) : null}
          {catalogApps('suites').length ? item('suites', tr('store.page.suites'), catalogApps('suites').length, PAGE_GLYPH.suites) : null}
          <View style={{ height: 12 }} />
        </ScrollView>
        <View style={{ paddingHorizontal: 20, paddingBottom: 12, paddingTop: 6 }}><debian.Status /></View>
      </View>
  );
  return (
    <View style={{ flex: 1, flexDirection: 'row' }}>
      {side.narrow ? null : sidebar}
      {subCounts && !app && !side.narrow ? (
        <View style={{ width: 190, backgroundColor: t.sidebarBg }}>
          <Hairline edge="end" />
          <ScrollView style={{ flex: 1, paddingHorizontal: 10, paddingTop: 4 }}>
            <SectionTitle>{tr('store.section.categories')}</SectionTitle>
            <SidebarItem label={tr('store.sub.all')} detail={subTotal || null} selected={!sub} onClick={() => setSub(null)} />
            {CATS.filter((c) => subCounts[c]).map((c) => (
              <SidebarItem key={c} icon={GLYPH[c]} label={tr(`store.cat.${c}`)} detail={subCounts[c]} selected={sub === c} onClick={() => setSub(c)} />
            ))}
            <View style={{ height: 12 }} />
          </ScrollView>
        </View>
      ) : null}
      <View style={{ flex: 1, minWidth: 0 }}>
        <Toolbar>
          {side.narrow ? <SidebarToggle small shown={side.shown} onClick={side.toggle} /> : null}
          {app ? <ToolbarButton icon={direction() === 'rtl' ? '›' : '‹'} iconSize={20} small title={tr('store.back')} onClick={() => setApp(null)} /> : null}
          <View style={{ flex: 1, minWidth: 0 }}>
            <Text style={{ fontSize: typeScale.headline, fontWeight: 600, color: t.text, whiteSpace: 'nowrap', overflow: 'hidden', textAlign: 'start' }}>{app ? appTitle : pageTitle}</Text>
          </View>
          {!app ? <SearchField value={query} onChange={onSearch} placeholder={tr('store.search')} width={side.narrow ? 150 : 200} /> : null}
        </Toolbar>
        <EndsContext.Provider value={ends}>
        <ScrollView scrollRef={scroll} onScroll={ends.check} resetKey={app ? `app:${app.flathub ? 'flathub' : app.web ? 'web' : app.ai ? 'ai' : 'deb'}:${app.name}` : listKey} style={{ flex: 1, paddingHorizontal: side.narrow ? 16 : 24, paddingTop: side.narrow ? 14 : 20 }}>
          {subCounts && !app && side.narrow ? (
            <View style={{ flexDirection: 'row', flexWrap: 'wrap', gap: 6, marginBottom: 14 }}>
              {[null, ...CATS.filter((c) => subCounts[c])].map((c) => (
                <View key={c || 'all'} onClick={() => setSub(c)} style={{ height: 26, paddingHorizontal: 12, borderRadius: 13, justifyContent: 'center',
                  backgroundColor: sub === c ? t.accent : t.hover }}>
                  <Text style={{ fontSize: 12, fontWeight: 600, color: sub === c ? '#fff' : t.text }}>{c ? tr(`store.cat.${c}`) : tr('store.sub.all')}</Text>
                </View>
              ))}
            </View>
          ) : null}
          {file && <InstallFile path={file} onDone={(cancel) => cancel && setFile(null)} />}
          {body}
          <View style={{ height: 24 }} />
        </ScrollView>
        </EndsContext.Provider>
      </View>
      {side.narrow && side.shown ? <SidebarOverlay width={sideW} onClose={side.close}>{sidebar}</SidebarOverlay> : null}
    </View>
  );
}
