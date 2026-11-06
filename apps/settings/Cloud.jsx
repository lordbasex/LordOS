// Settings → Cloud (docs/CLOUD.md sections 6, 8, 12): the accounts with their space, what Cloud keeps on this
// computer, bandwidth, the service and About rclone; the sheets to add an account (providers, sign-in in the
// browser, a server's or a bucket's credentials) and an account's own page. Built from the approved mocks (Main,
// AddAccount, SignIn, Credentials, Account). Everything goes through cloud (sdk/cloud.js); nothing here talks to
// rclone.
import {
  View, Text, Button, Toggle, TextInput, SearchField, ScrollView, useTheme, useLocale, useStore, useCloud, cloud, cloudModel, createStore,
  formatBytes, formatDate, tOptional, spacing, typeScale, wm, Layer, useAppWindow, FilePicker, openUrl, clipboard, toast, addListener, direction, errorText, fs,
  AccountBadge, UsageRing, SpaceBar, CloudGlyph, useSettings,
} from '@lordos/sdk';
import { useEffect, useMemo, useRef, useState } from 'react';
import { Stack, Section, SettingRow, Note, Actions, PopUp, tint } from './ui.jsx';

const M = cloudModel;
// the page and the open sheet survive the header's button and a remount: { page: account id | null, sheet }
export const cloudView = createStore({ page: null, sheet: null });
const setView = (p) => cloudView.set(p);

const RCLONE_SITE = 'https://rclone.org/';
const RCLONE_LICENSE = 'https://github.com/rclone/rclone/blob/master/COPYING';
const MB = 1e6;
const LIMITS = [0, 1, 2, 5, 10, 20, 50];                // MB/s, 0 = no limit
const FREE_BELOW = [5, 10, 20, 50, 100];                // GB (section 7.2)

// ---- small pieces ----

const box = (t) => ({ borderRadius: t.groupRadius + 2, backgroundColor: t.mode === 'dark' ? 'rgba(255,255,255,0.04)' : '#ffffff', border: `1px solid ${t.separator}`, overflow: 'hidden' });
const Chevron = ({ t }) => <Text style={{ fontSize: 15, color: t.textDim, width: 16, textAlign: 'center' }}>{direction() === 'rtl' ? '‹' : '›'}</Text>;
const Dim = ({ t, children, style }) => <Text style={[{ fontSize: typeScale.callout, color: t.textDim, textAlign: 'start', lineHeight: 1.4 }, style]}>{children}</Text>;
const toneColor = (t, tone) => (tone === 'warn' ? '#ffb340' : tone === 'busy' ? (t.mode === 'dark' ? '#64b5ff' : '#0a6cd6') : t.textDim);
const fmtSpace = (about, tr) => { const l = M.spaceLine(about, formatBytes); return tr(l.key, l.vars); };
const limitLabel = (v, tr) => (v ? tr('cloud.limit.mbs', { count: v }) : tr('cloud.limit.none'));

// A sheet over the Settings window (in the modal layer): a dim backdrop and a centered card. Escape closes it.
function Sheet({ width = 460, top, onClose, children, label }) {
  const t = useTheme();
  const win = useAppWindow();
  const g = wm.frame(wm.get(win.id) || {});
  useEffect(() => addListener('keydown', (e) => { if (e.key === 'Escape' && onClose) onClose(); }), [onClose]);
  return (
    <Layer name="modal">
      <View style={{ position: 'absolute', left: g.x || 0, top: g.y || 0, width: g.w || 880, height: g.h || 600, backgroundColor: 'rgba(0,0,0,0.45)',
        alignItems: 'center', justifyContent: top == null ? 'center' : 'flex-start', paddingTop: top || 0 }}>
        <View aria-label={label} onMouseDown={(e) => e.stopPropagation?.()} style={{ width, maxHeight: (g.h || 600) - 40, borderRadius: 14, backgroundColor: t.mode === 'dark' ? '#2a2a30' : t.windowBg,
          border: `1px solid ${t.separator}`, boxShadow: '0 30px 80px rgba(0,0,0,0.55)', overflow: 'hidden' }}>
          {children}
        </View>
      </View>
    </Layer>
  );
}

// A row of an inset box: content at the start, controls at the end, a hairline under it but the last.
function BoxRow({ t, children, last, onClick, pad = 11 }) {
  return (
    <View onClick={onClick} style={{ flexDirection: 'row', alignItems: 'center', gap: spacing.m, paddingHorizontal: 14, paddingVertical: pad,
      borderBottomWidth: last ? 0 : 1, borderBottomColor: t.separator }}>{children}</View>
  );
}

// ---- the overview (mock Main) ----

function AllAccounts({ st }) {
  const t = useTheme();
  const { t: tr } = useLocale();
  const bar = M.allAccountsBar(st.accounts);
  if (!bar.segs.length) return null;
  return (
    <View aria-label={tr('cloud.allAccounts')} style={[box(t), { padding: 16, gap: 10 }]}>
      <View style={{ flexDirection: 'row', alignItems: 'baseline' }}>
        <Text style={{ flex: 1, fontSize: typeScale.body, fontWeight: 600, color: t.text, textAlign: 'start' }}>{tr('cloud.allAccounts')}</Text>
        <Dim t={t}>{tr('cloud.space.usedOf', { used: formatBytes(bar.used), total: formatBytes(bar.total) })}</Dim>
      </View>
      <SpaceBar height={14} gap={2} parts={bar.segs.map((s) => ({ frac: s.pct, color: s.color }))} />
      <View style={{ flexDirection: 'row', flexWrap: 'wrap', gap: 16 }}>
        {bar.segs.map((s) => (
          <View key={s.id} style={{ flexDirection: 'row', alignItems: 'center', gap: 6 }}>
            <View style={{ width: 10, height: 10, borderRadius: 5, backgroundColor: s.color }} />
            <Text style={{ fontSize: typeScale.callout, color: t.text }}>{`${s.name} ${formatBytes(s.used)}`}</Text>
          </View>
        ))}
      </View>
    </View>
  );
}

