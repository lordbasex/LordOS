// Settings → Cloud (docs/CLOUD.md sections 6, 8, 12): the accounts with their space, what Cloud keeps on this
// computer, bandwidth, the service and About rclone; the sheets to add an account (providers, sign-in in the
// browser, a server's or a bucket's credentials) and an account's own page. Built from the approved mocks (Main,
// AddAccount, SignIn, Credentials, Account). Everything goes through cloud (sdk/cloud.js); nothing here talks to
// rclone.
import {
  View, Text, Button, Toggle, TextInput, SearchField, Segmented, ScrollView, useTheme, useLocale, useStore, useCloud, cloud, cloudModel, createStore,
  formatBytes, formatDate, formatTime, tOptional, spacing, typeScale, wm, useAppWindow, FilePicker, openUrl, clipboard, toast, addListener, direction, errorText, fs,
  AccountBadge, UsageRing, SpaceBar, CloudGlyph, useSettings, hasTextFocus, cloudState, askKeyringUnlock, systemInfo, lordosVersion,
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

// A sheet of the Settings window: drawn inside the window itself (CloudSheets, at the window's root), never in the
// shell's modal layer, so a window above Settings (the browser during a sign-in) keeps all its input. A dim backdrop
// over the window's content and a centered card; Escape closes it while Settings is the window in front.
function Sheet({ width = 460, top, onClose, children, label }) {
  const t = useTheme();
  const win = useAppWindow();
  const focused = useRef(win.focused);
  focused.current = win.focused;
  useEffect(() => addListener('keydown', (e) => { if (e.key === 'Escape' && onClose && focused.current) onClose(); }), [onClose]);
  return (
    <View onMouseDown={(e) => e.stopPropagation?.()} style={{ position: 'absolute', left: 0, top: 0, right: 0, bottom: 0, backgroundColor: 'rgba(0,0,0,0.45)',
      alignItems: 'center', justifyContent: top == null ? 'center' : 'flex-start', paddingTop: top || 0 }}>
      <View aria-label={label} style={{ width, maxHeight: '96%', borderRadius: 14, backgroundColor: t.mode === 'dark' ? '#2a2a30' : t.windowBg,
        border: `1px solid ${t.separator}`, boxShadow: '0 30px 80px rgba(0,0,0,0.55)', overflow: 'hidden' }}>
        {children}
      </View>
    </View>
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
// an error answer with its details (fingerprints, a date) in the person's language (errors.json); the service's
// "not running" only when it isn't (explainError)
function errorMessage(r, acc) {
  const e = M.errorVars(typeof r === 'string' ? { error: r } : r, (d) => formatDate(d, 'short'));
  const code = M.explainError(e.code, cloudState.get(), acc) || e.code;
  return tOptional(`errors.${code}`, e.vars) || errorText(code);
}

// a changed server key or certificate: before and now, in monospace (U10)
function Prints({ prints }) {
  const t = useTheme();
  const { t: tr } = useLocale();
  if (!prints) return null;
  const line = (k, v) => (v ? (
    <View key={k} style={{ flexDirection: 'row', gap: 8 }}>
      <Text style={{ width: 54, fontSize: typeScale.caption, color: t.textDim, textAlign: 'end' }}>{tr(k)}</Text>
      <Text style={{ flex: 1, minWidth: 0, fontFamily: 'monospace', fontSize: 11, color: t.text, direction: 'ltr', textAlign: 'start' }}>{v}</Text>
    </View>) : null);
  return <View style={{ gap: 3 }}>{line('cloud.trust.old', prints.old)}{line('cloud.trust.new', prints.new)}</View>;
}

const providerCache = createStore([]);
const loadProviders = () => cloud.providers().then((l) => { providerCache.set(() => l); return l; });
function providerOf(kind, a) {
  return providerCache.get().find((p) => p.kind === kind) || (a ? { kind: a.kind, label: M.providerName(a, providerCache.get()), group: a.group, fields: [] } : null);
}

// Measure (17.2): a provider without a quota is measured on demand; one at a time per account, it can be stopped
const measuring = new Map();       // account id → { error? }
const measureRev = createStore(0);
const startMeasure = (id) => {
  measuring.set(id, {});
  measureRev.set((x) => x + 1);
  cloud.measure(id).then((r) => {
    if (!measuring.has(id)) return;
    const st = cloudState.get(), acc = (st.accounts || []).find((x) => x.id === id);
    if (r && r.error) measuring.set(id, { error: errorMessage(r, acc), done: true }); else measuring.delete(id);
    measureRev.set((x) => x + 1);
  });
};
const stopMeasure = (id) => { cloud.cancelMeasure(id); measuring.delete(id); measureRev.set((x) => x + 1); };
function MeasureRow({ a }) {
  const t = useTheme();
  const { t: tr } = useLocale();
  useStore(measureRev);
  if (!M.measurable(a)) return null;
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
      {m && m.error ? <Text style={{ fontSize: typeScale.caption, color: tint.error, textAlign: 'start' }}>{m.error}</Text> : null}
    </View>
  );
}

// Unlock Keyring… (an account's error cloud.keyringLocked): the system's keyring prompt; once it is unlocked lord-server
// connects the waiting accounts at once (lord-server hears the vault's unlock too; cloud.retry asks it now, 17.2 item 8)
export function unlockKeyring() {
  return askKeyringUnlock().then((ok) => { if (ok) cloud.retry(null); return ok; });
}
function UnlockRow({ a, small }) {
  const t = useTheme();
  const { t: tr } = useLocale();
  if (!M.needsUnlock(a)) return null;
  return (
    <View style={{ gap: 4 }}>
      <View style={{ flexDirection: 'row', alignItems: 'center', gap: 8 }}>
        <Text style={{ flex: 1, minWidth: 0, fontSize: typeScale.callout, color: tint.warn, textAlign: 'start' }}>{tr('cloud.state.locked')}</Text>
        <Button small primary label={tr('cloud.unlock')} onClick={unlockKeyring} />
      </View>
      {small ? null : <Dim t={t} style={{ fontSize: typeScale.caption }}>{tr('cloud.unlockWhy')}</Dim>}
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
        <Dim t={t}>{bar.total != null ? tr('cloud.space.usedOf', { used: formatBytes(bar.used), total: formatBytes(bar.total) }) : tr('cloud.space.usedOnly', { used: formatBytes(bar.used) })}</Dim>
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
  const line = M.stateLine(a, (cloudState.get() || {}).service);
  const color = M.accountColor(accounts, a.id);
  return (
    <BoxRow t={t} last={last} onClick={() => setView({ page: a.id, sheet: null })}>
      <AccountBadge color={color} size={32} />
      <View style={{ flex: 1, minWidth: 0, gap: 2 }}>
        <Text style={{ fontSize: typeScale.body, fontWeight: 600, color: t.text, textAlign: 'start' }}>{a.name}</Text>
        <View style={{ flexDirection: 'row', minWidth: 0 }}>
          <Text style={{ flexShrink: 1, minWidth: 0, maxWidth: '55%', fontSize: typeScale.callout, color: t.textDim, whiteSpace: 'nowrap', overflow: 'hidden', textOverflow: 'ellipsis' }}>{M.providerName(a, providerCache.get())}</Text>
          <Text style={{ fontSize: typeScale.callout, color: t.textDim, whiteSpace: 'nowrap' }}>{' · '}</Text>
          <Text style={{ flex: 1, minWidth: 0, fontSize: typeScale.callout, color: toneColor(t, line.tone === 'warn' ? 'warn' : 'dim'), whiteSpace: 'nowrap', overflow: 'hidden', textAlign: 'start' }}>{tr(line.key, line.vars)}</Text>
        </View>
      </View>
      {M.needsUnlock(a) ? <View onClick={(e) => e.stopPropagation?.()}><Button small label={tr('cloud.unlock')} onClick={unlockKeyring} /></View> : null}
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
      {svc.running === false && accs.length ? (
        <View style={[box(t), { padding: 16, gap: 8, border: `1px solid ${tint.warn}` }]}>
          <Text style={{ fontSize: typeScale.body, fontWeight: 600, color: t.text, textAlign: 'start' }}>{tr('cloud.serviceDown.title')}</Text>
          <Dim t={t}>{svc.error ? errorText(svc.error) : tr('cloud.serviceDown.body')}</Dim>
          <Actions start>
            <Button small primary label={tr('cloud.service.restart')} onClick={() => cloud.restart()} />
            <Button small label={tr('cloud.service.log')} onClick={() => cloud.showLog()} />
          </Actions>
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
  if (!u) {
    const st = M.spaceState(a);
    if (st === 'noQuota') {
      return (
        <View style={[box(t), { padding: 18, gap: 8 }]}>
          <Text style={{ fontSize: typeScale.body, fontWeight: 600, color: t.text, textAlign: 'start' }}>{tr('cloud.space.title')}</Text>
          <MeasureRow a={a} />
        </View>
      );
    }
    const why = st === 'reason';
    return (
      <View style={[box(t), { padding: 18, gap: 8 }]}>
        <Text style={{ fontSize: typeScale.body, fontWeight: 600, color: t.text, textAlign: 'start' }}>{tr('cloud.space.title')}</Text>
        <Dim t={t}>{tr(why ? 'cloud.space.notConnected' : 'cloud.space.unknown')}</Dim>
        {/* the reason itself (and Unlock Keyring…) is said once, in the account's status box */}
      </View>
    );
  }
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
      for (const p of a.kept || []) { try { const d = fs.du(p, 300); if (d && d.size != null) s[p] = d.size; } catch (e) { /* not there yet */ } }
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
            <Text onClick={() => cloud.unlink(l.path).then((r) => { if (r && r.error) toast.set(errorMessage(r)); })} style={{ fontSize: typeScale.body, color: tint.error }}>{tr('cloud.stopSharing')}</Text>
          </BoxRow>
        );
      })}
    </View>
  );
}

// The account's name (its folder in ~/Cloud): a click makes it a field; Enter, Save or a click elsewhere saves it
// (cloud.rename), Escape leaves it; a problem shows under the field.
function NameField({ a, accounts }) {
  const t = useTheme();
  const { t: tr } = useLocale();
  const [edit, setEdit] = useState(null);
  const [err, setErr] = useState('');
  const box = useRef(null);
  const editRef = useRef(null);
  editRef.current = edit;
  const save = () => {
    const n = (editRef.current || '').trim();
    if (editRef.current == null) return;
    if (n === a.name) { setEdit(null); setErr(''); return; }
    const bad = M.nameProblem(n, accounts, a.id);
    if (bad) { setErr(tr(`cloud.name.${bad}`)); return; }
    const r = cloud.rename(a.id, n);
    if (r && r.error) { setErr(errorText(r.error)); return; }
    setEdit(null); setErr('');
  };
  // a click outside the field saves it, as leaving a field does
  useEffect(() => {
    if (edit == null) return undefined;
    return addListener('pointerdown', (e) => {
      const n = box.current;
      if (!n) return;
      const [x, y, w, h] = lord.getLayout(n.id);
      if (e.x < x || e.x > x + w || e.y < y || e.y > y + h) save();
    });
  }, [edit == null]);
  if (edit != null) {
    return (
      <View ref={box} style={{ width: 210, gap: 3 }}>
        <View style={{ flexDirection: 'row', alignItems: 'center', gap: 6 }}>
          <View style={{ flex: 1, minWidth: 0 }}>
            <TextInput autoFocus selectAll value={edit} onChange={(v) => { setEdit(v.slice(0, M.NAME_MAX)); setErr(''); }} onSubmit={save} onCancel={() => { setEdit(null); setErr(''); }} />
          </View>
          <Button small primary label={tr('cloud.save')} onClick={save} />
        </View>
        {err ? <Text style={{ fontSize: typeScale.caption, color: tint.error, textAlign: 'start' }}>{err}</Text> : null}
      </View>
    );
  }
  return (
    <View onClick={() => setEdit(a.name)} title={tr('cloud.rename')} style={{ flexDirection: 'row', alignItems: 'center', gap: 6, paddingHorizontal: 10, paddingVertical: 4, borderRadius: 6, backgroundColor: t.control }}>
      <Text style={{ fontSize: typeScale.callout, color: t.text }}>{a.name}</Text>
      <Text style={{ fontSize: 11, color: t.textDim }}>✎</Text>
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
function OwnAppChoice({ p, value, onChange, row }) {
  const t = useTheme();
  const { t: tr } = useLocale();
  const v = value || { own: false, id: '', secret: '' };
  const set = (o) => onChange({ ...v, ...o });
  const label = (p && p.label) || (p && p.kind) || '';
  // radio keys: ↑ ↓ (or 1, 2) choose while no text field has the keys
  useEffect(() => addListener('keydown', (e) => {
    if (hasTextFocus()) return;
    if (e.key === 'ArrowUp' || e.key === '1') set({ own: false });
    else if (e.key === 'ArrowDown' || e.key === '2') set({ own: true });
  }), [v.own, v.id, v.secret]);
  // one option: a card like the provider tiles, a radio dot, its title and line; the selected one has the accent border
  const card = (own, title, line, tag, extra) => {
    const on = !!v.own === own;
    return (
      <View role="radio" aria-checked={on ? 'true' : 'false'} onClick={() => { if (!on) set({ own }); }}
        style={{ flex: row ? 1 : undefined, minWidth: 0, padding: 12, gap: 10, borderRadius: 10, border: `1px solid ${on ? t.accent : t.separator}`,
          backgroundColor: on ? (t.mode === 'dark' ? '#16335a' : '#dcebff') : t.mode === 'dark' ? '#1e1e22' : '#f4f4f6' }}>
        <View style={{ flexDirection: 'row', alignItems: 'flex-start', gap: 10 }}>
          <View style={{ width: 16, height: 16, marginTop: 1, borderRadius: 8, border: `1.5px solid ${on ? t.accent : t.textDim}`, alignItems: 'center', justifyContent: 'center', flexShrink: 0 }}>
            {on ? <View style={{ width: 8, height: 8, borderRadius: 4, backgroundColor: t.accent }} /> : null}
          </View>
          <View style={{ flex: 1, minWidth: 0, gap: 3 }}>
            <View style={{ flexDirection: 'row', alignItems: 'center', gap: 8 }}>
              <Text style={{ fontSize: typeScale.body, fontWeight: 600, color: t.text, textAlign: 'start' }}>{title}</Text>
              {tag ? (
                <View style={{ paddingHorizontal: 6, height: 17, borderRadius: 5, justifyContent: 'center', backgroundColor: t.mode === 'dark' ? 'rgba(48,209,88,0.18)' : 'rgba(36,138,61,0.12)' }}>
                  <Text style={{ fontSize: 10, fontWeight: 600, color: t.mode === 'dark' ? '#30d158' : '#248a3d' }}>{tag}</Text>
                </View>
              ) : null}
            </View>
            <Text style={{ fontSize: typeScale.callout, color: t.textDim, lineHeight: 1.4, textAlign: 'start' }}>{line}</Text>
          </View>
        </View>
        {on && extra ? <View style={{ gap: 6, paddingInlineStart: 26 }}>{extra}</View> : null}
      </View>
    );
  };
  return (
    <View role="radiogroup" aria-label={tr('cloud.ownApp.title')} style={{ gap: 8, flexDirection: row ? 'row' : 'column', alignItems: row ? 'flex-start' : 'stretch' }}>
      {card(false, tr('cloud.ownApp.rclone'), tr('cloud.ownApp.rcloneLine', { provider: label }), tr('cloud.ownApp.recommended'))}
      {card(true, tr('cloud.ownApp.own'), tr('cloud.ownApp.ownLine', { provider: label }), null, (
        <>
          <TextInput value={v.id} onChange={(x) => set({ id: x.trim() })} placeholder={tr('cloud.ownClient.id')} style={{ direction: 'ltr' }} />
          <TextInput secure value={v.secret} onChange={(x) => set({ secret: x })} placeholder={tr('cloud.ownClient.secret')} style={{ direction: 'ltr' }} />
          <View style={{ flexDirection: 'row', alignItems: 'center', gap: 8 }}>
            <Text style={{ flex: 1, minWidth: 0, fontSize: typeScale.callout, color: t.textDim, textAlign: 'start' }}>{tr('cloud.ownApp.redirect', { redirect: M.OAUTH_REDIRECT })}</Text>
            <Button small label={tr('cloud.copy')} onClick={() => { clipboard.set(M.OAUTH_REDIRECT); toast.set(tr('cloud.ownApp.copied')); }} />
          </View>
          <Text onClick={() => openUrl(M.rcloneDocsUrl(p && p.kind))} style={{ fontSize: typeScale.callout, color: t.accent, textAlign: 'start' }}>{tr('cloud.ownApp.howTo')}</Text>
        </>
      ))}
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
  const label = (p && p.label) || M.providerName(a, providerCache.get());
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
function Advanced({ a, st, p, verified, wide }) {
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
          <Text style={{ fontSize: typeScale.body, fontWeight: 600, color: t.text, textAlign: 'start' }}>{tr('cloud.ownApp.title')}</Text>
          {a.ownClient ? <Dim t={t}>{tr('cloud.ownApp.inUse')}</Dim> : null}
          <OwnAppChoice p={p || { kind: a.kind }} value={own} onChange={setOwn} row={wide} />
          <Actions>
            <Button small label={tr('cloud.signInAgain')} disabled={own.own && !own.id} onClick={() => beginAgain(a, p, ownOpts(own))} />
          </Actions>
        </View>
      ) : null}
    </View>
  );
}

// Starts a sign-in in the browser, once: lord-server opens the page itself (nothing here opens it again), a second
// start while one runs is ignored, a waiting sign-in for the same account is shown again instead of starting another,
// and one for another account is cancelled first. The waiting card is inline in the Cloud pane, so the browser stays usable.
let signinStarting = false;
export function startSignIn(kind, name, own, provider) {
  if (signinStarting) return;
  const cur = cloudState.get().signin;
  const same = cur && cur.phase === 'waiting' && cur.kind === kind && String(cur.name || '').toLowerCase() === String(name).toLowerCase() && M.toMs(cur.expiresAt) > Date.now();
  if (same) { setView({ sheet: null, signin: { provider, name, kind, own } }); return; }
  if (cur && cur.phase === 'waiting') cloud.cancelSignIn(cur.id);
  signinStarting = true;
  setView({ sheet: null, signin: { provider, name, kind, own, starting: true } });
  let r;
  try { r = cloud.beginSignIn(kind, name, own || {}); } finally { signinStarting = false; }
  setView({ sheet: null, signin: { provider, name, kind, own, id: r && r.id, error: r && r.error } });
}
function beginAgain(a, p, own = {}) {
  // { accountId }: exactly this account, whatever its name now (17.2 item 10)
  startSignIn(a.kind, a.name, { ...own, accountId: a.id }, p || { kind: a.kind, label: a.provider });
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
  const open = (q) => { if (q && (q.fields || []).length) setView({ sheet: { kind: 'cred', provider: q, name: a.name, again: true, accountId: a.id } }); };
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
  const win = useAppWindow();
  const website = useMemo(() => cloud.websiteUrl(a.id), [a.id]);
  const last = (st.recent || []).find((r) => r.account === a.id || M.accountOf(r.path, [a]));
  useStore(providerCache);
  const p = providerOf(a.kind, a);
  const verified = typeof (providerCache.get().find((x) => x.kind === a.kind) || {}).verified === 'boolean'
    ? providerCache.get().find((x) => x.kind === a.kind).verified : M.isVerified(a.kind, CLOUD_PROVIDERS);
  const line = M.stateLine(a, st.service);
  const stateText = tr(line.key, line.vars);
  const errText = a.error && !M.needsUnlock(a) ? errorText(M.explainError(a.error, st, a)) : '';
  // two balanced columns once the window is wide enough, one column otherwise
  const wide = (wm.frame(wm.get(win.id) || {}).w || 880) >= 900;
  const left = (
    <View style={{ flex: wide ? 1 : undefined, minWidth: 0, alignSelf: 'stretch', gap: 16 }}>
      <SpaceCard a={a} accounts={accs} />
      <KeptList a={a} onAdd={() => setView({ sheet: { kind: 'pickFolder', id: a.id } })} />
      <LinksList a={a} />
      {verified ? null : (
        <View style={[box(t), { paddingHorizontal: 14, paddingVertical: 12, gap: 6, border: `1px solid ${t.accent}` }]}>
          <Text style={{ fontSize: typeScale.body, fontWeight: 600, color: t.text, textAlign: 'start' }}>{tr('cloud.check.title')}</Text>
          <CheckAccount a={a} st={st} p={p} verified={false} />
        </View>
      )}
    </View>
  );
  const right = (
    <View aria-label={tr('cloud.options')} style={{ flex: wide ? 1 : undefined, minWidth: 0, alignSelf: 'stretch', gap: 12 }}>
      <Options a={a} accounts={accs} />
      <View style={[box(t), { paddingHorizontal: 14, paddingVertical: 12, gap: 4 }]}>
        <Text style={{ fontSize: typeScale.callout, color: t.textDim, whiteSpace: 'nowrap', overflow: 'hidden', textOverflow: 'ellipsis', textAlign: 'start' }}>{`${M.providerName(a, providerCache.get())} · ${verified ? tr('cloud.verified.yes') : tr('cloud.verified.no')}`}</Text>
        {/* the state once (Unlock Keyring… says it for a locked keyring), the account's own error only when it says more */}
        {M.needsUnlock(a) ? <UnlockRow a={a} /> : <Dim t={t} style={{ color: toneColor(t, line.tone) }}>{stateText}</Dim>}
        {errText && errText !== stateText ? <Dim t={t} style={{ color: tint.warn }}>{errText}</Dim> : null}
        <Prints prints={M.changedPrints(null, a)} />
        <Dim t={t}>{last ? tr('cloud.lastSynced', { when: (() => { const g = M.ago(last.at); return tr(g.key, g.vars); })() }) : tr('cloud.notSyncedYet')}</Dim>
        <Dim t={t}>{tr('cloud.inKeyring')}</Dim>
      </View>
      <View style={{ flexDirection: 'row', gap: 8 }}>
        <Button label={tr('cloud.signInAgain')} style={{ flex: 1 }} onClick={() => signInAgain(a)} />
        <View onClick={() => setView({ sheet: { kind: 'remove', id: a.id } })} style={{ flex: 1, height: 28, borderRadius: t.controlRadius, alignItems: 'center', justifyContent: 'center', backgroundColor: t.mode === 'dark' ? '#7a2230' : '#d6453d' }}>
          <Text style={{ fontSize: typeScale.body, fontWeight: 600, color: '#fff' }}>{tr('cloud.remove')}</Text>
        </View>
      </View>
      <Dim t={t} style={{ fontSize: typeScale.caption }}>{tr('cloud.removeHint')}</Dim>
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
        {left}
        {right}
      </View>
      <Advanced a={a} st={st} p={p} verified={verified} wide={wide} />
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
    if (!p || signinStarting) return;
    const bad = M.nameProblem(name, st.accounts);
    if (bad) { setErr(tr(`cloud.name.${bad}`)); return; }
    if (offerOwn && own.own && !own.id) { setErr(tr('cloud.ownApp.needId')); return; }
    if (p.group === 'oauth') startSignIn(p.kind, name.trim(), offerOwn ? ownOpts(own) : {}, p);
    else setView({ sheet: { kind: 'cred', provider: p, name: name.trim() } });
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
      <ScrollView style={{ maxHeight: offerOwn ? (own.own ? 150 : 230) : 380 }}>
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
          <Text style={{ fontSize: typeScale.callout, fontWeight: 600, color: t.text, textAlign: 'start' }}>{tr('cloud.ownApp.title')}</Text>
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

// a field's label: ours for the usual rclone option names, else the first clause of rclone's help (never the raw sentence)
const fieldLabel = (f) => tOptional(`cloud.field.${f.name}`) || M.shortLabel(f.help || f.label, f.name);

// One field of a provider's form, by its kind (M.fieldInput): a masked field only for a password, a switch for a
// yes/no, a menu for rclone's choices, else text
function FieldControl({ f, value, onChange, onSubmit }) {
  const how = M.fieldInput(f);
  if (how.input === 'bool') {
    const on = String(value || f.default || 'false') === 'true';
    return <Toggle small on={on} onChange={(v) => onChange(v ? 'true' : 'false')} />;
  }
  if (how.input === 'choice') {
    const opts = [...new Set([...how.options, ...(value ? [value] : [])])].map((x) => [x, x]);
    return how.options.length <= 4
      ? <Segmented small fill value={value || how.options[0]} onChange={onChange} options={opts.map(([v]) => [v, v.charAt(0).toUpperCase() + v.slice(1)])} />
      : <PopUp width={250} value={value || how.options[0]} onChange={onChange} options={opts} />;
  }
  return <TextInput secure={how.input === 'password'} value={value || ''} placeholder={f.examples && f.examples[0] && how.input !== 'password' ? String(f.examples[0]) : ''}
    style={{ direction: 'ltr' }} onChange={onChange} onSubmit={onSubmit} />;
}

// A server's or a bucket's form: the essential fields (M.formLayout), SFTP's sign-in (a password, or a key read from a
// file and sent as key_pem, so rclone never needs the file), and the rest under Advanced options. Its body scrolls
// inside the Settings window; the title and Back / Connect stay.
function CredSheet({ st, sheet }) {
  const t = useTheme();
  const { t: tr } = useLocale();
  const win = useAppWindow();
  const frameH = (wm.frame(wm.get(win.id) || {}).h) || 600;
  const p = sheet.provider;
  const layout = M.formLayout(p);
  const [vals, setVals] = useState(() => sheet.vals || M.formValues(p));
  const [name, setName] = useState(sheet.name || '');
  const [err, setErr] = useState(() => (sheet.error ? { text: errorMessage(sheet.error), field: M.errorField(sheet.error, p), prints: M.changedPrints(sheet.error) } : null));       // { text, field }
  const [busy, setBusy] = useState(false);
  const [adv, setAdv] = useState(false);
  const [auth, setAuth] = useState('password');
  const [keyFile, setKeyFile] = useState('');
  const [picking, setPicking] = useState(false);
  const set = (k, v) => { setVals((o) => ({ ...o, [k]: v })); setErr(null); };
  const back = () => setView({ sheet: sheet.again ? null : { kind: 'add' } });
  const pickKey = (path) => {
    setPicking(false);
    if (!path) return;
    try {
      const text = lord.fs.read(path, 'text');
      if (!/-----BEGIN [A-Z ]*PRIVATE KEY-----/.test(text)) { setErr({ text: tr('cloud.sftp.notKey'), field: '__key' }); return; }
      setKeyFile(path); set(layout.auth.key, M.pemForRclone(text));
    } catch (e) { setErr({ text: errorText(e), field: '__key' }); }
  };
  const connect = () => {
    const v = { ...vals };
    if (layout.auth) {
      if (auth === 'password') { delete v[layout.auth.key]; delete v[layout.auth.passphrase]; }
      else { delete v[layout.auth.password]; if (!v[layout.auth.key]) { setErr({ text: tr('cloud.sftp.chooseKey'), field: '__key' }); return; } }
    }
    const miss = M.missingField(p, v);
    if (miss && !(layout.auth && Object.values(layout.auth).includes(miss))) { setErr({ text: tr('cloud.field.required'), field: miss }); if (layout.advanced.some((f) => f.name === miss)) setAdv(true); return; }
    const bad = sheet.again ? null : M.nameProblem(name, st.accounts);
    if (bad) { setErr({ text: tr(`cloud.name.${bad}`), field: '__name' }); return; }
    setBusy(true);
    Promise.resolve(cloud.add(p.kind, name.trim(), M.cleanFields(p, v), sheet.accountId ? { accountId: sheet.accountId } : {})).then((r) => {
      setBusy(false);
      const field = r && r.error ? M.errorField(r, p) : null;
      if (r && r.error) { setErr({ text: errorMessage(r), field, prints: M.changedPrints(r) }); if (field && layout.advanced.some((f) => f.name === field)) setAdv(true); return; }
      // a question (a code, trusting the server) comes back here on an error, with what was typed
      afterAdd(r, p, name.trim(), { ...sheet, kind: 'cred', provider: p, name: name.trim(), vals: { ...vals }, error: null });
    });
  };
  const notes = M.notesFor(p.kind);
  // SFTP is about the server's key (U9), the others' servers about their password
  const note = p.group === 'object' ? tr('cloud.cred.noteObject') : p.group === 'crypt' ? tr('cloud.cred.noteCrypt') : p.kind === 'sftp' ? tr('cloud.cred.noteSftp') : tr('cloud.cred.noteServer');
  const row = (key, label, input, help) => (
    <View key={key} style={{ gap: 3 }}>
      <View style={{ flexDirection: 'row', alignItems: 'center', gap: 10 }}>
        <Text style={{ width: 130, fontSize: typeScale.body, color: t.textDim, textAlign: 'end' }}>{label}</Text>
        <View style={{ flex: 1, minWidth: 0, flexDirection: 'row', alignItems: 'center', borderRadius: 8, border: err && err.field === key ? `1px solid ${tint.error}` : '1px solid transparent' }}>{input}</View>
      </View>
      {help ? <Text style={{ marginInlineStart: 140, fontSize: typeScale.caption, color: t.textDim, lineHeight: 1.35, textAlign: 'start' }}>{help}</Text> : null}
    </View>
  );
  const control = (f) => <FieldControl f={f} value={vals[f.name]} onChange={(x) => set(f.name, x)} onSubmit={connect} />;
  const fieldRow = (f) => row(f.name, fieldLabel(f), <View style={{ flex: 1, minWidth: 0 }}>{control(f)}</View>);
  // an advanced option: its short label (a switch beside it), the control under it, rclone's whole help dimmed below
  const advRow = (f) => {
    const bool = M.fieldInput(f).input === 'bool';
    const help = String(f.help || '').trim();
    return (
      <View key={f.name} style={{ gap: 4, paddingVertical: 4, borderRadius: 8, border: err && err.field === f.name ? `1px solid ${tint.error}` : '1px solid transparent' }}>
        <View style={{ flexDirection: 'row', alignItems: 'center', gap: 10 }}>
          <Text style={{ flex: 1, minWidth: 0, fontSize: typeScale.body, color: t.text, textAlign: 'start' }}>{fieldLabel(f)}</Text>
          {bool ? control(f) : null}
        </View>
        {bool ? null : control(f)}
        {help ? <Text style={{ fontSize: typeScale.caption, color: t.textDim, lineHeight: 1.35, textAlign: 'start' }}>{help}</Text> : null}
        <Text style={{ fontSize: 10, color: t.textDim, fontFamily: 'monospace', direction: 'ltr', textAlign: 'start' }}>{f.name}</Text>
      </View>
    );
  };
  const byName = (n) => (p.fields || []).find((f) => f.name === n);
  return (
    <>
      <Sheet width={500} top={24} onClose={back} label={p.label}>
        <View style={{ flexDirection: 'row', alignItems: 'center', gap: 10, paddingHorizontal: 20, paddingTop: 18, paddingBottom: 10, borderBottomWidth: 1, borderBottomColor: t.separator }}>
          <View style={{ width: 34, height: 34, borderRadius: 9, alignItems: 'center', justifyContent: 'center', backgroundColor: M.providerColor(p) }}>
            <Text style={{ fontSize: 13, fontWeight: 700, color: '#fff' }}>{p.group === 'crypt' ? '◆' : M.providerMark(p.label)}</Text>
          </View>
          <View style={{ flex: 1, minWidth: 0 }}>
            <Text style={{ fontSize: typeScale.headline, fontWeight: 600, color: t.text, textAlign: 'start' }}>{p.label}</Text>
            {p.description || p.hint ? <Dim t={t}>{p.description || p.hint}</Dim> : null}
          </View>
        </View>
        <ScrollView style={{ maxHeight: Math.max(180, frameH - 220) }}>
          <View style={{ paddingHorizontal: 20, paddingVertical: 12, gap: 10 }}>
            {notes.length ? (
              <View style={{ paddingHorizontal: 12, paddingVertical: 10, gap: 4, borderRadius: 8, border: `1px solid ${tint.warn}` }}>
                <Text style={{ fontSize: typeScale.callout, fontWeight: 600, color: t.text, textAlign: 'start' }}>{tr('cloud.notes.before', { provider: p.label })}</Text>
                {notes.map((k) => <Dim key={k} t={t}>{`• ${tr(k)}`}</Dim>)}
              </View>
            ) : null}
            {layout.main.map((f) => fieldRow(f))}
            {layout.auth ? (
              <>
                {row('__auth', tr('cloud.sftp.signInWith'), <Segmented small fill value={auth} onChange={(x) => { setAuth(x); setErr(null); }}
                  options={[['password', tr('cloud.field.password')], ['key', tr('cloud.sftp.key')]]} />)}
                {auth === 'password'
                  ? (byName(layout.auth.password) ? fieldRow(byName(layout.auth.password)) : null)
                  : (
                    <>
                      {row('__key', tr('cloud.sftp.keyFile'), (
                        <View style={{ flex: 1, minWidth: 0, flexDirection: 'row', alignItems: 'center', gap: 8 }}>
                          <Text style={{ flex: 1, minWidth: 0, fontSize: typeScale.callout, color: keyFile ? t.text : t.textDim, whiteSpace: 'nowrap', overflow: 'hidden', direction: 'ltr', textAlign: 'start' }}>
                            {keyFile ? keyFile.split('/').pop() : tr('cloud.sftp.noKey')}</Text>
                          <Button small label={tr('cloud.sftp.chooseFile')} onClick={() => setPicking(true)} />
                        </View>
                      ), tr('cloud.sftp.keyHint'))}
                      {byName(layout.auth.passphrase) ? row(layout.auth.passphrase, tr('cloud.sftp.passphrase'),
                        <View style={{ flex: 1, minWidth: 0 }}><TextInput secure value={vals[layout.auth.passphrase] || ''} onChange={(x) => set(layout.auth.passphrase, x)} onSubmit={connect} /></View>) : null}
                    </>
                  )}
              </>
            ) : null}
            {sheet.again ? null : row('__name', tr('cloud.accountName'), <View style={{ flex: 1, minWidth: 0 }}><TextInput value={name} onChange={(v) => { setName(v.slice(0, M.NAME_MAX)); setErr(null); }} onSubmit={connect} /></View>)}
            {layout.advanced.length ? (
              <View style={{ gap: 10, marginTop: 4 }}>
                <View onClick={() => setAdv(!adv)} style={{ flexDirection: 'row', alignItems: 'center', gap: 6 }}>
                  <Text style={{ fontSize: 10, color: t.textDim }}>{adv ? '▼' : direction() === 'rtl' ? '◀' : '▶'}</Text>
                  <Text style={{ flex: 1, fontSize: typeScale.body, fontWeight: 600, color: t.text, textAlign: 'start' }}>{tr('cloud.advancedOptions')}</Text>
                  <Dim t={t}>{String(layout.advanced.length)}</Dim>
                </View>
                {adv ? layout.advanced.map(advRow) : null}
              </View>
            ) : null}
            {err && err.prints ? <View style={{ padding: 10, gap: 6, borderRadius: 8, border: `1px solid ${tint.error}` }}><Text style={{ fontSize: typeScale.callout, color: tint.error, textAlign: 'start' }}>{err.text}</Text><Prints prints={err.prints} /></View> : null}
            <View style={{ paddingHorizontal: 12, paddingVertical: 10, borderRadius: 8, backgroundColor: t.mode === 'dark' ? '#1e1e22' : '#f4f4f6' }}>
              <Dim t={t}>{note}</Dim>
            </View>
          </View>
        </ScrollView>
        <View style={{ flexDirection: 'row', alignItems: 'center', gap: 8, paddingHorizontal: 20, paddingTop: 12, paddingBottom: 16, borderTopWidth: 1, borderTopColor: t.separator }}>
          <Text style={{ flex: 1, fontSize: typeScale.callout, color: tint.error, textAlign: 'start' }}>{err && !err.prints ? err.text : ''}</Text>
          <Button label={tr('cloud.back')} onClick={back} />
          <Button primary label={tr(busy ? 'cloud.connecting' : 'cloud.connect')} disabled={busy} onClick={connect} />
        </View>
      </Sheet>
      {picking ? <FilePicker title={tr('cloud.sftp.pickKey')} start=".ssh" onClose={() => setPicking(false)} onPick={(x) => pickKey([].concat(x)[0])} /> : null}
    </>
  );
}

// What add/answer said (17.2): a question of rclone's (another step of the same sheet), the new account, or an error.
function afterAdd(r, p, name, back) {
  if (r && r.question) { setView({ sheet: { kind: 'question', question: r.question, provider: p, name, back } }); return; }
  if (r && r.error && back) { setView({ sheet: { ...back, error: r } }); return; }
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
  const [err, setErr] = useState(sheet.error ? errorMessage(sheet.error) : '');
  useEffect(() => { setValue(M.questionInput(sheet.question).value); setErr(sheet.error ? errorMessage(sheet.error) : ''); }, [sheet.question && sheet.question.state, sheet.error]);
  const close = () => setView({ sheet: sheet.back ? { ...sheet.back, error: null } : null });
  const go = () => {
    const ans = M.questionAnswer(q, value);
    if (ans == null) { setErr(tr('cloud.field.required')); return; }
    setBusy(true);
    cloud.answer(q.state, ans).then((r) => { setBusy(false); afterAdd(r, sheet.provider, sheet.name, sheet.back); });
  };
  // a trust question (17.2, e2e): the server's SSH host key or its certificate, by its fingerprint
  const trust = M.trustKind(q);
  const close2 = () => setView({ sheet: sheet.back ? { ...sheet.back, error: null } : null });
  if (trust) {
    const mono = { fontFamily: 'monospace', fontSize: 12, color: t.text, direction: 'ltr', textAlign: 'start', lineHeight: 1.4 };
    const fact = (k, v) => (v ? (
      <View key={k} style={{ flexDirection: 'row', gap: 10 }}>
        <Text style={{ width: 90, fontSize: typeScale.callout, color: t.textDim, textAlign: 'end' }}>{k}</Text>
        <Text style={{ flex: 1, minWidth: 0, ...mono }}>{String(v)}</Text>
      </View>) : null);
    const doTrust = () => { setBusy(true); cloud.answer(q.state, 'trust').then((r) => { setBusy(false); afterAdd(r, sheet.provider, sheet.name, sheet.back); }); };
    return (
      <Sheet width={480} top={60} onClose={close2} label={(sheet.provider || {}).label}>
        <View style={{ padding: spacing.xl, gap: spacing.m }}>
          <Text style={{ fontSize: typeScale.headline, fontWeight: 600, color: t.text, textAlign: 'start' }}>{tr(trust === 'hostKey' ? 'cloud.trust.hostTitle' : trust === 'hostKeyChanged' ? 'cloud.trust.changedTitle' : 'cloud.trust.certTitle')}</Text>
          <Text style={{ fontSize: typeScale.body, color: t.textDim, lineHeight: 1.45, textAlign: 'start' }}>
            {tr(trust === 'hostKey' ? 'cloud.trust.hostBody' : trust === 'hostKeyChanged' ? 'cloud.trust.changedBody' : 'cloud.trust.certBody', { host: `${q.host || tr('cloud.trust.server')}${q.port ? `:${q.port}` : ''}` })}</Text>
          {q.warning ? <Text style={{ fontSize: typeScale.callout, color: tint.warn, lineHeight: 1.45, textAlign: 'start' }}>{errorMessage(q.warning)}</Text> : null}
          <View style={{ gap: 6, padding: 12, borderRadius: 8, backgroundColor: t.mode === 'dark' ? '#1e1e22' : '#f4f4f6' }}>
            {fact(tr('cloud.trust.server'), q.host ? `${q.host}${q.port ? `:${q.port}` : ''}` : null)}
            {fact(tr('cloud.trust.fingerprint'), trust === 'hostKeyChanged' ? null : q.fingerprint)}
            {trust === 'hostKeyChanged' ? fact(tr('cloud.trust.old'), q.old) : null}
            {trust === 'hostKeyChanged' ? fact(tr('cloud.trust.new'), q.new || q.fingerprint) : null}
            {trust === 'certificate' ? fact(tr('cloud.trust.subject'), q.subject) : null}
            {trust === 'certificate' ? fact(tr('cloud.trust.issuer'), q.issuer) : null}
          </View>
          {err ? <Text style={{ fontSize: typeScale.callout, color: tint.error, textAlign: 'start' }}>{err}</Text> : null}
          <View style={{ flexDirection: 'row', justifyContent: 'flex-end', gap: spacing.s }}>
            <Button label={tr('cloud.cancel')} onClick={close2} />
            <Button primary label={tr(busy ? 'cloud.connecting' : trust === 'hostKey' ? 'cloud.trust.trust' : trust === 'hostKeyChanged' ? 'cloud.trust.trustNew' : 'cloud.trust.trustCert')} disabled={busy} onClick={doTrust} />
          </View>
        </View>
      </Sheet>
    );
  }
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
    cloud.emptyTrash(a.id).then((r) => { setBusy(false); setMsg(r && r.error ? { ok: false, text: errorMessage(r, a) } : { ok: true, text: tr('cloud.trash.done', { provider: M.providerName(a, providerCache.get()) }) }); });
  };
  return (
    <Sheet width={400} onClose={close} label={tr('cloud.trash.empty')}>
      <View style={{ padding: spacing.xl, gap: spacing.s }}>
        <Text style={{ fontSize: typeScale.headline, fontWeight: 600, color: t.text, textAlign: 'start' }}>{tr('cloud.trash.title', { name: a.name })}</Text>
        <Dim t={t} style={{ fontSize: typeScale.body }}>{tr('cloud.trash.detail', { provider: M.providerName(a, providerCache.get()) })}</Dim>
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

function SignInCard({ st, info }) {
  const t = useTheme();
  const { t: tr } = useLocale();
  const [, tick] = useState(0);
  const [showInFiles, setShowInFiles] = useState(true);
  const [keepAll, setKeepAll] = useState(false);
  const [s, set] = useSettings();
  useEffect(() => { const id = setInterval(() => tick((x) => x + 1), 1000); return () => clearInterval(id); }, []);
  const si = st.signin && (st.signin.id === info.id || String(st.signin.name || '').toLowerCase() === String(info.name || '').toLowerCase()) ? st.signin : null;
  const p = info.provider || {};
  const expired = si && si.phase === 'waiting' && M.toMs(si.expiresAt) <= Date.now();
  const failed = !info.starting && (info.error || (si && si.phase === 'failed') || expired || (!si && !info.id));
  const close = () => setView({ signin: null });
  const cancel = () => { if (si && si.phase === 'waiting') cloud.cancelSignIn(si.id); close(); };
  const card = (children, accent) => (
    <View aria-label={tr('cloud.signin.title', { provider: p.label || '' })} style={[box(t), { padding: 18, gap: 10, border: `1px solid ${accent || t.accent}` }]}>{children}</View>
  );
  if (si && si.phase === 'done') {
    const a = (st.accounts || []).find((x) => x.id === si.account || x.id === si.id || x.name === si.name);
    const finish = () => {
      if (a) {
        if (keepAll) cloud.keep([a.mount]);
        const hidden = s.cloudHidden || [];
        set({ cloudHidden: showInFiles ? hidden.filter((x) => x !== a.id) : [...new Set([...hidden, a.id])] });
      }
      cloud.cancelSignIn(si.id);       // a finished sign-in: clears state.signin (17.1)
    };
    return card(
      <>
        <View style={{ flexDirection: 'row', alignItems: 'center', gap: 10 }}>
          <View style={{ width: 28, height: 28, borderRadius: 14, backgroundColor: '#30d158', alignItems: 'center', justifyContent: 'center' }}>
            <Text style={{ fontSize: 14, fontWeight: 700, color: '#fff' }}>✓</Text>
          </View>
          <Text style={{ flex: 1, fontSize: typeScale.headline, fontWeight: 600, color: t.text, textAlign: 'start' }}>{tr('cloud.ready', { name: si.name })}</Text>
        </View>
        <Dim t={t} style={{ fontSize: typeScale.body }}>{`${a && a.about ? `${fmtSpace(a.about, tr)}. ` : ''}${tr('cloud.readyBody')}`}</Dim>
        <View style={{ flexDirection: 'row', alignItems: 'center', gap: 10 }}><Toggle small on={showInFiles} onChange={setShowInFiles} /><Text style={{ fontSize: typeScale.body, color: t.text }}>{tr('cloud.showInFiles')}</Text></View>
        <View style={{ flexDirection: 'row', alignItems: 'center', gap: 10 }}><Toggle small on={keepAll} onChange={setKeepAll} /><Text style={{ fontSize: typeScale.body, color: t.text }}>{tr('cloud.keepEverything')}</Text></View>
        <Actions>
          <Button label={tr('cloud.done')} onClick={() => { finish(); setView({ signin: null, page: a ? a.id : null }); }} />
          <Button primary label={tr('cloud.openInFiles')} onClick={() => { finish(); setView({ signin: null, page: a ? a.id : null }); if (a) wm.open('files', { new: true, args: { path: a.mount, nonce: Date.now() } }); }} />
        </Actions>
      </>, '#30d158');
  }
  const left = si && !failed ? M.countdown(si.expiresAt) : '';
  return card(
    <>
      <View style={{ flexDirection: 'row', alignItems: 'center', gap: 14 }}>
        <View style={{ width: 44, height: 44, alignItems: 'center', justifyContent: 'center', flexShrink: 0 }}>
          {failed ? null : <View style={{ position: 'absolute', left: 0, top: 0 }}><Spinner size={44} /></View>}
          <CloudGlyph kind="cloud" size={20} color={failed ? tint.warn : t.accent} />
        </View>
        <View style={{ flex: 1, minWidth: 0, gap: 3 }}>
          <Text style={{ fontSize: typeScale.headline, fontWeight: 600, color: t.text, textAlign: 'start' }}>{tr(failed ? 'cloud.signin.failedTitle' : 'cloud.signin.title', { provider: p.label || '' })}</Text>
          <Dim t={t} style={{ fontSize: typeScale.body, lineHeight: 1.45 }}>
            {failed ? (info.error ? errorText(info.error) : si && si.error ? errorText(si.error) : tr('cloud.signin.expired')) : tr('cloud.signin.body', { name: info.name })}</Dim>
        </View>
        {left ? <Text style={{ fontSize: typeScale.callout, color: t.textDim }}>{tr('cloud.signin.left', { time: left })}</Text> : null}
      </View>
      <Actions>
        <Button label={tr(failed ? 'cloud.close' : 'cloud.cancel')} onClick={cancel} />
        {failed
          ? <Button primary label={tr('cloud.signin.tryAgain')} onClick={() => startSignIn(info.kind || p.kind, info.name, info.own || {}, p)} />
          : <Button primary label={tr('cloud.signin.again')} disabled={!si || info.starting} onClick={() => si && cloud.openSignInPage(si.id)} />}
      </Actions>
      <Dim t={t} style={{ fontSize: typeScale.caption, lineHeight: 1.5 }}>{tr('cloud.signin.rclone')}</Dim>
      <Dim t={t} style={{ fontSize: typeScale.caption, lineHeight: 1.5 }}>{tr('cloud.signin.local')}</Dim>
    </>, failed ? tint.warn : null);
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
  // the providers' short names for the account lines (asked once in the background)
  useEffect(() => { if (cloud.available() && !providerCache.get().length) loadProviders().catch(() => {}); }, []);
  if (!cloud.available() || !st) {
    return <Stack><Note tone="warn">{tr('cloud.unavailable')}</Note></Stack>;
  }
  const a = v.page ? (st.accounts || []).find((x) => x.id === v.page) : null;
  return (
    <View style={{ gap: 16 }}>
      {v.signin ? <SignInCard st={st} info={v.signin} /> : null}
      {a ? <AccountPage a={a} st={st} /> : <Overview st={st} />}
    </View>
  );
}

// The Cloud pane's sheets, drawn by Settings at its window's root (index.jsx), over the window's content only.
export function CloudSheets() {
  const { t: tr } = useLocale();
  const st = useCloud();
  const v = useStore(cloudView);
  const sh = v.sheet;
  if (!sh || !st) return null;
  const a = sh.id ? (st.accounts || []).find((x) => x.id === sh.id) : null;
  return (
    <>
      {sh.kind === 'add' ? <AddSheet st={st} /> : null}
      {sh.kind === 'cred' ? <CredSheet st={st} sheet={sh} /> : null}
      {sh.kind === 'remove' ? <RemoveSheet st={st} sheet={sh} /> : null}
      {sh.kind === 'again' ? <AgainSheet st={st} sheet={sh} /> : null}
      {sh.kind === 'question' ? <QuestionSheet sheet={sh} /> : null}
      {sh.kind === 'emptyTrash' ? <EmptyTrashSheet st={st} sheet={sh} /> : null}
      {sh.kind === 'pickFolder' && a ? (
        <FilePicker mode="folder" start={a.mount} title={tr('cloud.addFolderTitle', { name: a.name })} onClose={() => setView({ sheet: null })}
          onPick={(p2) => { setView({ sheet: null }); const path = [].concat(p2)[0]; if (path && M.accountOf(path, [a])) cloud.keep([path]); }} />
      ) : null}
    </>
  );
}
