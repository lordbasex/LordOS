// cloud.jsx — Files and Cloud (docs/CLOUD.md section 7, docs/FILES.md "Cloud"): the Cloud section of the sidebar,
// each file's status (asked for the visible rows only, again on every push), the context menu's cloud items, the
// Share Link sheet, Get Info's cloud part, the header's transfers pill and the offline dialog. Built from the
// approved mocks (Files, FilesMenu, ShareLink, GetInfo, States).
import { View, Text, Button, Segmented, useTheme, useCloud, cloud, cloudModel as M, clipboard, toast, openUrl, formatBytes, formatDate, formatTime, spacing, typeScale, t as tr,
  CloudGlyph, StatusGlyph, UsageRing, addListener, Toggle, errorText, tOptional, cloudState } from '@lordos/sdk';
import { useEffect, useMemo, useRef, useState } from 'react';
import { SideItem, SideTitle } from '../../sdk/fileparts.jsx';
import { EntryIcon } from './glyphs.jsx';

// ---- status of the visible rows ----

// { accounts, st, status: { path: { st, pct, error } }, accountOf(path), cloudOnly(entry) } for the paths shown; asked again
// when they change and on each pushed state (at most twice a second).
export function useCloudFiles(paths) {
  const st = useCloud();
  const accounts = (st && st.accounts) || [];
  const [status, setStatus] = useState({});
  const key = paths.join('\n');
  useEffect(() => {
    const ask = M.pathsToAsk(paths, accounts);
    if (!ask.length) { setStatus((s) => (Object.keys(s).length ? {} : s)); return; }
    const answer = cloud.status(ask);
    // a file that just came down isn't shown in the cloud again by a stale answer (U3)
    setStatus((prev) => {
      const next = M.mergeStatusHeld(Object.fromEntries(Object.entries(prev).filter(([p]) => ask.includes(p))), answer, ask, Date.now());
      return M.sameStatus(prev, next) ? prev : next;      // nothing changed: the list isn't drawn again
    });
  }, [key, st]);
  return {
    accounts, st, status,
    accountOf: (p) => M.accountOf(p, accounts),
    // only in the cloud (or not known yet): no thumbnail and no preview, which would bring it down
    cloudOnly: (e) => !!e && !e.dir && !!M.accountOf(e.path, accounts) && (status[e.path]?.st || 'cloud') === 'cloud',
  };
}

// ---- the sidebar's Cloud section (mock Files) ----

export function CloudSection({ t, accounts, hidden, sideSel, navigate, dropTarget, onContext }) {
  const items = M.cloudSideItems(accounts, hidden, (cloudState.get() || {}).service);
  if (!items.length) return null;
  return (
    <>
      <SideTitle t={t}>{tr('cloud.title')}</SideTitle>
      {items.map((it) => (
        <SideItem key={it.id} t={t} label={it.name} dim={it.dim} selected={sideSel(it.path)} onClick={() => navigate(it.path)}
          icon={<View style={{ width: 18, height: 18, borderRadius: 5, backgroundColor: it.alert ? '#ff9f0a' : it.color, alignItems: 'center', justifyContent: 'center', opacity: it.dim ? 0.6 : 1 }}>
            {it.alert ? <Text style={{ fontSize: 11, fontWeight: 700, color: '#fff' }}>!</Text> : <CloudGlyph kind="cloud" size={13} color="#ffffff" />}</View>}
          accessory={it.pct != null ? <View title={tr('cloud.usedPct', { pct: Math.round(it.pct * 100) })} style={{ opacity: it.dim ? 0.5 : 1 }}><UsageRing size={14} parts={[{ frac: it.pct, color: it.color }]} /></View> : null}
          drop={it.dim ? undefined : { dropPath: it.path }} hot={dropTarget === it.path} onContext={onContext ? (ev) => onContext(ev, it) : undefined} />
      ))}
    </>
  );
}

// ---- the header's pill: "2 coming down · 1 going up" ----