function AccountRow({ a, accounts, last }) {
  const t = useTheme();
  const { t: tr } = useLocale();
  const u = M.usage(a.about);
  const line = M.stateLine(a);
  const color = M.accountColor(accounts, a.id);
  return (
    <BoxRow t={t} last={last} onClick={() => setView({ page: a.id, sheet: null })}>
      <AccountBadge color={color} size={32} />
      <View style={{ flex: 1, minWidth: 0, gap: 2 }}>
        <Text style={{ fontSize: typeScale.body, fontWeight: 600, color: t.text, textAlign: 'start' }}>{a.name}</Text>
        <View style={{ flexDirection: 'row', minWidth: 0 }}>
          <Text style={{ fontSize: typeScale.callout, color: t.textDim, whiteSpace: 'nowrap' }}>{`${a.provider || a.kind} · `}</Text>
          <Text style={{ flex: 1, minWidth: 0, fontSize: typeScale.callout, color: toneColor(t, line.tone === 'warn' ? 'warn' : 'dim'), whiteSpace: 'nowrap', overflow: 'hidden', textAlign: 'start' }}>{tr(line.key, line.vars)}</Text>
        </View>
      </View>
      <View style={{ width: 190, gap: 5 }}>
        <SpaceBar parts={u ? [{ frac: u.pct != null ? u.pct : 0.02, color }] : []} />
        <Text style={{ fontSize: typeScale.caption, color: t.textDim, textAlign: 'end' }}>{fmtSpace(a.about, tr)}</Text>
      </View>
      <Chevron t={t} />
    </BoxRow>
  );
}

function Overview({ st }) {
  const t = useTheme();
  const { t: tr } = useLocale();
  const accs = st.accounts || [];
  const o = st.options || {};
  const svc = st.service || {};
  const cache = accs.reduce((s, a) => s + (a.cacheBytes || 0), 0), kept = accs.reduce((s, a) => s + (a.keptBytes || 0), 0);
  const [msg, setMsg] = useState(null);
  const freeAll = () => {
    const r = cloud.free(accs.map((a) => a.mount));
    setMsg({ ok: true, text: tr('cloud.freed', { size: formatBytes(r?.freed || 0) }) });
  };
  const svcLine = svc.running
    ? [tr('cloud.service.running'), svc.version ? tr('cloud.service.version', { version: svc.version }) : '', svc.rssKb ? formatBytes(svc.rssKb * 1024) : '', tr('cloud.service.private')].filter(Boolean).join(' · ')
    : svc.error ? errorText(svc.error) : tr('cloud.service.stopped');
  return (
    <Stack gap={16}>
      {!accs.length ? (
        <View style={[box(t), { padding: 20, gap: 10, alignItems: 'center' }]}>
          <CloudGlyph kind="cloud" size={40} color={t.accent} />
          <Text style={{ fontSize: typeScale.headline, fontWeight: 600, color: t.text, textAlign: 'center' }}>{tr('cloud.empty.title')}</Text>
          <Dim t={t} style={{ textAlign: 'center' }}>{tr('cloud.empty.body')}</Dim>
          <Button primary label={tr('cloud.addAccount')} onClick={() => setView({ sheet: { kind: 'add' } })} />
        </View>
      ) : null}
      <AllAccounts st={st} />
      {accs.length ? (
        <View aria-label={tr('cloud.accounts')} style={box(t)}>
          {accs.map((a, i) => <AccountRow key={a.id} a={a} accounts={accs} last={i === accs.length - 1} />)}
        </View>
      ) : null}
      <View aria-label={tr('cloud.onThisComputer')} style={box(t)}>
        <BoxRow t={t}>
          <Text style={{ flex: 1, fontSize: typeScale.body, color: t.text, textAlign: 'start' }}>{tr('cloud.onThisComputer')}</Text>
          <Dim t={t}>{tr('cloud.cacheLine', { size: formatBytes(cache), kept: formatBytes(kept) })}</Dim>
          <Button small label={tr('cloud.freeUp')} disabled={!accs.length} onClick={freeAll} />
        </BoxRow>
        <BoxRow t={t}>
          <Text style={{ flex: 1, fontSize: typeScale.body, color: t.text, textAlign: 'start' }}>{tr('cloud.freeBelow')}</Text>
          <PopUp width={130} value={o.autoFree === false ? 0 : (o.freeBelowGb || 10)} onChange={(v) => cloud.setOptions(v ? { autoFree: true, freeBelowGb: v } : { autoFree: false })}
            options={[...FREE_BELOW.map((g) => [g, tr('cloud.gbFree', { count: g })]), ['-', ''], [0, tr('cloud.never')]]} />
        </BoxRow>
        <BoxRow t={t} last>
          <View style={{ flex: 1, minWidth: 0, gap: 2 }}>
            <Text style={{ fontSize: typeScale.body, color: t.text, textAlign: 'start' }}>{tr('cloud.service.title')}</Text>
            <Dim t={t}>{svcLine}</Dim>
          </View>
          <Button small label={tr('cloud.service.log')} onClick={() => cloud.showLog()} />
          <Button small label={tr('cloud.service.restart')} onClick={() => cloud.restart()} />
        </BoxRow>
      </View>
      {msg ? <Note tone={msg.ok ? 'ok' : 'error'}>{msg.text}</Note> : null}
      <Section title={tr('cloud.bandwidth')} footer={tr('cloud.bandwidthHint')}>
        <SettingRow label={tr('cloud.uploadLimit')}>
          <PopUp width={130} value={Math.round((o.bwUp || 0) / MB)} onChange={(v) => cloud.setOptions({ bwUp: v * MB })} options={LIMITS.map((v) => [v, limitLabel(v, tr)])} />
        </SettingRow>
        <SettingRow label={tr('cloud.downloadLimit')}>
          <PopUp width={130} value={Math.round((o.bwDown || 0) / MB)} onChange={(v) => cloud.setOptions({ bwDown: v * MB })} options={LIMITS.map((v) => [v, limitLabel(v, tr)])} />
        </SettingRow>
        <SettingRow label={tr('cloud.pauseOnBattery')}><Toggle small on={o.pauseOnBattery !== false} onChange={(v) => cloud.setOptions({ pauseOnBattery: v })} /></SettingRow>
        <SettingRow label={tr('cloud.pauseOnMetered')}><Toggle small on={o.pauseOnMetered !== false} onChange={(v) => cloud.setOptions({ pauseOnMetered: v })} /></SettingRow>
      </Section>
      <Section title={tr('cloud.about.title')} footer={tr('cloud.about.hint')}>
        <SettingRow label={svc.version ? tr('cloud.about.version', { version: svc.version }) : 'rclone'} hint={tr('cloud.about.license')}>
          <Button small label={tr('cloud.about.licenseButton')} onClick={() => openUrl(RCLONE_LICENSE)} />
          <Button small label="rclone.org" onClick={() => openUrl(RCLONE_SITE)} />
        </SettingRow>
      </Section>
    </Stack>
  );
}

