// Settings → Cloud (docs/CLOUD.md sections 6, 8, 12): the accounts with their space, what Cloud keeps on this
// computer, bandwidth, the service and About rclone; the sheets to add an account (providers, sign-in in the
// browser, a server's or a bucket's credentials) and an account's own page. Built from the approved mocks (Main,
// AddAccount, SignIn, Credentials, Account). Everything goes through cloud (sdk/cloud.js); nothing here talks to
// rclone.
import {
  View, Text, Button, Toggle, TextInput, SearchField, Segmented, ScrollView, useTheme, useLocale, useStore, useCloud, cloud, cloudModel, createStore,
  formatBytes, formatDate, formatTime, tOptional, spacing, typeScale, wm, Layer, useAppWindow, FilePicker, openUrl, clipboard, toast, addListener, direction, errorText, fs,
  AccountBadge, UsageRing, SpaceBar, CloudGlyph, useSettings, systemInfo, lordosVersion,
} from '@lordos/sdk';
import { useEffect, useMemo, useRef, useState } from 'react';
import { Stack, Section, SettingRow, Note, Actions, PopUp, tint } from './ui.jsx';
import { runCheck, buildReport, issueUrl, aboutFields, checkWorks, CHECK_STEPS } from '../../sdk/cloudcheck.js';
import CLOUD_PROVIDERS from '../../assets/cloud/providers.json';

const M = cloudModel;
// the page and the open sheet survive the header's button and a remount: { page: account id | null, sheet }
export const cloudView = createStore({ page: null, sheet: null });
const setView = (p) => cloudView.set(p);

const RCLONE_SITE = 'https://rclone.org/';
const RCLONE_LICENSE = 'https://github.com/rclone/rclone/blob/master/COPYING';
const MIB = 1024;                                       // lord.cloud's bwUp/bwDown are KiB/s (17.1)
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

// A small turning ring (a spinner) while lord-server answers.
function Spinner({ size = 28 }) {
  const t = useTheme();
  const [a, setA] = useState(0);
  useEffect(() => { const id = setInterval(() => setA((x) => (x + 30) % 360), 80); return () => clearInterval(id); }, []);
  return <View style={{ width: size, height: size, transform: `rotate(${a}deg)` }}><UsageRing size={size} thickness={2.4} parts={[{ frac: 0.28, color: t.accent }]} /></View>;
}

// The providers as rclone last listed them in this session (17.2: asked fresh each time the sheet opens; kept here only
// so an account's page knows its provider's group and fields)
const providerCache = createStore([]);
const loadProviders = () => cloud.providers().then((l) => { providerCache.set(() => l); return l; });
function providerOf(kind, a) {
  return providerCache.get().find((p) => p.kind === kind) || (a ? { kind: a.kind, label: a.provider || a.kind, group: a.group, fields: [] } : null);
}