export function TransfersPill({ t, accounts, acc }) {
  const x = M.transfers(accounts, acc ? acc.id : null);
  if (!x.down && !x.up) return null;
  return (
    <View style={{ flexDirection: 'row', alignItems: 'center', gap: 6, paddingHorizontal: 10, height: 24, borderRadius: 12, backgroundColor: t.mode === 'dark' ? '#2e2e34' : 'rgba(0,0,0,0.06)' }}>
      {x.down ? <Text style={{ fontSize: 13, fontWeight: 700, color: '#0a84ff' }}>↓</Text> : null}
      {x.down ? <Text style={{ fontSize: typeScale.callout, color: t.textDim, whiteSpace: 'nowrap' }}>{tr('cloud.comingDown', { count: x.down })}</Text> : null}
      {x.down && x.up ? <Text style={{ fontSize: typeScale.callout, color: t.textDim }}>·</Text> : null}
      {x.up ? <Text style={{ fontSize: 13, fontWeight: 700, color: '#bf5af2' }}>↑</Text> : null}
      {x.up ? <Text style={{ fontSize: typeScale.callout, color: t.textDim, whiteSpace: 'nowrap' }}>{tr('cloud.goingUp', { count: x.up })}</Text> : null}
    </View>
  );
}

// the status bar's end: "540 GB of 1 TB used in Work · 4.1 GB on this computer"
export function footerLine(acc) {
  if (!acc) return '';
  const u = M.usage(acc.about);
  // nothing about space while it isn't known (an unmeasured account says 0 used: U4)
  const used = !u || M.notMeasured(acc.about) ? '' : u.total != null ? tr('cloud.footer.usedOf', { used: formatBytes(u.used), total: formatBytes(u.total), name: acc.name })
    : tr('cloud.footer.used', { used: formatBytes(u.used), name: acc.name });
  return [used, tr('cloud.footer.here', { size: formatBytes(acc.cacheBytes || 0) })].filter(Boolean).join(' · ');
}

// ---- the context menu's cloud items (mock FilesMenu) ----

// list: the entries the menu acts on; returns the items to put after Open… (and a separator), or []
export function cloudMenuItems(list, cf, { onShare, onMsg }) {
  if (!list.length) return [];
  const acc = cf.accountOf(list[0].path);
  if (!acc || list.some((e) => cf.accountOf(e.path) !== acc) || list.some((e) => e.path === acc.mount)) return [];
  const paths = list.map((e) => e.path);
  const links = acc.linksSupported ? cloud.links(acc.id) : [];
  const single = list.length === 1;
  const shared = single && !!M.linkOf(links, paths[0]);
  const website = single ? cloud.websiteUrl(acc.id) : null;
  const sel = list.map((e) => ({ st: cf.status[e.path]?.st, dir: !!e.dir }));
  const files = list.filter((e) => !e.dir);
  const sizeOf = (want) => files.filter((e) => want(cf.status[e.path]?.st)).reduce((s, e) => s + (e.size || 0), 0);
  const keepSize = sizeOf((s) => s !== 'kept' && s !== 'here'), freeSize = sizeOf((s) => s === 'here');
  const items = [];
  for (const it of M.cloudMenu(sel, { single, linksSupported: acc.linksSupported, shared, website: !!website })) {
    if (it === 'sep') { items.push('-'); continue; }
    switch (it.id) {
      case 'keep': items.push({ label: tr('cloud.keep'), checked: it.checked, hint: !it.checked && keepSize ? formatBytes(keepSize) : undefined,
        onClick: () => (it.checked ? cloud.unkeep(paths) : cloud.keep(paths)) }); break;
      case 'free': items.push({ label: tr('cloud.freeUp'), disabled: it.disabled, hint: freeSize ? formatBytes(freeSize) : undefined, onClick: () => {
        const r = cloud.free(paths);
        const skipped = (r && r.skipped) || [];
        onMsg(skipped.length ? tr('cloud.freedSkipped', { size: formatBytes((r && r.freed) || 0), count: skipped.length }) : tr('cloud.freed', { size: formatBytes((r && r.freed) || 0) }));
      } }); break;
      case 'copyLink': items.push({ label: tr('cloud.copyLink'), onClick: () => {
        const l = M.linkOf(links, paths[0]);
        Promise.resolve(l ? { url: l.url } : cloud.link(paths[0], M.DEFAULT_EXPIRY)).then((r) => {
          if (r && r.url) { clipboard.set(r.url); toast.set(tr('cloud.linkCopied')); } else onMsg(linkError(r));
        });
      } }); break;
      case 'shareLink': items.push({ label: tr('cloud.shareLink'), onClick: () => onShare(list[0]) }); break;
      case 'openWebsite': items.push({ label: tr('cloud.openOnWebsite'), onClick: () => openUrl(website) }); break;
      case 'stopSharing': items.push({ label: tr('cloud.stopSharing'), danger: true, onClick: () => cloud.unlink(paths[0]).then((r) => { if (r && r.error) onMsg(errorMessage(r)); }) }); break;
      default: break;
    }
  }
  return items.length ? [...items, '-'] : [];
}
// an error answer with its details (a date, fingerprints) in the person's language
export const errorMessage = (r) => { const e = M.errorVars(typeof r === 'string' ? { error: r } : r, (d) => formatDate(d, 'short')); return tOptional(`errors.${e.code}`, e.vars) || errorText(e.code); };
const linkError = (r) => (r && r.error === 'unsupported' ? tr('cloud.link.unsupported') : tr('cloud.link.failed'));