// ---- the account's page (mock Account) ----

function SpaceCard({ a, accounts }) {
  const t = useTheme();
  const { t: tr } = useLocale();
  const u = M.usage(a.about);
  const color = M.accountColor(accounts, a.id);
  const parts = M.ringParts(a.about, color);
  if (!u) return <View style={[box(t), { padding: 18 }]}><Dim t={t}>{tr('cloud.space.unknown')}</Dim></View>;
  return (
    <View aria-label={tr('cloud.space.title')} style={[box(t), { padding: 18, flexDirection: 'row', gap: 22, alignItems: 'center' }]}>
      {parts ? (
        <View aria-label={fmtSpace(a.about, tr)} style={{ width: 116, height: 116, alignItems: 'center', justifyContent: 'center', flexShrink: 0 }}>
          <View style={{ position: 'absolute', left: 0, top: 0 }}><UsageRing size={116} thickness={3.4} parts={parts} /></View>
          <Text style={{ fontSize: 20, fontWeight: 700, color: t.text }}>{`${Math.round(u.pct * 100)}%`}</Text>
          <Text style={{ fontSize: typeScale.caption, color: t.textDim }}>{tr('cloud.space.ofTotal', { total: formatBytes(u.total) })}</Text>
        </View>
      ) : null}
      <View style={{ flex: 1, minWidth: 0, gap: 8 }}>
        <Text style={{ fontSize: typeScale.body, fontWeight: 600, color: t.text, textAlign: 'start' }}>
          {u.total != null ? tr('cloud.space.usedFree', { used: formatBytes(u.used), free: formatBytes(u.free) }) : tr('cloud.space.usedOnly', { used: formatBytes(u.used) })}</Text>
        {(parts || []).map((p) => (
          <View key={p.key} style={{ flexDirection: 'row', alignItems: 'center', gap: 8 }}>
            <View style={{ width: 10, height: 10, borderRadius: 3, backgroundColor: p.color }} />
            <Text style={{ flex: 1, fontSize: typeScale.body, color: t.text, textAlign: 'start' }}>{tr(`cloud.space.part.${p.key}`)}</Text>
            <Dim t={t}>{formatBytes(p.bytes)}</Dim>
          </View>
        ))}
        {!parts ? <Dim t={t}>{tr('cloud.space.noQuota')}</Dim> : null}
        {u.trashed > 0 ? <Text onClick={() => { const url = cloud.websiteUrl(a.id); if (url) openUrl(url); }} style={{ fontSize: typeScale.callout, color: t.accent, textAlign: 'start' }}>{tr('cloud.space.emptyTrash')}</Text> : null}
      </View>
    </View>
  );
}

function KeptList({ a, onAdd }) {
  const t = useTheme();
  const { t: tr } = useLocale();
  const [sizes, setSizes] = useState({});
  useEffect(() => {
    const id = setTimeout(() => {
      const s = {};
      for (const p of a.kept || []) { try { s[p] = fs.du(p, 300).size; } catch (e) { /* not there yet */ } }
      setSizes(s);
    }, 50);
    return () => clearTimeout(id);
  }, [(a.kept || []).join('|')]);
  return (
    <View aria-label={tr('cloud.keptTitle')} style={box(t)}>
      <BoxRow t={t}>
        <Text style={{ flex: 1, fontSize: typeScale.body, fontWeight: 600, color: t.text, textAlign: 'start' }}>{tr('cloud.keptTitle')}</Text>
        <Dim t={t}>{formatBytes(a.keptBytes || 0)}</Dim>
      </BoxRow>
      {(a.kept || []).map((p) => (
        <BoxRow key={p} t={t} pad={8}>
          <CloudGlyph kind="kept" size={16} />
          <Text style={{ flex: 1, minWidth: 0, fontSize: typeScale.body, color: t.text, whiteSpace: 'nowrap', overflow: 'hidden', textAlign: 'start' }}>{M.relPath(p, a) || a.name}</Text>
          {sizes[p] != null ? <Dim t={t}>{formatBytes(sizes[p])}</Dim> : null}
          <Button small plain label={tr('cloud.stopKeeping')} onClick={() => cloud.unkeep([p])} />
        </BoxRow>
      ))}
      <View onClick={onAdd} style={{ paddingHorizontal: 14, paddingVertical: 9 }}>
        <Text style={{ fontSize: typeScale.body, color: t.accent, textAlign: 'start' }}>{tr('cloud.addFolder')}</Text>
      </View>
    </View>
  );
}

function LinksList({ a }) {
  const t = useTheme();
  const { t: tr } = useLocale();
  const st = useCloud();
  const links = useMemo(() => cloud.links(a.id), [a.id, st]);
  if (!a.linksSupported) return null;
  const date = (d) => formatDate(d, 'short');
  return (
    <View aria-label={tr('cloud.links.title')} style={box(t)}>
      <BoxRow t={t} last={!links.length}>
        <Text style={{ flex: 1, fontSize: typeScale.body, fontWeight: 600, color: t.text, textAlign: 'start' }}>{tr('cloud.links.title')}</Text>
        <Dim t={t}>{tr('cloud.links.fromHere')}</Dim>
      </BoxRow>
      {links.map((l, i) => {
        const e = M.expiryLine(l, date);
        return (
          <BoxRow key={l.path} t={t} pad={8} last={i === links.length - 1}>
            <View style={{ flex: 1, minWidth: 0, gap: 1 }}>
              <Text style={{ fontSize: typeScale.body, color: t.text, whiteSpace: 'nowrap', overflow: 'hidden', textOverflow: 'ellipsis', textAlign: 'start' }}>{String(l.path).split('/').pop()}</Text>
              <Text style={{ fontSize: typeScale.caption, color: t.textDim, textAlign: 'start' }}>{tr(e.key, e.vars)}</Text>
            </View>
            <Text onClick={() => { clipboard.set(l.url); toast.set(tr('cloud.linkCopied')); }} style={{ fontSize: typeScale.body, color: t.accent }}>{tr('cloud.copy')}</Text>
            <Text onClick={() => cloud.unlink(l.path)} style={{ fontSize: typeScale.body, color: tint.error }}>{tr('cloud.stopSharing')}</Text>
          </BoxRow>
        );
      })}
    </View>
  );
}