// Measure (17.2): a provider without a quota is measured on demand; one at a time per account, it can be stopped
const measuring = new Map();       // account id → { error? }
const measureRev = createStore(0);
const startMeasure = (id) => {
  measuring.set(id, {});
  measureRev.set((x) => x + 1);
  cloud.measure(id).then((r) => { if (measuring.has(id)) { if (r && r.error) measuring.set(id, { error: r.error, done: true }); else measuring.delete(id); measureRev.set((x) => x + 1); } });
};
const stopMeasure = (id) => { cloud.cancelMeasure(id); measuring.delete(id); measureRev.set((x) => x + 1); };
function MeasureRow({ a }) {
  const t = useTheme();
  const { t: tr } = useLocale();
  useStore(measureRev);
  if (!M.canMeasure(a.about)) return null;
  const m = measuring.get(a.id);
  const line = M.measuredLine(a.about, formatBytes, (d) => `${formatDate(d, 'short')} ${formatTime(d)}`);
  return (
    <View style={{ gap: 4 }}>
      {m && !m.done ? (
        <View style={{ flexDirection: 'row', alignItems: 'center', gap: 8 }}>
          <Spinner size={16} />
          <Dim t={t} style={{ flex: 1 }}>{tr('cloud.measure.running')}</Dim>
          <Button small label={tr('cloud.cancel')} onClick={() => stopMeasure(a.id)} />
        </View>
      ) : (
        <View style={{ flexDirection: 'row', alignItems: 'center', gap: 8 }}>
          <Dim t={t} style={{ flex: 1 }}>{line ? tr(line.key, line.vars) : tr('cloud.measure.hint')}</Dim>
          <Button small label={tr(line ? 'cloud.measure.again' : 'cloud.measure.run')} onClick={() => startMeasure(a.id)} />
        </View>
      )}
      {m && m.error ? <Text style={{ fontSize: typeScale.caption, color: tint.error, textAlign: 'start' }}>{errorText(m.error)}</Text> : null}
    </View>
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
        <Text style={{ fontSize: typeScale.caption, color: t.textDim, textAlign: 'end' }}>{(() => {
          const m = M.measuredLine(a.about, formatBytes, (d) => formatDate(d, 'short'));
          return m ? tr(m.key, m.vars) : fmtSpace(a.about, tr);
        })()}</Text>
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
          <PopUp width={130} value={Math.round((o.bwUp || 0) / MIB)} onChange={(v) => cloud.setOptions({ bwUp: v * MIB })} options={LIMITS.map((v) => [v, limitLabel(v, tr)])} />
        </SettingRow>
        <SettingRow label={tr('cloud.downloadLimit')}>
          <PopUp width={130} value={Math.round((o.bwDown || 0) / MIB)} onChange={(v) => cloud.setOptions({ bwDown: v * MIB })} options={LIMITS.map((v) => [v, limitLabel(v, tr)])} />
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
  if (!u) return <View style={[box(t), { padding: 18, gap: 8 }]}><Dim t={t}>{tr('cloud.space.unknown')}</Dim><MeasureRow a={a} /></View>;
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
        {!parts ? <MeasureRow a={a} /> : null}
        {a.trashSupported
          ? <Actions start><Button small label={tr('cloud.trash.empty')} onClick={() => setView({ sheet: { kind: 'emptyTrash', id: a.id } })} /></Actions>
          : u.trashed > 0 ? <Text onClick={() => { const url = cloud.websiteUrl(a.id); if (url) openUrl(url); }} style={{ fontSize: typeScale.callout, color: t.accent, textAlign: 'start' }}>{tr('cloud.space.emptyTrash')}</Text> : null}
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
      {row(tr('cloud.opt.pause'), tr('cloud.opt.pauseHint'), <Toggle small on={a.state === 'paused' || !!a.paused} onChange={(v) => (v ? cloud.pause(a.id) : cloud.resume(a.id))} />, !own)}
      {/* an account's own options only where lord-server has them (not in 0.8.8.5: docs/CLOUD.md 17.1) */}
      {own ? row(tr('cloud.opt.unmetered'), '', <Toggle small on={!!ao.unmeteredOnly} onChange={(v) => cloud.setAccountOptions(a.id, { unmeteredOnly: v })} />) : null}
      {own ? row(tr('cloud.opt.uploadLimit'), tr('cloud.opt.thisAccount'),
        <PopUp width={110} value={Math.round((ao.bwUp || 0) / MIB)} onChange={(v) => cloud.setAccountOptions(a.id, { bwUp: v * MIB })} options={LIMITS.map((v) => [v, limitLabel(v, tr)])} />, true) : null}
    </View>
  );
}

// The choice for a browser sign-in (docs/CLOUD.md 19): rclone's app (the default) or the person's own app, its client ID
// and secret (to beginSignIn's { clientId, clientSecret }, kept in the keyring), why, and rclone's page on making one.
function OwnAppChoice({ p, value, onChange }) {
  const t = useTheme();
  const { t: tr } = useLocale();
  const v = value || { own: false, id: '', secret: '' };
  const set = (o) => onChange({ ...v, ...o });
  return (
    <View style={{ gap: 6 }}>
      <Segmented small fill value={v.own ? 'own' : 'rclone'} onChange={(x) => set({ own: x === 'own' })}
        options={[['rclone', tr('cloud.ownApp.rclone')], ['own', tr('cloud.ownApp.own')]]} />
      {v.own ? (
        <View style={{ gap: 6 }}>
          <Dim t={t}>{tr('cloud.ownApp.hint', { redirect: M.OAUTH_REDIRECT })}</Dim>
          <TextInput value={v.id} onChange={(x) => set({ id: x.trim() })} placeholder={tr('cloud.ownClient.id')} style={{ direction: 'ltr' }} />
          <TextInput secure value={v.secret} onChange={(x) => set({ secret: x })} placeholder={tr('cloud.ownClient.secret')} style={{ direction: 'ltr' }} />
          <Text onClick={() => openUrl(M.rcloneDocsUrl(p && p.kind))} style={{ fontSize: typeScale.callout, color: t.accent, textAlign: 'start' }}>{tr('cloud.ownApp.howTo')}</Text>
        </View>
      ) : null}
    </View>
  );
}
const ownOpts = (c) => (c && c.own && c.id ? { clientId: c.id, clientSecret: c.secret || '' } : {});