// ---- Share Link… (mock ShareLink) ----

export function ShareSheet({ e, cf, onClose }) {
  const t = useTheme();
  const acc = cf.accountOf(e.path);
  const existing = useMemo(() => M.linkOf(cloud.links(acc.id), e.path), [e.path]);
  const [expire, setExpire] = useState(existing ? null : M.DEFAULT_EXPIRY);
  const [link, setLink] = useState(existing ? existing.url : null);
  const [until, setUntil] = useState(existing ? existing.expires : null);     // what the provider answered (S3: 7 days at most)
  const [err, setErr] = useState('');
  const website = useMemo(() => cloud.websiteUrl(acc.id), [acc.id]);
  useEffect(() => addListener('keydown', (ev) => { if (ev.key === 'Escape') onClose(); }), []);
  const make = (x) => {
    Promise.resolve(cloud.link(e.path, x)).then((r) => { if (r && r.url) { setLink(r.url); setUntil(r.expires || null); setErr(''); } else setErr(linkError(r)); });
  };
  useEffect(() => { if (!existing && acc.linksSupported) make(M.DEFAULT_EXPIRY); }, []);
  const copy = () => { if (link) { clipboard.set(link); toast.set(tr('cloud.linkCopied')); } };
  const where = M.whereLine(e.path, acc, '·');
  return (
    <View onMouseDown={(ev) => ev.stopPropagation()} style={{ position: 'absolute', left: 0, top: 0, right: 0, bottom: 0, backgroundColor: 'rgba(0,0,0,0.45)', alignItems: 'center', paddingTop: 70 }}>
      <View style={{ width: 500, padding: 22, gap: 16, borderRadius: 14, backgroundColor: t.mode === 'dark' ? '#2a2a30' : t.windowBg, border: `1px solid ${t.separator}`, boxShadow: '0 30px 80px rgba(0,0,0,0.55)' }}>
        <View style={{ flexDirection: 'row', alignItems: 'center', gap: 14 }}>
          <EntryIcon e={e} size={48} dark={t.mode === 'dark'} thumb={false} />
          <View style={{ flex: 1, minWidth: 0, gap: 3 }}>
            <Text style={{ fontSize: 16, fontWeight: 600, color: t.text, whiteSpace: 'nowrap', overflow: 'hidden', textAlign: 'start' }}>{e.name}</Text>
            <Text style={{ fontSize: typeScale.callout, color: t.textDim, textAlign: 'start' }}>{[where, e.dir ? '' : formatBytes(e.size || 0)].filter(Boolean).join(' · ')}</Text>
          </View>
        </View>
        {acc.linksSupported ? (
          <>
            <View style={{ gap: 6 }}>
              <Text style={{ fontSize: typeScale.callout, color: t.textDim, textAlign: 'start' }}>{tr('cloud.share.anyone')}</Text>
              <View style={{ flexDirection: 'row', gap: 8 }}>
                <View style={{ flex: 1, minWidth: 0, height: 32, justifyContent: 'center', paddingHorizontal: 10, borderRadius: 7, backgroundColor: t.mode === 'dark' ? '#1e1e22' : t.inputBg || '#f4f4f6', border: `1px solid ${t.separator}` }}>
                  <Text style={{ fontSize: typeScale.body, color: link ? t.text : t.textDim, whiteSpace: 'nowrap', overflow: 'hidden', textOverflow: 'ellipsis', direction: 'ltr', textAlign: 'start' }}>{link || tr('cloud.share.making')}</Text>
                </View>
                <Button primary label={tr('cloud.copy')} disabled={!link} onClick={copy} />
              </View>
            </View>
            <View style={{ gap: 6 }}>
              <Text style={{ fontSize: typeScale.callout, color: t.textDim, textAlign: 'start' }}>{tr('cloud.share.expires')}</Text>
              <Segmented fill value={expire} onChange={(x) => { setExpire(x); make(x); }} options={M.expiryChoices(acc).map((x) => [x, tr(`cloud.expiry.${x}`)])} appearance="neutral" />
              {link ? <Text style={{ fontSize: typeScale.caption, color: t.textDim, textAlign: 'start' }}>{until ? tr('cloud.link.expires', { date: formatDate(new Date(M.toMs(until)), 'short') }) : tr('cloud.link.never')}</Text> : null}
            </View>
          </>
        ) : <Text style={{ fontSize: typeScale.body, color: t.textDim, textAlign: 'start' }}>{tr('cloud.link.unsupported')}</Text>}
        {err ? <Text style={{ fontSize: typeScale.callout, color: '#ff453a', textAlign: 'start' }}>{err}</Text> : null}
        <View style={{ paddingHorizontal: 12, paddingVertical: 10, borderRadius: 8, backgroundColor: t.mode === 'dark' ? '#1e1e22' : '#f4f4f6' }}>
          <Text style={{ fontSize: typeScale.callout, color: t.textDim, lineHeight: 1.5, textAlign: 'start' }}>{tr('cloud.share.provider')}</Text>
        </View>
        <View style={{ flexDirection: 'row', alignItems: 'center', gap: 8 }}>
          {link ? <Button plain destructive label={tr('cloud.stopSharing')} onClick={() => cloud.unlink(e.path).then((r) => { if (r && r.error) setErr(errorMessage(r)); else onClose(); })} /> : null}
          <View style={{ flex: 1 }} />
          {website ? <Button label={tr('cloud.openOnWebsite')} onClick={() => openUrl(website)} /> : null}
          <Button label={tr('cloud.done')} onClick={onClose} />
        </View>
      </View>
    </View>
  );
}