function NameField({ a, accounts }) {
  const t = useTheme();
  const { t: tr } = useLocale();
  const [edit, setEdit] = useState(null);
  const [err, setErr] = useState('');
  const save = () => {
    const n = (edit || '').trim();
    if (n === a.name) { setEdit(null); return; }
    const bad = M.nameProblem(n, accounts, a.id);
    if (bad) { setErr(tr(`cloud.name.${bad}`)); return; }
    const r = cloud.rename(a.id, n);
    if (r && r.error) { setErr(errorText(r.error)); return; }
    setEdit(null); setErr('');
  };
  if (edit != null) {
    return (
      <View style={{ width: 150, gap: 2 }}>
        <TextInput autoFocus selectAll value={edit} onChange={(v) => { setEdit(v.slice(0, M.NAME_MAX)); setErr(''); }} onSubmit={save} onCancel={() => { setEdit(null); setErr(''); }} />
        {err ? <Text style={{ fontSize: typeScale.caption, color: tint.error, textAlign: 'start' }}>{err}</Text> : null}
      </View>
    );
  }
  return (
    <View onClick={() => setEdit(a.name)} style={{ paddingHorizontal: 10, paddingVertical: 4, borderRadius: 6, backgroundColor: t.control }}>
      <Text style={{ fontSize: typeScale.callout, color: t.text }}>{a.name}</Text>
    </View>
  );
}

function Options({ a, accounts }) {
  const t = useTheme();
  const { t: tr } = useLocale();
  const onDemand = !(a.kept || []).includes(a.mount);
  const own = cloud.accountOptionsSupported();
  const ao = a.options || {};
  const row = (label, hint, ctl, last, off) => (
    <BoxRow t={t} last={last}>
      <View style={{ flex: 1, minWidth: 0, gap: 2, opacity: off ? 0.45 : 1 }}>
        <Text style={{ fontSize: typeScale.body, color: t.text, textAlign: 'start' }}>{label}</Text>
        {hint ? <Text style={{ fontSize: typeScale.caption, color: t.textDim, textAlign: 'start' }}>{hint}</Text> : null}
      </View>
      <View style={{ opacity: off ? 0.45 : 1 }}>{ctl}</View>
    </BoxRow>
  );
  return (
    <View aria-label={tr('cloud.options')} style={box(t)}>
      {row(tr('cloud.opt.folder'), `~/Cloud/${a.name}`, <NameField a={a} accounts={accounts} />)}
      {row(tr('cloud.opt.onDemand'), tr('cloud.opt.onDemandHint'), <Toggle small on={onDemand} onChange={(v) => (v ? cloud.unkeep([a.mount]) : cloud.keep([a.mount]))} />)}
      {row(tr('cloud.opt.pause'), tr('cloud.opt.pauseHint'), <Toggle small on={a.state === 'paused'} onChange={(v) => (v ? cloud.pause(a.id) : cloud.resume(a.id))} />)}
      {row(tr('cloud.opt.unmetered'), own ? '' : tr('cloud.opt.later'),
        <Toggle small on={!!ao.unmeteredOnly} onChange={(v) => own && cloud.setAccountOptions(a.id, { unmeteredOnly: v })} />, false, !own)}
      {row(tr('cloud.opt.uploadLimit'), tr('cloud.opt.thisAccount'),
        <PopUp width={110} disabled={!own} value={Math.round((ao.bwUp || 0) / MB)} onChange={(v) => cloud.setAccountOptions(a.id, { bwUp: v * MB })} options={LIMITS.map((v) => [v, limitLabel(v, tr)])} />, true, !own)}
    </View>
  );
}

// Advanced → Use my own client ID (section 6.2): signs in again with the person's own OAuth app.
function OwnClient({ a }) {
  const t = useTheme();
  const { t: tr } = useLocale();
  const [open, setOpen] = useState(false);
  const [id, setId] = useState('');
  const [secret, setSecret] = useState('');
  return (
    <View style={[box(t), { paddingHorizontal: 14, paddingVertical: 10, gap: 8 }]}>
      <View onClick={() => setOpen(!open)} style={{ flexDirection: 'row', alignItems: 'center', gap: 6 }}>
        <Text style={{ fontSize: 10, color: t.textDim }}>{open ? '▼' : direction() === 'rtl' ? '◀' : '▶'}</Text>
        <Text style={{ fontSize: typeScale.body, color: t.text, textAlign: 'start' }}>{tr('cloud.advanced')}</Text>
      </View>
      {open ? (
        <View style={{ gap: 6 }}>
          <Text style={{ fontSize: typeScale.body, fontWeight: 600, color: t.text, textAlign: 'start' }}>{tr('cloud.ownClient.title')}</Text>
          <Dim t={t}>{tr('cloud.ownClient.hint')}</Dim>
          <TextInput value={id} onChange={setId} placeholder={tr('cloud.ownClient.id')} style={{ direction: 'ltr' }} />
          <TextInput secure value={secret} onChange={setSecret} placeholder={tr('cloud.ownClient.secret')} style={{ direction: 'ltr' }} />
          <Actions>
            <Button small label={tr('cloud.signInAgain')} disabled={!id.trim()} onClick={() => signInAgain(a, { clientId: id.trim(), clientSecret: secret })} />
          </Actions>
        </View>
      ) : null}
    </View>
  );
}

function providerOf(kind) { return cloud.providers().find((p) => p.kind === kind) || null; }

// Sign In Again: the browser for an OAuth account, the credentials form for the others (the same name updates it).
function signInAgain(a, own = {}) {
  const p = providerOf(a.kind);
  if (!p || p.group === 'oauth') {
    const r = cloud.beginSignIn(a.kind, a.name, own);
    setView({ sheet: { kind: 'signin', provider: p || { kind: a.kind, label: a.provider }, name: a.name, error: r && r.error } });
    return;
  }
  setView({ sheet: { kind: 'cred', provider: p, name: a.name, again: true } });
}