// Check This Account (docs/CLOUD.md 19): its steps, then a report with nothing personal, to copy or send.
const checks = new Map();          // account id → { running, steps, report } (survives leaving the page)
// what the check uses of lord.fs (its own names; move is lord.fs.copy's move job)
const checkFs = {
  home: () => lord.fs.home(), list: (p) => lord.fs.list(p), mkdir: (p) => lord.fs.mkdir(p), write: (p, d) => lord.fs.write(p, d),
  read: (p, m) => lord.fs.read(p, m), stat: (p) => lord.fs.stat(p), move: (a, b) => lord.fs.copy(a, b, true), jobs: () => lord.fs.jobs(),
  forgetJob: (id) => lord.fs.jobForget(id), trash: (p) => lord.fs.trash(p), trashList: () => lord.fs.trashList(), removeFromTrash: (id) => lord.fs.removeFromTrash(id),
};
const checkRev = createStore(0);
function CheckAccount({ a, st, p, verified }) {
  const t = useTheme();
  const { t: tr } = useLocale();
  useStore(checkRev);
  const c = checks.get(a.id) || { running: false, steps: null, report: '' };
  const label = (p && p.label) || a.provider || a.kind;
  const save = (o) => { checks.set(a.id, { ...(checks.get(a.id) || {}), ...o }); checkRev.set((x) => x + 1); };
  const start = () => {
    save({ running: true, steps: CHECK_STEPS.map((id) => ({ id, result: null, ms: 0 })), report: '' });
    const env = { fs: checkFs, cloud, sleep: (ms) => new Promise((r) => setTimeout(r, ms)), now: () => Date.now() };
    runCheck(a, env, (steps) => save({ steps })).then((steps) => {
      const acc = (cloud.state().accounts || []).find((x) => x.id === a.id) || a;
      let info = {};
      try { info = systemInfo() || {}; } catch (e) { /* none */ }
      const report = buildReport({ version: lordosVersion(info).version, arch: info.arch, rclone: (cloud.state().service || {}).version, kind: a.kind, label,
        verified, ownApp: !!acc.ownClient, aboutFields: aboutFields(acc.about), linksSupported: !!acc.linksSupported, steps });
      save({ running: false, steps, report });
    }, () => save({ running: false }));
  };
  const mark = { pass: ['✓', tint.ok], fail: ['✕', tint.error], skipped: ['–', t.textDim] };
  return (
    <View style={{ gap: 8 }}>
      <Dim t={t}>{tr(verified ? 'cloud.check.hintVerified' : 'cloud.check.hint')}</Dim>
      <Actions start><Button small primary={!verified} disabled={c.running} label={tr(c.running ? 'cloud.check.running' : c.steps ? 'cloud.check.again' : 'cloud.check.run')} onClick={start} /></Actions>
      {c.steps ? (
        <View style={{ gap: 2 }}>
          {c.steps.map((s) => (
            <View key={s.id} style={{ flexDirection: 'row', alignItems: 'center', gap: 8, minHeight: 20 }}>
              <Text style={{ width: 14, fontSize: 12, fontWeight: 700, color: s.result ? mark[s.result][1] : t.textDim, textAlign: 'center' }}>{s.result ? mark[s.result][0] : '·'}</Text>
              <Text style={{ flex: 1, minWidth: 0, fontSize: typeScale.callout, color: t.text, textAlign: 'start' }}>{tr(`cloud.check.step.${s.id}`)}</Text>
              <Text style={{ fontSize: typeScale.caption, color: t.textDim }}>{s.result ? (s.result === 'skipped' ? tr('cloud.check.skipped') : `${Math.round(s.ms)} ms`) : ''}</Text>
            </View>
          ))}
        </View>
      ) : null}
      {c.report ? (
        <View style={{ gap: 6 }}>
          <Text style={{ fontSize: typeScale.body, fontWeight: 600, color: checkWorks(c.steps) === 'yes' ? tint.ok : tint.warn, textAlign: 'start' }}>{tr(`cloud.check.works.${checkWorks(c.steps)}`)}</Text>
          <View style={{ maxHeight: 160, borderRadius: 8, border: `1px solid ${t.separator}`, backgroundColor: t.mode === 'dark' ? 'rgba(0,0,0,0.2)' : '#f7f7f9' }}>
            <ScrollView style={{ padding: 8 }}>
              <Text style={{ fontFamily: 'monospace', fontSize: 11, color: t.text, direction: 'ltr', textAlign: 'start', lineHeight: 1.35 }}>{c.report}</Text>
            </ScrollView>
          </View>
          <Dim t={t} style={{ fontSize: typeScale.caption }}>{tr('cloud.check.private')}</Dim>
          <Actions start>
            <Button small label={tr('cloud.check.copy')} onClick={() => { clipboard.set(c.report); toast.set(tr('cloud.check.copied')); }} />
            <Button small label={tr('cloud.tellUs')} onClick={() => openUrl(issueUrl({ label, report: c.report, version: lordosVersion().version }))} />
          </Actions>
        </View>
      ) : null}
    </View>
  );
}

