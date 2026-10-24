// Store: LordOS apps (.lpk packages, no root) and Debian apps (our own package engine,
// server/src/pkg; install and remove go through the administration helper).
import {
  View, Text, Image, useTheme, useLocale, useAppWindow, useStore, Button, ScrollView, SidebarItem, SectionTitle, SearchField, Hairline, EmptyState, GroupBox,
  packages, packageList, refreshPackages, useLauncherApps, launchApp, fs, formatBytes, wm, direction, typeScale, spacing, isSystemApp,
  suggestions, unseenSuggestions, refreshSuggestions, standaloneDesktop,
} from '@lordos/sdk';
import { useEffect, useMemo, useState } from 'react';
import { debian, useDebian } from './debian.js';

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
    for (const e of list) if (!e.dir && (standaloneDesktop ? /\.lpk$/i : /\.(lpk|deb)$/i).test(e.name)) out.push({ path: `${fs.home()}/${dir ? `${dir}/` : ''}${e.name}`, name: e.name, size: e.size });
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
    if (standaloneDesktop && isDeb) { setInfo(null); setMsg({ ok: false, text: tr('store.host.description') }); return; }
    if (isDeb) { setInfo(null); return; }
    try { setInfo(packages.inspect(path)); } catch (e) { setInfo(null); setMsg({ ok: false, text: tr('store.lordos.invalid', { error: e.message }) }); }
  }, [path]);
  const m = info?.manifest;
  const lang = useLocale().lang;
  const loc = (v) => (v && typeof v === 'object' ? v[lang] || v[lang.split('-')[0]] || v.en || Object.values(v)[0] : v) || '';
  const installed = m && packageList.get().find((p) => p.id === m.id);
  const install = () => {
    if (isDeb && !standaloneDesktop) {
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
      {isDeb && !standaloneDesktop && <Text style={{ fontSize: typeScale.callout, color: t.text, lineHeight: 1.4, textAlign: 'start' }}>{tr('store.file.deb')}</Text>}
      <Message msg={msg} />
      <View style={{ flexDirection: 'row', gap: spacing.s, justifyContent: 'flex-end' }}>
        <Button label={tr('store.close')} onClick={() => onDone?.(true)} />
        {(m || (isDeb && !standaloneDesktop)) && !msg?.ok && <Button primary label={busy ? tr('store.working') : tr('store.install')} onClick={() => !busy && install()} />}
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
  const q = query.trim().toLowerCase();
  const shown = list.filter((p) => !q || `${p.name} ${p.description} ${p.id}`.toLowerCase().includes(q));
  return (
    <View>
      <PageTitle title={tr('store.lordos.title')} subtitle={tr('store.lordos.subtitle')} />
      {pick && <InstallFile path={pick} onDone={(cancel) => { if (cancel) setPick(null); setFiles(findPackageFiles().filter((f) => /\.lpk$/i.test(f.name))); }} />}
      <Message msg={msg} />
      <List title={tr('store.lordos.installedTitle')} count={shown.length || null} empty={tr('store.lordos.none')}>
        {shown.map((p) => (
          <AppRow key={p.id} icon={<p.Icon size={40} />} name={p.name} detail={p.description} extra={tr('store.version', { version: p.version })}
            below={<PkgManage pkg={p} onMsg={setMsg} />}>
            <Button small label={tr('store.remove')} onClick={() => { packages.remove(p.id); setMsg({ ok: true, text: tr('store.lordos.removed', { name: p.name }) }); }} />
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

function InstalledPage({ query }) {
  const t = useTheme();
  const { t: tr } = useLocale();
  const apps = useLauncherApps();
  const q = query.trim().toLowerCase();
  const shown = apps.filter((a) => !q || `${a.name} ${a.comment || ''} ${a.id}`.toLowerCase().includes(q));
  const kind = (a) => tr(`store.kind.${a.kind}`);
  return (
    <View>
      <PageTitle title={tr('store.installed.title')} />
      <List count={null} empty={tr('store.debian.none')}>
        {shown.map((a) => (
          <AppRow key={a.id} icon={<a.Icon size={32} />} name={a.name} detail={a.comment || a.genericName || ''} extra={kind(a)}>
            {/* a system app is part of LordOS: never removable here (docs/SIGNING.md, "Verified system code") */}
            {a.kind === 'lordos' && isSystemApp(a.id) && <Text style={{ fontSize: typeScale.caption, color: t.textDim, whiteSpace: 'nowrap' }}>{tr('store.systemApp')}</Text>}
            {a.kind === 'linux' && debian.canRemove(a) && <Button small label={tr('store.remove')} onClick={() => debian.removeApp(a)} />}
            <Button small label={tr('store.open')} onClick={() => launchApp(a.id)} />
          </AppRow>
        ))}
      </List>
    </View>
  );
}

// ---- window ----

export default function Store() {
  const t = useTheme();
  const { t: tr } = useLocale();
  const { args } = useAppWindow();
  const [page, setPage] = useState('discover');
  const [query, setQuery] = useState('');
  const [file, setFile] = useState(null);
  const deb = useDebian();
  useEffect(() => {
    if (args?.install) setFile(args.install);
    if (args?.page) setPage(args.page);
    if (args?.search) { setQuery(args.search); setPage('discover'); }       // from Search
  }, [args?.nonce]);
  const sug = useStore(suggestions);
  useEffect(() => { refreshSuggestions(); }, []);                  // the hardware may have changed since the last look
  const unseen = page === 'discover' ? 0 : unseenSuggestions(sug.list).length;    // new suggestions (Discover's badge)
  const pages = [
    ['discover', tr('store.page.discover'), unseen || null],
    ['lordos', tr('store.page.lordos')],
    ['installed', tr('store.page.installed')],
    ['updates', deb.upgrades?.length ? tr('store.page.updatesCount', { count: deb.upgrades.length }) : tr('store.page.updates')],
  ];
  const Page = { discover: debian.Discover, lordos: LordOSPage, installed: InstalledPage, updates: debian.Updates }[page];
  return (
    <View style={{ flex: 1, flexDirection: 'row' }}>
      <View style={{ width: 210, backgroundColor: t.sidebarBg, paddingHorizontal: 10, paddingTop: 10, paddingBottom: 12, gap: 2 }}>
        <Hairline edge="end" />
        <SearchField value={query} onChange={(v) => { setQuery(v); if (v && page !== 'discover' && page !== 'installed' && page !== 'lordos') setPage('discover'); }}
          placeholder={tr('store.search')} style={{ marginBottom: 10 }} />
        {pages.map(([id, label, detail]) => <SidebarItem key={id} label={label} detail={detail} selected={page === id} onClick={() => setPage(id)} />)}
        <View style={{ flex: 1 }} />
        <View style={{ paddingHorizontal: 10 }}><debian.Status /></View>
      </View>
      <ScrollView style={{ flex: 1, paddingHorizontal: 20, paddingTop: 20 }}>
        {file && <InstallFile path={file} onDone={(cancel) => cancel && setFile(null)} />}
        <Page query={query} onPage={setPage} />
        <View style={{ height: 20 }} />
      </ScrollView>
    </View>
  );
}