// ---- Get Info's cloud part (mock GetInfo) ----

const when = (ms) => { if (!ms) return '—'; const d = new Date(M.toMs(ms)); return `${formatDate(d, 'short')} ${formatTime(d)}`; };

export function CloudInfo({ e, cf, onShare }) {
  const t = useTheme();
  const acc = cf.accountOf(e.path);
  const s = cf.status[e.path] || (e.path === acc?.mount ? null : cloud.status([e.path])[e.path]) || null;
  const samples = useRef([]);
  const size = e.size || 0;
  const done = s && s.st === 'down' && s.pct != null ? Math.round(size * (s.pct > 1 ? s.pct / 100 : s.pct)) : null;
  useEffect(() => {
    if (done == null) { samples.current = []; return; }
    samples.current = [...samples.current, { at: Date.now(), done }].slice(-6);
  }, [done]);
  if (!acc) return null;
  const links = acc.linksSupported ? cloud.links(acc.id) : [];
  const link = M.linkOf(links, e.path);
  const last = (cf.st?.recent || []).find((r) => r.path === e.path);
  const here = !s ? null : s.st === 'cloud' ? 0 : s.st === 'down' ? done : size;
  const g = M.statusGlyph(s);
  const rate = done != null ? M.rateOf(samples.current, size) : null;
  const kept = s && s.st === 'kept';
  const website = cloud.websiteUrl(acc.id);
  const row = (k, v) => (
    <View key={k} style={{ flexDirection: 'row', gap: spacing.m, paddingVertical: 3 }}>
      <Text style={{ width: 96, fontSize: typeScale.callout, color: t.textDim, textAlign: 'end' }}>{k}</Text>
      <Text style={{ flex: 1, fontSize: typeScale.callout, color: t.text, textAlign: 'start' }}>{v}</Text>
    </View>
  );
  return (
    <View style={{ gap: 8 }}>
      {g ? (
        <View style={{ gap: 8 }}>
          <View style={{ flexDirection: 'row', alignItems: 'center', gap: 8 }}>
            <StatusGlyph s={s} size={18} />
            <Text style={{ fontSize: typeScale.body, fontWeight: 600, color: t.text, textAlign: 'start' }}>
              {g.pct != null ? tr('cloud.info.moving', { what: tr(g.key), pct: Math.round(g.pct * 100) }) : tr(g.key)}</Text>
          </View>
          {s.st === 'down' && g.pct != null ? (
            <View style={{ height: 6, borderRadius: 3, backgroundColor: t.mode === 'dark' ? '#3a3a42' : '#e2e2e7', overflow: 'hidden', direction: 'ltr' }}>
              <View style={{ width: `${Math.round(g.pct * 100)}%`, height: 6, backgroundColor: '#0a84ff' }} />
            </View>
          ) : null}
          {s.st === 'down' && done != null && !e.dir ? (
            <Text style={{ fontSize: typeScale.callout, color: t.textDim, textAlign: 'start' }}>
              {[tr('cloud.info.of', { done: formatBytes(done), total: formatBytes(size) }), rate ? `${formatBytes(rate.rate)}/s` : '', rate && rate.left ? tr('cloud.info.left', { s: rate.left }) : ''].filter(Boolean).join(' · ')}</Text>
          ) : null}
          {s.st === 'error' ? <Text style={{ fontSize: typeScale.callout, color: '#ff9f0a', textAlign: 'start' }}>{(s.error || acc.message) ? errorText(s.error || acc.message) : tr('cloud.info.failed')}</Text> : null}
        </View>
      ) : null}
      <View style={{ height: 1, backgroundColor: t.separator }} />
      <View>
        {row(tr('cloud.info.account'), `${acc.name} (${M.providerName(acc)})`)}
        {row(tr('cloud.info.where'), M.whereLine(e.path, acc) || acc.name)}
        {!e.dir ? row(tr('cloud.info.inCloud'), formatBytes(size)) : null}
        {!e.dir && here != null ? row(tr('cloud.info.here'), s.st === 'down' ? tr('cloud.info.soFar', { size: formatBytes(here) }) : formatBytes(here)) : null}
        {row(tr('cloud.info.lastSynced'), last ? when(last.at) : '—')}
        {row(tr('cloud.info.shared'), link ? (link.expires ? tr('cloud.link.expires', { date: formatDate(new Date(M.toMs(link.expires)), 'short') }) : tr('cloud.link.never')) : tr('cloud.info.noLink'))}
      </View>
      <View style={{ height: 1, backgroundColor: t.separator }} />
      <View style={{ flexDirection: 'row', alignItems: 'center', gap: 10 }}>
        <Toggle small on={kept} onChange={(v) => (v ? cloud.keep([e.path]) : cloud.unkeep([e.path]))} />
        <Text style={{ fontSize: typeScale.body, color: t.text }}>{tr('cloud.keepToggle')}</Text>
      </View>
      {acc.linksSupported || website ? (
        <View style={{ flexDirection: 'row', gap: 8 }}>
          {acc.linksSupported ? <Button small label={tr('cloud.shareLink')} style={{ flex: 1 }} onClick={() => onShare(e)} /> : null}
          {website ? <Button small label={tr('cloud.openOnWebsite')} style={{ flex: 1 }} onClick={() => openUrl(website)} /> : null}
        </View>
      ) : null}
    </View>
  );
}