// Advanced: Check This Account (for a verified provider: to report a problem) and, for a browser sign-in, the person's own app.
function Advanced({ a, st, p, verified }) {
  const t = useTheme();
  const { t: tr } = useLocale();
  const [open, setOpen] = useState(false);
  const [own, setOwn] = useState({ own: true, id: '', secret: '' });
  const oauth = p ? p.group === 'oauth' : a.group === 'oauth';
  return (
    <View style={[box(t), { paddingHorizontal: 14, paddingVertical: 10, gap: 8 }]}>
      <View onClick={() => setOpen(!open)} style={{ flexDirection: 'row', alignItems: 'center', gap: 6 }}>
        <Text style={{ fontSize: 10, color: t.textDim }}>{open ? '▼' : direction() === 'rtl' ? '◀' : '▶'}</Text>
        <Text style={{ fontSize: typeScale.body, color: t.text, textAlign: 'start' }}>{tr('cloud.advanced')}</Text>
      </View>
      {open && verified ? (
        <View style={{ gap: 6 }}>
          <Text style={{ fontSize: typeScale.body, fontWeight: 600, color: t.text, textAlign: 'start' }}>{tr('cloud.check.title')}</Text>
          <CheckAccount a={a} st={st} p={p} verified />
        </View>
      ) : null}
      {open && oauth ? (
        <View style={{ gap: 6 }}>
          <Text style={{ fontSize: typeScale.body, fontWeight: 600, color: t.text, textAlign: 'start' }}>{tr('cloud.ownClient.title')}</Text>
          <Dim t={t}>{tr(a.ownClient ? 'cloud.ownApp.inUse' : 'cloud.ownClient.hint')}</Dim>
          <OwnAppChoice p={p || { kind: a.kind }} value={own} onChange={setOwn} />
          <Actions>
            <Button small label={tr('cloud.signInAgain')} disabled={own.own && !own.id} onClick={() => beginAgain(a, p, ownOpts(own))} />
          </Actions>
        </View>
      ) : null}
    </View>
  );
}

function beginAgain(a, p, own = {}) {
  const r = cloud.beginSignIn(a.kind, a.name, own);
  setView({ sheet: { kind: 'signin', provider: p || { kind: a.kind, label: a.provider }, name: a.name, error: r && r.error } });
}
// Sign In Again: the browser for an OAuth account (with the own-app choice first for a provider not verified yet, or
// when the account already uses its own app), the credentials form for the others (the same name updates it).
function signInAgain(a) {
  const p = providerOf(a.kind, a);
  const oauth = (p && p.group) ? p.group === 'oauth' : a.group === 'oauth';
  if (oauth) {
    if (M.ownAppOffered(p || { kind: a.kind, group: 'oauth' }, CLOUD_PROVIDERS) || a.ownClient) setView({ sheet: { kind: 'again', id: a.id } });
    else beginAgain(a, p);
    return;
  }
  // the form needs the provider's fields: from rclone's list (asked now if this session hasn't yet)
  const open = (q) => { if (q && (q.fields || []).length) setView({ sheet: { kind: 'cred', provider: q, name: a.name, again: true } }); };
  if (p && (p.fields || []).length) open(p);
  else loadProviders().then(() => open(providerOf(a.kind)), () => {});
}

function AgainSheet({ st, sheet }) {
  const t = useTheme();
  const { t: tr } = useLocale();
  const a = (st.accounts || []).find((x) => x.id === sheet.id);
  const p = a ? providerOf(a.kind, a) : null;
  const [own, setOwn] = useState({ own: !!(a && a.ownClient), id: '', secret: '' });
  const close = () => setView({ sheet: null });
  if (!a) return null;
  return (
    <Sheet width={440} onClose={close} label={tr('cloud.signInAgain')}>
      <View style={{ padding: spacing.xl, gap: spacing.m }}>
        <Text style={{ fontSize: typeScale.headline, fontWeight: 600, color: t.text, textAlign: 'start' }}>{tr('cloud.again.title', { name: a.name })}</Text>
        <OwnAppChoice p={p || { kind: a.kind }} value={own} onChange={setOwn} />
        <View style={{ flexDirection: 'row', justifyContent: 'flex-end', gap: spacing.s }}>
          <Button label={tr('cloud.cancel')} onClick={close} />
          <Button primary label={tr('cloud.continue')} disabled={own.own && !own.id} onClick={() => beginAgain(a, p, ownOpts(own))} />
        </View>
      </View>
    </Sheet>
  );
}