function AccountPage({ a, st }) {
  const t = useTheme();
  const { t: tr } = useLocale();
  const accs = st.accounts || [];
  const [picking, setPicking] = useState(false);
  const win = useAppWindow();
  const website = useMemo(() => cloud.websiteUrl(a.id), [a.id]);
  const last = (st.recent || []).find((r) => r.account === a.id || M.accountOf(r.path, [a]));
  const p = providerOf(a.kind);
  const line = M.stateLine(a);
  const wide = (wm.frame(wm.get(win.id) || {}).w || 880) >= 860;
  const side = (
    <View aria-label={tr('cloud.options')} style={{ width: wide ? 280 : undefined, gap: 12 }}>
      <Options a={a} accounts={accs} />
      <View style={[box(t), { paddingHorizontal: 14, paddingVertical: 12, gap: 4 }]}>
        <Dim t={t}>{a.provider || a.kind}</Dim>
        <Dim t={t} style={{ color: toneColor(t, line.tone) }}>{tr(line.key, line.vars)}</Dim>
        <Dim t={t}>{tr('cloud.inKeyring')}</Dim>
        <Dim t={t}>{last ? tr('cloud.lastSynced', { when: (() => { const g = M.ago(last.at); return tr(g.key, g.vars); })() }) : tr('cloud.notSyncedYet')}</Dim>
        {a.message && a.state === 'error' ? <Dim t={t} style={{ color: tint.warn }}>{errorText(a.message)}</Dim> : null}
      </View>
      <Button label={tr('cloud.signInAgain')} onClick={() => signInAgain(a)} />
      <View onClick={() => setView({ sheet: { kind: 'remove', id: a.id } })} style={{ height: 30, borderRadius: t.controlRadius, alignItems: 'center', justifyContent: 'center', backgroundColor: '#7a2230' }}>
        <Text style={{ fontSize: typeScale.body, fontWeight: 600, color: '#fff' }}>{tr('cloud.remove')}</Text>
      </View>
      <Dim t={t} style={{ fontSize: typeScale.caption }}>{tr('cloud.removeHint')}</Dim>
      {p && p.group === 'oauth' ? <OwnClient a={a} /> : null}
    </View>
  );
  return (
    <Stack gap={16}>
      <View style={{ flexDirection: 'row', alignItems: 'center', gap: 12 }}>
        <View aria-label={tr('cloud.back')} onClick={() => setView({ page: null, sheet: null })} style={{ width: 28, height: 28, borderRadius: 7, alignItems: 'center', justifyContent: 'center', backgroundColor: t.control }}>
          <Text style={{ fontSize: 16, color: t.text }}>{direction() === 'rtl' ? '›' : '‹'}</Text>
        </View>
        <Text style={{ flex: 1, fontSize: typeScale.title, fontWeight: 600, color: t.text, textAlign: 'start' }}>{a.name}</Text>
        {website ? <Button label={tr('cloud.openWebsite')} onClick={() => openUrl(website)} /> : null}
        <Button label={tr('cloud.showInFiles')} onClick={() => wm.open('files', { new: true, args: { path: a.mount, nonce: Date.now() } })} />
      </View>
      <View style={{ flexDirection: wide ? 'row' : 'column', gap: 20, alignItems: 'flex-start' }}>
        <View style={{ flex: wide ? 1 : undefined, minWidth: 0, alignSelf: 'stretch', gap: 16 }}>
          <SpaceCard a={a} accounts={accs} />
          <KeptList a={a} onAdd={() => setPicking(true)} />
          <LinksList a={a} />
        </View>
        {wide ? side : <View style={{ alignSelf: 'stretch' }}>{side}</View>}
      </View>
      {picking ? (() => {
        const g = wm.frame(wm.get(win.id) || {});
        return (
          <Layer name="modal">
            <View style={{ position: 'absolute', left: g.x || 0, top: g.y || 0, width: g.w || 880, height: g.h || 600 }}>
              <FilePicker mode="folder" start={a.mount} title={tr('cloud.addFolderTitle', { name: a.name })} onClose={() => setPicking(false)}
                onPick={(p2) => { setPicking(false); const path = [].concat(p2)[0]; if (path && M.accountOf(path, [a])) cloud.keep([path]); }} />
            </View>
          </Layer>
        );
      })() : null}
    </Stack>
  );
}

// ---- Add a Cloud Account (mock AddAccount) ----

function ProviderTile({ p, on, onClick, more }) {
  const t = useTheme();
  const { t: tr } = useLocale();
  const hint = more != null ? tr('cloud.add.more', { count: more }) : p.hint;
  return (
    <View onClick={onClick} style={{ flex: 1, minWidth: 0, height: 54, flexDirection: 'row', alignItems: 'center', gap: 10, paddingHorizontal: 12, borderRadius: 10,
      backgroundColor: on ? (t.mode === 'dark' ? '#16335a' : '#dcebff') : t.mode === 'dark' ? '#1e1e22' : '#f4f4f6', border: `1px solid ${on ? t.accent : t.separator}` }}>
      <View style={{ width: 30, height: 30, borderRadius: 8, alignItems: 'center', justifyContent: 'center', backgroundColor: more != null ? '#636366' : M.providerColor(p), flexShrink: 0 }}>
        <Text style={{ fontSize: 13, fontWeight: 700, color: '#fff' }}>{more != null ? '…' : p.group === 'crypt' ? '◆' : M.providerMark(p.label)}</Text>
      </View>
      <View style={{ flex: 1, minWidth: 0, gap: 1 }}>
        <Text style={{ fontSize: typeScale.body, fontWeight: 600, color: t.text, whiteSpace: 'nowrap', overflow: 'hidden', textAlign: 'start' }}>{more != null ? tr('cloud.add.moreTitle') : p.label}</Text>
        <Text style={{ fontSize: typeScale.caption, color: t.textDim, whiteSpace: 'nowrap', overflow: 'hidden', textAlign: 'start' }}>{hint || ''}</Text>
      </View>
    </View>
  );
}