// ---- opening a file that isn't here while the cloud can't be reached (mock States) ----

export function OfflineDialog({ e, onClose }) {
  const t = useTheme();
  useEffect(() => addListener('keydown', (ev) => { if (ev.key === 'Escape' || ev.key === 'Enter') onClose(); }), []);
  return (
    <View onMouseDown={(ev) => ev.stopPropagation()} style={{ position: 'absolute', left: 0, top: 0, right: 0, bottom: 0, backgroundColor: 'rgba(0,0,0,0.3)', alignItems: 'center', justifyContent: 'center' }}>
      <View style={{ width: 420, padding: 16, gap: 12, borderRadius: 12, backgroundColor: t.mode === 'dark' ? '#2a2a30' : t.windowBg, border: `1px solid ${t.separator}`, boxShadow: '0 14px 36px rgba(0,0,0,0.45)' }}>
        <View style={{ flexDirection: 'row', gap: 12, alignItems: 'flex-start' }}>
          <View style={{ width: 34, height: 34, borderRadius: 17, backgroundColor: '#636366', alignItems: 'center', justifyContent: 'center' }}><CloudGlyph kind="cloud" size={20} color="#ffffff" /></View>
          <View style={{ flex: 1, minWidth: 0, gap: 4 }}>
            <Text style={{ fontSize: 14, fontWeight: 600, color: t.text, textAlign: 'start' }}>{tr('cloud.offline.title', { name: e.name })}</Text>
            <Text style={{ fontSize: typeScale.callout, color: t.textDim, lineHeight: 1.5, textAlign: 'start' }}>{tr('cloud.offline.body')}</Text>
          </View>
        </View>
        <View style={{ flexDirection: 'row', justifyContent: 'flex-end', gap: 8 }}>
          <Button label={tr('cloud.ok')} onClick={onClose} />
          <Button primary label={tr('cloud.keep')} onClick={() => { cloud.keep([e.path]); onClose(); }} />
        </View>
      </View>
    </View>
  );
}