function AccountPage({ a, st }) {
  const t = useTheme();
  const { t: tr } = useLocale();
  const accs = st.accounts || [];
  const [picking, setPicking] = useState(false);
  const win = useAppWindow();
  const website = useMemo(() => cloud.websiteUrl(a.id), [a.id]);
  const last = (st.recent || []).find((r) => r.account === a.id || M.accountOf(r.path, [a]));
  useStore(providerCache);
  const p = providerOf(a.kind, a);
  const verified = typeof (providerCache.get().find((x) => x.kind === a.kind) || {}).verified === 'boolean'
    ? providerCache.get().find((x) => x.kind === a.kind).verified : M.isVerified(a.kind, CLOUD_PROVIDERS);
  const line = M.stateLine(a);
  const wide = (wm.frame(wm.get(win.id) || {}).w || 880) >= 860;
  const side = (
    <View aria-label={tr('cloud.options')} style={{ width: wide ? 280 : undefined, gap: 12 }}>
      {verified ? null : (
        <View style={[box(t), { paddingHorizontal: 14, paddingVertical: 12, gap: 6, border: `1px solid ${t.accent}` }]}>
          <Text style={{ fontSize: typeScale.body, fontWeight: 600, color: t.text, textAlign: 'start' }}>{tr('cloud.check.title')}</Text>
          <CheckAccount a={a} st={st} p={p} verified={false} />
        </View>
      )}
      <Options a={a} accounts={accs} />
      <View style={[box(t), { paddingHorizontal: 14, paddingVertical: 12, gap: 4 }]}>
        <Dim t={t}>{a.provider || a.kind}</Dim>
        <Dim t={t} style={{ color: toneColor(t, line.tone) }}>{tr(line.key, line.vars)}</Dim>
        <Dim t={t}>{tr('cloud.inKeyring')}</Dim>
        <Dim t={t}>{last ? tr('cloud.lastSynced', { when: (() => { const g = M.ago(last.at); return tr(g.key, g.vars); })() }) : tr('cloud.notSyncedYet')}</Dim>
        {a.error ? <Dim t={t} style={{ color: tint.warn }}>{errorText(a.error)}</Dim> : null}
        <Dim t={t}>{verified ? tr('cloud.verified.yes') : tr('cloud.verified.no')}</Dim>
      </View>
      <Button label={tr('cloud.signInAgain')} onClick={() => signInAgain(a)} />
      <View onClick={() => setView({ sheet: { kind: 'remove', id: a.id } })} style={{ height: 30, borderRadius: t.controlRadius, alignItems: 'center', justifyContent: 'center', backgroundColor: t.mode === 'dark' ? '#7a2230' : '#d6453d' }}>
        <Text style={{ fontSize: typeScale.body, fontWeight: 600, color: '#fff' }}>{tr('cloud.remove')}</Text>
      </View>
      <Dim t={t} style={{ fontSize: typeScale.caption }}>{tr('cloud.removeHint')}</Dim>
      <Advanced a={a} st={st} p={p} verified={verified} />
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
  const hint = more != null ? tr('cloud.add.more', { count: more }) : p.description || p.hint;
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

function ProviderGroups({ groups, sel, onPick, onMore }) {
  const t = useTheme();
  const { t: tr } = useLocale();
  const rows = (items) => { const out = []; for (let i = 0; i < items.length; i += 3) out.push(items.slice(i, i + 3)); return out; };
  return groups.map((g) => {
    const items = [...g.items.map((x) => ({ p: x })), ...(g.more ? [{ more: g.more, group: g.group }] : [])];
    return (
      <View key={g.group}>
        <Text style={{ paddingHorizontal: 22, paddingTop: 8, paddingBottom: 4, fontSize: 11, fontWeight: 600, color: t.textDim, textTransform: 'uppercase', textAlign: 'start' }}>{tr(`cloud.group.${g.group}`)}</Text>
        <View style={{ paddingHorizontal: 22, paddingBottom: 8, gap: 8 }}>
          {rows(items).map((r, i) => (
            <View key={i} style={{ flexDirection: 'row', gap: 8 }}>
              {r.map((it) => (it.more != null
                ? <ProviderTile key="more" more={it.more} onClick={() => onMore(it.group)} />
                : <ProviderTile key={it.p.kind} p={it.p} on={sel === it.p.kind} onClick={() => onPick(it.p)} />))}
              {r.length < 3 ? [...Array(3 - r.length)].map((_, k) => <View key={`f${k}`} style={{ flex: 1 }} />) : null}
            </View>
          ))}
        </View>
      </View>
    );
  });
}

function AddSheet({ st }) {
  const t = useTheme();
  const { t: tr } = useLocale();
  // 17.2: asked of rclone every time the sheet opens; nothing but Cancel until it answers
  const [providers, setProviders] = useState([]);
  const [loading, setLoading] = useState(true);
  const [loadErr, setLoadErr] = useState(null);
  const load = () => {
    setLoading(true); setLoadErr(null);
    loadProviders().then((l) => { setProviders(l); setLoading(false); }, (e) => { setLoadErr((e && e.error) || 'cloud.unavailable'); setLoading(false); });
  };
  useEffect(load, []);
  const [q, setQ] = useState('');
  const [expanded, setExpanded] = useState([]);
  const [showUnverified, setShowUnverified] = useState(false);
  const [sel, setSel] = useState(null);
  const [own, setOwn] = useState({ own: false, id: '', secret: '' });
  const [name, setName] = useState(() => defaultName(st.accounts, null, tr));
  const [named, setNamed] = useState(false);          // the person typed a name: picking a provider keeps it
  const [err, setErr] = useState('');
  const parts = M.splitProviders(providers, CLOUD_PROVIDERS, q, expanded, showUnverified);
  const p = providers.find((x) => x.kind === sel);
  const offerOwn = M.ownAppOffered(p, CLOUD_PROVIDERS);
  const close = () => setView({ sheet: null });
  const pick = (x) => { setSel(x.kind); setErr(''); setOwn({ own: false, id: '', secret: '' }); if (!named) setName(defaultName(st.accounts, x, tr)); };
  const go = () => {
    if (!p) return;
    const bad = M.nameProblem(name, st.accounts);
    if (bad) { setErr(tr(`cloud.name.${bad}`)); return; }
    if (offerOwn && own.own && !own.id) { setErr(tr('cloud.ownApp.needId')); return; }
    if (p.group === 'oauth') {
      const r = cloud.beginSignIn(p.kind, name.trim(), offerOwn ? ownOpts(own) : {});
      setView({ sheet: { kind: 'signin', provider: p, name: name.trim(), error: r && r.error } });
    } else setView({ sheet: { kind: 'cred', provider: p, name: name.trim() } });
  };
  const none = providers.length && !parts.verified.length && !parts.unverified.length;
  if (loading || loadErr) {
    return (
      <Sheet width={640} top={30} onClose={close} label={tr('cloud.add.title')}>
        <View style={{ paddingHorizontal: 22, paddingTop: 20, gap: 6 }}>
          <Text style={{ fontSize: typeScale.title, fontWeight: 600, color: t.text, textAlign: 'start' }}>{tr('cloud.add.title')}</Text>
        </View>
        <View style={{ height: 220, alignItems: 'center', justifyContent: 'center', gap: 12, paddingHorizontal: 40 }}>
          {loading ? <Spinner /> : null}
          <Text style={{ fontSize: typeScale.body, color: loadErr ? tint.error : t.textDim, textAlign: 'center' }}>{loading ? tr('cloud.add.loading') : `${tr('cloud.add.loadFailed')} ${errorText(loadErr)}`}</Text>
          {loadErr ? <Button primary label={tr('cloud.signin.tryAgain')} onClick={load} /> : null}
        </View>
        <View style={{ flexDirection: 'row', justifyContent: 'flex-end', paddingHorizontal: 22, paddingBottom: 18 }}>
          <Button label={tr('cloud.cancel')} onClick={close} />
        </View>
      </Sheet>
    );
  }
  return (
    <Sheet width={640} top={30} onClose={close} label={tr('cloud.add.title')}>
      <View style={{ paddingHorizontal: 22, paddingTop: 20, paddingBottom: 10, gap: 6 }}>
        <Text style={{ fontSize: typeScale.title, fontWeight: 600, color: t.text, textAlign: 'start' }}>{tr('cloud.add.title')}</Text>
        <Dim t={t} style={{ fontSize: typeScale.body }}>{tr('cloud.add.subtitle')}</Dim>
      </View>
      <View style={{ paddingHorizontal: 22, paddingTop: 6, paddingBottom: 10 }}>
        <SearchField value={q} onChange={setQ} placeholder={tr('cloud.add.search', { count: providers.length })} autoFocus />
      </View>
      <ScrollView style={{ maxHeight: offerOwn ? 260 : 380 }}>
        <View style={{ paddingBottom: 6 }}>
          {!providers.length ? <Dim t={t} style={{ paddingHorizontal: 22 }}>{tr('cloud.add.none')}</Dim> : null}
          {none ? <Dim t={t} style={{ paddingHorizontal: 22 }}>{tr('cloud.add.none')}</Dim> : null}
          <ProviderGroups groups={parts.verified} sel={sel} onPick={pick} onMore={(g) => setExpanded([...expanded, g])} />
          {parts.unverifiedCount ? (
            <View style={{ marginTop: 6, borderTopWidth: 1, borderTopColor: t.separator }}>
              <View onClick={() => setShowUnverified(!parts.open)} style={{ flexDirection: 'row', alignItems: 'center', gap: 6, paddingHorizontal: 22, paddingTop: 10, paddingBottom: 4 }}>
                <Text style={{ fontSize: 10, color: t.textDim }}>{parts.open ? '▼' : direction() === 'rtl' ? '◀' : '▶'}</Text>
                <Text style={{ flex: 1, fontSize: typeScale.body, fontWeight: 600, color: t.text, textAlign: 'start' }}>{tr('cloud.unverified.title')}</Text>
                <Dim t={t}>{String(parts.unverifiedCount)}</Dim>
              </View>
              {parts.open ? (
                <>
                  <View style={{ paddingHorizontal: 22, paddingBottom: 6, gap: 6 }}>
                    <Dim t={t}>{tr('cloud.unverified.line')}</Dim>
                    <Actions start><Button small label={tr('cloud.tellUs')} onClick={() => openUrl(issueUrl({ label: p && !parts.verified.some((g) => g.items.includes(p)) ? p.label : '', version: lordosVersion().version }))} /></Actions>
                  </View>
                  <ProviderGroups groups={parts.unverified} sel={sel} onPick={pick} onMore={() => {}} />
                </>
              ) : null}
            </View>
          ) : null}
        </View>
      </ScrollView>
      {p && M.notesFor(p.kind).length ? (
        <View style={{ marginHorizontal: 22, marginTop: 8, paddingHorizontal: 12, paddingVertical: 8, gap: 3, borderRadius: 8, border: `1px solid ${tint.warn}` }}>
          <Text style={{ fontSize: typeScale.callout, fontWeight: 600, color: t.text, textAlign: 'start' }}>{tr('cloud.notes.before', { provider: p.label })}</Text>
          {M.notesFor(p.kind).map((k) => <Dim key={k} t={t}>{`• ${tr(k)}`}</Dim>)}
        </View>
      ) : null}
      {offerOwn ? (
        <View style={{ paddingHorizontal: 22, paddingTop: 10, gap: 6, borderTopWidth: 1, borderTopColor: t.separator }}>
          <Text style={{ fontSize: typeScale.callout, fontWeight: 600, color: t.text, textAlign: 'start' }}>{tr('cloud.ownApp.title', { provider: p.label })}</Text>
          <OwnAppChoice p={p} value={own} onChange={setOwn} />
        </View>
      ) : null}
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
    Promise.resolve(cloud.add(p.kind, name.trim(), M.cleanFields(p, vals))).then((r) => {
      setBusy(false);
      if (r && r.error) { setErr({ text: errorText(r.error), field: r.field || null }); return; }
      afterAdd(r, p, name.trim());
    });
  };
  const notes = M.notesFor(p.kind);
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
          {p.description || p.hint ? <Dim t={t}>{p.description || p.hint}</Dim> : null}
        </View>
      </View>
      {notes.length ? (
        <View style={{ marginHorizontal: 20, marginTop: 4, paddingHorizontal: 12, paddingVertical: 10, gap: 4, borderRadius: 8, border: `1px solid ${tint.warn}` }}>
          <Text style={{ fontSize: typeScale.callout, fontWeight: 600, color: t.text, textAlign: 'start' }}>{tr('cloud.notes.before', { provider: p.label })}</Text>
          {notes.map((k) => <Dim key={k} t={t}>{`• ${tr(k)}`}</Dim>)}
        </View>
      ) : null}
      <View style={{ paddingHorizontal: 20, paddingVertical: 8, gap: 10 }}>
        {(p.fields || []).map((f) => field(f.name, fieldLabel(f),
          // a choice among a few of the provider's examples (a preset, a service), else a text field; secrets masked
          // (a segmented control: a pop-up's list would open under the sheet's layer)
          !f.secret && (f.examples || []).length > 1 && f.examples.length <= 4
            ? <Segmented small fill value={vals[f.name] || String(f.examples[0])} onChange={(v) => { setVals({ ...vals, [f.name]: v }); setErr(null); }}
              options={f.examples.map(String).map((x) => [x, x.charAt(0).toUpperCase() + x.slice(1)])} />
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

// What add/answer said (17.2): a question of rclone's (another step of the same sheet), the new account, or an error.
function afterAdd(r, p, name) {
  if (r && r.question) { setView({ sheet: { kind: 'question', question: r.question, provider: p, name } }); return; }
  if (r && r.error) { setView({ sheet: { kind: 'question', provider: p, name, error: r.error } }); return; }
  setView({ page: r && r.id ? r.id : null, sheet: null });
}

// One of rclone's questions while setting up an account (a code, a choice, a confirmation): its help, an input of its
// type, Continue (cloud.answer) until the account exists.
function QuestionSheet({ sheet }) {
  const t = useTheme();
  const { t: tr } = useLocale();
  const q = sheet.question || null;
  const how = M.questionInput(q);
  const [value, setValue] = useState(how.value);
  const [busy, setBusy] = useState(false);
  const [err, setErr] = useState(sheet.error ? errorText(sheet.error) : '');
  useEffect(() => { setValue(M.questionInput(sheet.question).value); setErr(sheet.error ? errorText(sheet.error) : ''); }, [sheet.question && sheet.question.state, sheet.error]);
  const close = () => setView({ sheet: null });
  const go = () => {
    const ans = M.questionAnswer(q, value);
    if (ans == null) { setErr(tr('cloud.field.required')); return; }
    setBusy(true);
    cloud.answer(q.state, ans).then((r) => { setBusy(false); afterAdd(r, sheet.provider, sheet.name); });
  };
  const input = !q ? null : how.input === 'bool'
    ? <Toggle on={value === 'true'} onChange={(v) => setValue(v ? 'true' : 'false')} />
    : how.input === 'choice'
      ? <View style={{ gap: 4 }}>{how.options.map(([v, label]) => (
        <View key={v} onClick={() => setValue(v)} style={{ flexDirection: 'row', alignItems: 'center', gap: 8, minHeight: 26, paddingHorizontal: 8, borderRadius: 6, backgroundColor: value === v ? t.accent : 'transparent' }}>
          <Text style={{ flex: 1, fontSize: typeScale.body, color: value === v ? '#fff' : t.text, textAlign: 'start' }}>{label}</Text>
        </View>))}</View>
      : <TextInput autoFocus secure={how.input === 'password'} value={value} onChange={setValue} onSubmit={go} style={{ direction: 'ltr' }} />;
  return (
    <Sheet width={460} top={60} onClose={close} label={(sheet.provider || {}).label}>
      <View style={{ padding: spacing.xl, gap: spacing.m }}>
        <Text style={{ fontSize: typeScale.headline, fontWeight: 600, color: t.text, textAlign: 'start' }}>{tr('cloud.question.title', { provider: (sheet.provider || {}).label || '' })}</Text>
        {q && q.help ? <Text style={{ fontSize: typeScale.body, color: t.text, lineHeight: 1.45, textAlign: 'start' }}>{String(q.help)}</Text> : null}
        {input}
        {err ? <Text style={{ fontSize: typeScale.callout, color: tint.error, textAlign: 'start' }}>{err}</Text> : null}
        <View style={{ flexDirection: 'row', justifyContent: 'flex-end', gap: spacing.s }}>
          <Button label={tr('cloud.cancel')} onClick={close} />
          {q ? <Button primary label={tr(busy ? 'cloud.connecting' : 'cloud.continue')} disabled={busy} onClick={go} /> : null}
        </View>
      </View>
    </Sheet>
  );
}

// Empty Trash (17.2), for providers that have one: asked first; the answer comes later.
function EmptyTrashSheet({ st, sheet }) {
  const t = useTheme();
  const { t: tr } = useLocale();
  const a = (st.accounts || []).find((x) => x.id === sheet.id);
  const [busy, setBusy] = useState(false);
  const [msg, setMsg] = useState(null);
  const close = () => setView({ sheet: null });
  if (!a) return null;
  const go = () => {
    setBusy(true);
    cloud.emptyTrash(a.id).then((r) => { setBusy(false); setMsg(r && r.error ? { ok: false, text: errorText(r.error) } : { ok: true, text: tr('cloud.trash.done', { provider: a.provider || a.kind }) }); });
  };
  return (
    <Sheet width={400} onClose={close} label={tr('cloud.trash.empty')}>
      <View style={{ padding: spacing.xl, gap: spacing.s }}>
        <Text style={{ fontSize: typeScale.headline, fontWeight: 600, color: t.text, textAlign: 'start' }}>{tr('cloud.trash.title', { name: a.name })}</Text>
        <Dim t={t} style={{ fontSize: typeScale.body }}>{tr('cloud.trash.detail', { provider: a.provider || a.kind })}</Dim>
        {busy ? <View style={{ flexDirection: 'row', alignItems: 'center', gap: 8 }}><Spinner size={16} /><Dim t={t}>{tr('cloud.trash.running')}</Dim></View> : null}
        {msg ? <Text style={{ fontSize: typeScale.callout, color: msg.ok ? tint.ok : tint.error, textAlign: 'start' }}>{msg.text}</Text> : null}
        <View style={{ flexDirection: 'row', justifyContent: 'flex-end', gap: spacing.s, marginTop: spacing.m }}>
          <Button label={tr(msg ? 'cloud.done' : 'cloud.cancel')} onClick={close} />
          {msg ? null : (
            <View onClick={busy ? undefined : go} style={{ height: 28, paddingHorizontal: 14, borderRadius: t.controlRadius, justifyContent: 'center', backgroundColor: '#e5483f', opacity: busy ? 0.5 : 1 }}>
              <Text style={{ fontSize: typeScale.body, fontWeight: 500, color: '#fff' }}>{tr('cloud.trash.ok')}</Text>
            </View>
          )}
        </View>
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
      {sh && sh.kind === 'again' ? <AgainSheet st={st} sheet={sh} /> : null}
      {sh && sh.kind === 'question' ? <QuestionSheet sheet={sh} /> : null}
      {sh && sh.kind === 'emptyTrash' ? <EmptyTrashSheet st={st} sheet={sh} /> : null}
    </View>
  );
}