const defaultName = (accounts, p, tr) => {
  for (const n of [tr('cloud.add.personal'), tr('cloud.add.work'), p ? p.label : '']) if (n && !M.nameProblem(n, accounts)) return n;
  for (let i = 2; i < 50; i++) { const n = `${p ? p.label : tr('cloud.title')} ${i}`; if (!M.nameProblem(n, accounts)) return n; }
  return '';
};

function AddSheet({ st }) {
  const t = useTheme();
  const { t: tr } = useLocale();
  const providers = useMemo(() => cloud.providers(), []);
  const [q, setQ] = useState('');
  const [expanded, setExpanded] = useState([]);
  const [sel, setSel] = useState(null);
  const [name, setName] = useState(() => defaultName(st.accounts, null, tr));
  const [named, setNamed] = useState(false);          // the person typed a name: picking a provider keeps it
  const [err, setErr] = useState('');
  const groups = M.groupProviders(providers, q, expanded);
  const p = providers.find((x) => x.kind === sel);
  const close = () => setView({ sheet: null });
  const go = () => {
    if (!p) return;
    const bad = M.nameProblem(name, st.accounts);
    if (bad) { setErr(tr(`cloud.name.${bad}`)); return; }
    if (p.group === 'oauth') {
      const r = cloud.beginSignIn(p.kind, name.trim(), {});
      setView({ sheet: { kind: 'signin', provider: p, name: name.trim(), error: r && r.error } });
    } else setView({ sheet: { kind: 'cred', provider: p, name: name.trim() } });
  };
  const rows = (items) => { const out = []; for (let i = 0; i < items.length; i += 3) out.push(items.slice(i, i + 3)); return out; };
  return (
    <Sheet width={640} top={40} onClose={close} label={tr('cloud.add.title')}>
      <View style={{ paddingHorizontal: 22, paddingTop: 20, paddingBottom: 10, gap: 6 }}>
        <Text style={{ fontSize: typeScale.title, fontWeight: 600, color: t.text, textAlign: 'start' }}>{tr('cloud.add.title')}</Text>
        <Dim t={t} style={{ fontSize: typeScale.body }}>{tr('cloud.add.subtitle')}</Dim>
      </View>
      <View style={{ paddingHorizontal: 22, paddingTop: 6, paddingBottom: 10 }}>
        <SearchField value={q} onChange={setQ} placeholder={tr('cloud.add.search', { count: providers.length })} autoFocus />
      </View>
      <ScrollView style={{ maxHeight: 380 }}>
        <View style={{ paddingBottom: 6 }}>
          {!providers.length ? <Dim t={t} style={{ paddingHorizontal: 22 }}>{tr('cloud.unavailable')}</Dim> : null}
          {providers.length && !groups.length ? <Dim t={t} style={{ paddingHorizontal: 22 }}>{tr('cloud.add.none')}</Dim> : null}
          {groups.map((g) => {
            const items = [...g.items.map((x) => ({ p: x })), ...(g.more ? [{ more: g.more, group: g.group }] : [])];
            return (
              <View key={g.group}>
                <Text style={{ paddingHorizontal: 22, paddingTop: 8, paddingBottom: 4, fontSize: 11, fontWeight: 600, color: t.textDim, textTransform: 'uppercase', textAlign: 'start' }}>{tr(`cloud.group.${g.group}`)}</Text>
                <View style={{ paddingHorizontal: 22, paddingBottom: 8, gap: 8 }}>
                  {rows(items).map((r, i) => (
                    <View key={i} style={{ flexDirection: 'row', gap: 8 }}>
                      {r.map((it) => (it.more != null
                        ? <ProviderTile key="more" more={it.more} onClick={() => setExpanded([...expanded, it.group])} />
                        : <ProviderTile key={it.p.kind} p={it.p} on={sel === it.p.kind} onClick={() => { setSel(it.p.kind); setErr(''); if (!named) setName(defaultName(st.accounts, it.p, tr)); }} />))}
                      {r.length < 3 ? [...Array(3 - r.length)].map((_, k) => <View key={`f${k}`} style={{ flex: 1 }} />) : null}
                    </View>
                  ))}
                </View>
              </View>
            );
          })}
        </View>
      </ScrollView>
      <View style={{ flexDirection: 'row', alignItems: 'center', gap: 8, paddingHorizontal: 22, paddingTop: 12, paddingBottom: 18, borderTopWidth: 1, borderTopColor: t.separator, marginTop: 6 }}>
        <Text style={{ fontSize: typeScale.callout, color: t.textDim }}>{tr('cloud.accountName')}</Text>
        <View style={{ width: 180 }}><TextInput value={name} onChange={(v) => { setName(v.slice(0, M.NAME_MAX)); setNamed(true); setErr(''); }} onSubmit={go} /></View>
        <Text style={{ flex: 1, fontSize: typeScale.caption, color: tint.error, textAlign: 'start' }}>{err}</Text>
        <Button label={tr('cloud.cancel')} onClick={close} />
        <Button primary label={tr('cloud.continue')} disabled={!p} onClick={go} />
      </View>
    </Sheet>
  );
}

// ---- a server's or a bucket's credentials (mock Credentials) ----

// a field's label: ours for the usual rclone option names, else the provider list's own
const fieldLabel = (f) => tOptional(`cloud.field.${f.name}`) || f.label || f.name;

function CredSheet({ st, sheet }) {
  const t = useTheme();
  const { t: tr } = useLocale();
  const p = sheet.provider;
  const [vals, setVals] = useState(() => M.formValues(p));
  const [name, setName] = useState(sheet.name || '');
  const [err, setErr] = useState(null);       // { text, field }
  const [busy, setBusy] = useState(false);
  const back = () => setView({ sheet: sheet.again ? null : { kind: 'add' } });
  const connect = () => {
    const miss = M.missingField(p, vals);
    if (miss) { setErr({ text: tr('cloud.field.required'), field: miss }); return; }
    const bad = sheet.again ? null : M.nameProblem(name, st.accounts);
    if (bad) { setErr({ text: tr(`cloud.name.${bad}`), field: '__name' }); return; }
    setBusy(true);
    const r = cloud.add(p.kind, name.trim(), M.cleanFields(p, vals));
    setBusy(false);
    if (r && r.error) { setErr({ text: errorText(r.error), field: r.field || null }); return; }
    setView({ page: r && r.id ? r.id : null, sheet: null });
  };
  const note = p.group === 'object' ? tr('cloud.cred.noteObject') : p.group === 'crypt' ? tr('cloud.cred.noteCrypt') : tr('cloud.cred.noteServer');
  const field = (key, label, input) => (
    <View key={key} style={{ flexDirection: 'row', alignItems: 'center', gap: 10 }}>
      <Text style={{ width: 120, fontSize: typeScale.body, color: t.textDim, textAlign: 'end' }}>{label}</Text>
      <View style={{ flex: 1, minWidth: 0, borderRadius: 8, border: err && err.field === key ? `1px solid ${tint.error}` : '1px solid transparent' }}>{input}</View>
    </View>
  );
  return (
    <Sheet width={460} top={40} onClose={back} label={p.label}>
      <View style={{ flexDirection: 'row', alignItems: 'center', gap: 10, paddingHorizontal: 20, paddingTop: 18, paddingBottom: 8 }}>
        <View style={{ width: 34, height: 34, borderRadius: 9, alignItems: 'center', justifyContent: 'center', backgroundColor: M.providerColor(p) }}>
          <Text style={{ fontSize: 13, fontWeight: 700, color: '#fff' }}>{p.group === 'crypt' ? '◆' : M.providerMark(p.label)}</Text>
        </View>
        <View style={{ flex: 1, minWidth: 0 }}>
          <Text style={{ fontSize: typeScale.headline, fontWeight: 600, color: t.text, textAlign: 'start' }}>{p.label}</Text>
          {p.hint ? <Dim t={t}>{p.hint}</Dim> : null}
        </View>
      </View>
      <View style={{ paddingHorizontal: 20, paddingVertical: 8, gap: 10 }}>
        {(p.fields || []).map((f) => field(f.name, fieldLabel(f),
          // a choice among the provider's examples (a preset, a service), else a text field; secrets masked
          !f.secret && (f.examples || []).length > 1
            ? <PopUp width={250} value={vals[f.name] || String(f.examples[0])} onChange={(v) => { setVals({ ...vals, [f.name]: v }); setErr(null); }}
              options={[...new Set([...(f.examples || []).map(String), ...(vals[f.name] ? [vals[f.name]] : [])])].map((x) => [x, x])} />
            : <TextInput secure={!!f.secret} value={vals[f.name] || ''} placeholder={f.examples && f.examples[0] ? String(f.examples[0]) : ''} style={{ direction: 'ltr' }}
              onChange={(v) => { setVals({ ...vals, [f.name]: v }); setErr(null); }} onSubmit={connect} />))}
        {sheet.again ? null : field('__name', tr('cloud.accountName'), <TextInput value={name} onChange={(v) => { setName(v.slice(0, M.NAME_MAX)); setErr(null); }} onSubmit={connect} />)}
      </View>
      <View style={{ marginHorizontal: 20, marginTop: 4, paddingHorizontal: 12, paddingVertical: 10, borderRadius: 8, backgroundColor: t.mode === 'dark' ? '#1e1e22' : '#f4f4f6' }}>
        <Dim t={t}>{note}</Dim>
      </View>
      {err ? <Text style={{ marginHorizontal: 20, marginTop: 8, fontSize: typeScale.callout, color: tint.error, textAlign: 'start' }}>{err.text}</Text> : null}
      <View style={{ flexDirection: 'row', justifyContent: 'flex-end', gap: 8, paddingHorizontal: 20, paddingTop: 14, paddingBottom: 18 }}>
        <Button label={tr('cloud.back')} onClick={back} />
        <Button primary label={tr(busy ? 'cloud.connecting' : 'cloud.connect')} disabled={busy} onClick={connect} />
      </View>
    </Sheet>
  );
}

// ---- signing in with the browser (mock SignIn) ----

function SignInSheet({ st, sheet }) {
  const t = useTheme();
  const { t: tr } = useLocale();
  const [, tick] = useState(0);
  const [showInFiles, setShowInFiles] = useState(true);
  const [keepAll, setKeepAll] = useState(false);
  const [s, set] = useSettings();
  useEffect(() => { const id = setInterval(() => tick((x) => x + 1), 1000); return () => clearInterval(id); }, []);
  const si = st.signin && st.signin.name === sheet.name ? st.signin : null;
  const p = sheet.provider || {};
  const expired = si && si.phase === 'waiting' && M.toMs(si.expiresAt) <= Date.now();
  const failed = sheet.error || (si && si.phase === 'failed') || expired;
  const cancel = () => { if (si && si.phase === 'waiting') cloud.cancelSignIn(si.id); setView({ sheet: null }); };
  if (si && si.phase === 'done') {
    const a = (st.accounts || []).find((x) => x.id === si.id || x.name === si.name);
    const finish = () => {
      if (a) {
        if (keepAll) cloud.keep([a.mount]);
        const hidden = s.cloudHidden || [];
        set({ cloudHidden: showInFiles ? hidden.filter((x) => x !== a.id) : [...new Set([...hidden, a.id])] });
      }
    };
    return (
      <Sheet width={360} onClose={() => { finish(); setView({ sheet: null }); }} label={tr('cloud.ready', { name: si.name })}>
        <View style={{ padding: 26, gap: 12 }}>
          <View style={{ flexDirection: 'row', alignItems: 'center', gap: 10 }}>
            <View style={{ width: 32, height: 32, borderRadius: 16, backgroundColor: '#30d158', alignItems: 'center', justifyContent: 'center' }}>
              <Text style={{ fontSize: 16, fontWeight: 700, color: '#fff' }}>✓</Text>
            </View>
            <Text style={{ flex: 1, fontSize: typeScale.headline, fontWeight: 600, color: t.text, textAlign: 'start' }}>{tr('cloud.ready', { name: si.name })}</Text>
          </View>
          <Dim t={t} style={{ fontSize: typeScale.body }}>{`${a && a.about ? `${fmtSpace(a.about, tr)}. ` : ''}${tr('cloud.readyBody')}`}</Dim>
          <View style={{ flexDirection: 'row', alignItems: 'center', gap: 10 }}><Toggle small on={showInFiles} onChange={setShowInFiles} /><Text style={{ fontSize: typeScale.body, color: t.text }}>{tr('cloud.showInFiles')}</Text></View>
          <View style={{ flexDirection: 'row', alignItems: 'center', gap: 10 }}><Toggle small on={keepAll} onChange={setKeepAll} /><Text style={{ fontSize: typeScale.body, color: t.text }}>{tr('cloud.keepEverything')}</Text></View>
          <Button primary label={tr('cloud.openInFiles')} onClick={() => { finish(); setView({ page: a ? a.id : null, sheet: null }); if (a) wm.open('files', { new: true, args: { path: a.mount, nonce: Date.now() } }); }} />
        </View>
      </Sheet>
    );
  }
  const left = si ? M.countdown(si.expiresAt) : '';
  return (
    <Sheet width={440} onClose={cancel} label={tr('cloud.signin.title', { provider: p.label || '' })}>
      <View style={{ padding: 28, gap: 14, alignItems: 'center' }}>
        <View style={{ width: 72, height: 72, alignItems: 'center', justifyContent: 'center' }}>
          <View style={{ position: 'absolute', left: 0, top: 0 }}><UsageRing size={72} thickness={1.4} parts={failed ? [] : [{ frac: 0.25, color: t.accent }]} /></View>
          <CloudGlyph kind="cloud" size={30} color={failed ? tint.warn : t.accent} />
        </View>
        <Text style={{ fontSize: typeScale.title, fontWeight: 600, color: t.text, textAlign: 'center' }}>{tr(failed ? 'cloud.signin.failedTitle' : 'cloud.signin.title', { provider: p.label || '' })}</Text>
        <Dim t={t} style={{ fontSize: typeScale.body, textAlign: 'center', lineHeight: 1.5 }}>
          {failed ? (sheet.error ? errorText(sheet.error) : si && si.error ? errorText(si.error) : tr('cloud.signin.expired')) : tr('cloud.signin.body', { name: sheet.name })}</Dim>
        {!failed && left ? <Dim t={t}>{tr('cloud.signin.left', { time: left })}</Dim> : null}
        <View style={{ flexDirection: 'row', gap: 8 }}>
          <Button label={tr('cloud.cancel')} onClick={cancel} />
          {failed
            ? <Button primary label={tr('cloud.signin.tryAgain')} onClick={() => { const r = cloud.beginSignIn(p.kind, sheet.name, {}); setView({ sheet: { ...sheet, error: r && r.error } }); }} />
            : <Button primary label={tr('cloud.signin.again')} disabled={!si} onClick={() => si && cloud.openSignInPage(si.id)} />}
        </View>
        <Dim t={t} style={{ fontSize: typeScale.caption, textAlign: 'center', lineHeight: 1.5 }}>{tr('cloud.signin.rclone')}</Dim>
        <Dim t={t} style={{ fontSize: typeScale.caption, textAlign: 'center', lineHeight: 1.5 }}>{tr('cloud.signin.local')}</Dim>
      </View>
    </Sheet>
  );
}

// ---- Remove Account… ----

function RemoveSheet({ st, sheet }) {
  const t = useTheme();
  const { t: tr } = useLocale();
  const a = (st.accounts || []).find((x) => x.id === sheet.id);
  const [pending, setPending] = useState(null);
  const [err, setErr] = useState('');
  const close = () => setView({ sheet: null });
  if (!a) return null;
  const go = (discardPending) => {
    const r = cloud.remove(a.id, { discardPending });
    if (r && r.error === 'pending') { setPending(r.files || 1); return; }
    if (r && r.error) { setErr(errorText(r.error)); return; }
    setView({ page: null, sheet: null });
  };
  return (
    <Sheet width={380} onClose={close} label={tr('cloud.removeTitle', { name: a.name })}>
      <View style={{ padding: spacing.xl, gap: spacing.s }}>
        <Text style={{ fontSize: typeScale.headline, fontWeight: 600, color: t.text, textAlign: 'start' }}>{tr('cloud.removeTitle', { name: a.name })}</Text>
        <Dim t={t} style={{ fontSize: typeScale.body }}>{pending ? tr('cloud.removePending', { count: Number(pending) || 1 }) : tr('cloud.removeHint')}</Dim>
        {err ? <Text style={{ fontSize: typeScale.callout, color: tint.error, textAlign: 'start' }}>{err}</Text> : null}
        <View style={{ flexDirection: 'row', justifyContent: 'flex-end', gap: spacing.s, marginTop: spacing.m }}>
          <Button label={tr('cloud.cancel')} onClick={close} />
          <View onClick={() => go(!!pending)} style={{ height: 28, paddingHorizontal: 14, borderRadius: t.controlRadius, justifyContent: 'center', backgroundColor: '#e5483f' }}>
            <Text style={{ fontSize: typeScale.body, fontWeight: 500, color: '#fff' }}>{tr(pending ? 'cloud.removeAnyway' : 'cloud.removeOk')}</Text>
          </View>
        </View>
      </View>
    </Sheet>
  );
}

// ---- the page ----

// The title bar's button (Settings' header, beside "Cloud"), on the overview only.
export function CloudHeaderAction() {
  const { t: tr } = useLocale();
  const v = useStore(cloudView);
  if (v.page || !cloud.available()) return null;
  return <Button primary small label={tr('cloud.addAccount')} onClick={() => setView({ sheet: { kind: 'add' } })} />;
}

export function CloudSettings({ page }) {
  const t = useTheme();
  const { t: tr } = useLocale();
  const st = useCloud();
  const v = useStore(cloudView);
  // another app opened an account's page (wm.open('settings', { args: { section: 'cloud', page: id } }))
  useEffect(() => { if (page) setView({ page, sheet: null }); }, [page]);
  if (!cloud.available() || !st) {
    return <Stack><Note tone="warn">{tr('cloud.unavailable')}</Note></Stack>;
  }
  const a = v.page ? (st.accounts || []).find((x) => x.id === v.page) : null;
  const sh = v.sheet;
  return (
    <View>
      {a ? <AccountPage a={a} st={st} /> : <Overview st={st} />}
      {sh && sh.kind === 'add' ? <AddSheet st={st} /> : null}
      {sh && sh.kind === 'cred' ? <CredSheet st={st} sheet={sh} /> : null}
      {sh && sh.kind === 'signin' ? <SignInSheet st={st} sheet={sh} /> : null}
      {sh && sh.kind === 'remove' ? <RemoveSheet st={st} sheet={sh} /> : null}
    </View>
  );
}
