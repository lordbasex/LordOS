// Install LordOS: a wizard that installs the live system (ISO) on the disk.
// It only shows up in the live session ("live": true in the manifest). What needs root is done by
// the administration helper with closed actions (server/src/install): install-disks, install-run
// (validates everything again on the root side) and install-reboot. See docs/ISO.md.
// On the OVA image's first boot the same wizard, without the disk, makes the person's account on the
// running system (install-account): lord.system.info().firstAccount.
import {
  View, Text, Button, TextInput, ScrollView, Toggle, Segmented, SearchField, Hairline, GroupBox, FormRow, MenuButton, useTheme, useLocale, useStore, createStore,
  admin, keyboard, languages, setLanguage, formatBytes, locale, regionOf, localeOf, regionCountry, tOptional, wm, useAppWindow, useSystemInfo, typeScale, statusText, errorText, Lumo,
} from '@lordos/sdk';
import { useEffect, useState } from 'react';
import { emptyManual, actionsFor, setAction, addPartition, removeAdded, placeIn, rows as manualRows, review as manualReview } from './manualmodel.js';

export function Icon({ size: s }) {
  return (
    <View style={{ width: s, height: s, borderRadius: s * 0.225, background: 'linear-gradient(180deg, #5eead4, #0f766e)',
      boxShadow: `0 ${s * 0.04}px ${s * 0.12}px rgba(0,0,0,0.35)`, alignItems: 'center' }}>
      {/* down arrow */}
      <View style={{ position: 'absolute', top: s * 0.14, left: s * 0.44, width: s * 0.12, height: s * 0.3, borderRadius: s * 0.03, backgroundColor: '#fff' }} />
      <View style={{ position: 'absolute', top: s * 0.38, left: s * 0.35, width: s * 0.3, height: s * 0.09, borderRadius: s * 0.045, backgroundColor: '#fff' }} />
      <View style={{ position: 'absolute', top: s * 0.45, left: s * 0.42, width: s * 0.16, height: s * 0.07, borderRadius: s * 0.035, backgroundColor: '#fff' }} />
      {/* the disk */}
      <View style={{ position: 'absolute', bottom: s * 0.14, left: s * 0.16, right: s * 0.16, height: s * 0.22, borderRadius: s * 0.06, backgroundColor: '#fff' }}>
        <View style={{ position: 'absolute', right: s * 0.07, top: s * 0.08, width: s * 0.06, height: s * 0.06, borderRadius: s * 0.03, backgroundColor: '#22c55e' }} />
        <View style={{ position: 'absolute', left: s * 0.07, top: s * 0.095, width: s * 0.3, height: s * 0.03, borderRadius: s * 0.015, backgroundColor: '#99f6e4' }} />
      </View>
    </View>
  );
}

// Wizard state: it survives the UI remounting when the language changes.
const wiz = createStore({
  step: 0, lang: '', region: '', zone: '', kb: null, disk: null, mode: 'erase', confirmDisk: '', confirmBox: false, confirmFree: false, swap: 2048,
  manual: emptyManual(), confirmManual: false,
  fullname: '', user: '', userEdited: false, host: '', hostEdited: false, pass: '', pass2: '',
  progress: null, log: [], error: null, done: false,
});
const set = (p) => wiz.set((s) => ({ ...s, ...p }));

const RE_USER = /^[a-z_][a-z0-9_-]{0,31}$/;
const RE_HOST = /^[a-z0-9]([a-z0-9-]{0,61}[a-z0-9])?$/;
const RESERVED = ['root', 'daemon', 'bin', 'sys', 'sync', 'games', 'man', 'lp', 'mail', 'news', 'uucp', 'proxy', 'www-data',
  'backup', 'list', 'irc', 'nobody', 'systemd-network', 'messagebus', 'pipewire', 'sudo', 'admin'];
const ascii = (s) => s.normalize('NFD').replace(/[̀-ͯ]/g, '').toLowerCase();
const suggestUser = (full) => (ascii(full).split(/\s+/)[0] || '').replace(/[^a-z0-9_-]/g, '').slice(0, 32);

const FIRST_ACCOUNT = (() => { try { return !!lord.system.info().firstAccount; } catch (e) { return false; } })();
const STEPS = ['welcome', 'language', 'keyboard', ...(FIRST_ACCOUNT ? [] : ['disk']), 'user', 'summary', 'install'];

function Card({ children, style }) {
  const t = useTheme();
  return <View style={[{ padding: 16, borderRadius: 10, backgroundColor: t.hover, border: `1px solid ${t.separator}`, gap: 8 }, style]}>{children}</View>;
}

function Field({ label, children, hint, error }) {
  const t = useTheme();
  return (
    <View style={{ gap: 6, marginBottom: 16 }}>
      <Text style={{ fontSize: typeScale.callout, fontWeight: 600, color: t.text }}>{label}</Text>
      {children}
      {error ? <Text style={{ fontSize: typeScale.caption, color: '#ff453a', lineHeight: 1.35 }}>{error}</Text>
        : hint ? <Text style={{ fontSize: typeScale.caption, color: t.textDim, lineHeight: 1.35 }}>{hint}</Text> : null}
    </View>
  );
}

// A list with search (regions, zones, layouts).
function Pick({ items, value, onPick, placeholder, height = 170, limit = 300 }) {
  const t = useTheme();
  const [q, setQ] = useState('');
  const n = q.trim().toLowerCase();
  const shown = (n ? items.filter(([id, label]) => `${id} ${label}`.toLowerCase().includes(n)) : items).slice(0, limit);
  return (
    <View style={{ gap: 6 }}>
      <SearchField value={q} onChange={setQ} placeholder={placeholder} />
      <View style={{ height, borderRadius: 8, border: `1px solid ${t.separator}`, backgroundColor: t.mode === 'dark' ? 'rgba(255,255,255,0.03)' : 'rgba(0,0,0,0.02)' }}>
        <ScrollView style={{ padding: 4 }}>
          {shown.map(([id, label]) => (
            <View key={id} onClick={() => onPick(id)} style={{ height: 24, justifyContent: 'center', paddingHorizontal: 8, borderRadius: 5,
              backgroundColor: id === value ? t.accent : 'transparent' }}>
              <Text style={{ fontSize: typeScale.callout, color: id === value ? '#fff' : t.text, whiteSpace: 'nowrap' }}>{label}</Text>
            </View>
          ))}
        </ScrollView>
      </View>
    </View>
  );
}

// ---- steps ----

function Welcome() {
  const t = useTheme();
  const { t: tr } = useLocale();
  return (
    <View style={{ alignItems: 'center', gap: 12, paddingTop: 32 }}>
      <Lumo pose="wave" size={128} />
      <Text style={{ fontSize: 26, fontWeight: 600, color: t.text, marginTop: 8 }}>{tr('installer.welcome.title')}</Text>
      <Text style={{ fontSize: 13, color: t.textDim, textAlign: 'center', maxWidth: 480, lineHeight: 1.45 }}>{tr(FIRST_ACCOUNT ? 'installer.account.welcome' : 'installer.welcome.text')}</Text>
      {!admin.available() && <Text style={{ fontSize: 12, color: '#ff453a' }}>{tr('installer.needAdmin')}</Text>}
    </View>
  );
}

function Language() {
  const { t: tr } = useLocale();
  const s = useStore(wiz);
  const langs = languages();
  const [locales] = useState(() => [...new Set(lord.locale.available().map((l) => regionOf(l.name)))]);
  const [zones] = useState(() => lord.time.zones().sort((a, b) => (a === 'UTC' ? -1 : b === 'UTC' ? 1 : a.localeCompare(b))));
  const lang = s.lang || locale.get().lang.split('-')[0];
  // "United Kingdom · en_GB": found by the country's name too (#53)
  const regions = locales.filter((r) => r.startsWith(`${lang}_`))
    .map((r) => { const cc = regionCountry(r), name = cc ? tOptional(`countries.${cc}`) : ''; return [r, name ? `${name} · ${r}` : r]; })
    .sort((a, b) => a[1].localeCompare(b[1]));
  return (
    <View>
      <Field label={tr('installer.language.language')}>
        <Pick items={langs.map((l) => [l.id, l.english ? `${l.name} · ${l.english}` : l.name])} value={lang} height={100}
          placeholder={tr('installer.search')} onPick={(id) => { set({ lang: id, region: '' }); setLanguage(id); }} />
      </Field>
      <View style={{ flexDirection: 'row', gap: 16 }}>
        <View style={{ flex: 1, minWidth: 0 }}>
          <Field label={tr('installer.language.region')} hint={tr('installer.language.regionHint')}>
            <Pick items={regions} value={s.region} onPick={(region) => set({ region })} placeholder={tr('installer.search')} height={116} />
          </Field>
        </View>
        <View style={{ flex: 1, minWidth: 0 }}>
          <Field label={tr('installer.language.zone')}>
            <Pick items={zones.map((z) => [z, z.replace(/_/g, ' ')])} value={s.zone} onPick={(zone) => set({ zone })} placeholder={tr('installer.search')} height={116}
              limit={zones.length} />
          </Field>
        </View>
      </View>
    </View>
  );
}

function Keyboard() {
  const { t: tr } = useLocale();
  const s = useStore(wiz);
  const [sample, setSample] = useState('');
  const all = keyboard.list();
  const cur = s.kb || keyboard.active() || { layout: 'us', variant: '' };
  const id = cur.variant ? `${cur.layout}(${cur.variant})` : cur.layout;
  const all2 = all.flatMap((l) => [[l.id, l.name], ...l.variants.map((v) => [`${l.id}(${v.id})`, `${l.name} — ${v.name}`])]);
  const items = [...all2.filter(([k]) => k === id), ...all2.filter(([k]) => k !== id)];   // the current one first
  const pick = (v) => {
    const m = /^([\w-]+)(?:\(([\w-]+)\))?$/.exec(v);
    if (!m) return;
    const kb = { layout: m[1], variant: m[2] || '' };
    set({ kb });
    keyboard.set([kb]);                  // also in the live session, to try it
  };
  return (
    <View>
      <Field label={tr('installer.keyboard.layout')}>
        <Pick items={items} value={id} onPick={pick} placeholder={tr('installer.search')} height={230} />
      </Field>
      <Field label={tr('installer.keyboard.test')}>
        <TextInput value={sample} onChange={setSample} placeholder={tr('installer.keyboard.testHint')} />
      </Field>
    </View>
  );
}

function DiskStep() {
  const t = useTheme();
  const { t: tr } = useLocale();
  const s = useStore(wiz);
  const [disks, setDisks] = useState(null);
  const [err, setErr] = useState(null);
  const load = () => {
    const found = [];
    setDisks(null);
    admin.run('install-disks', '', (l) => { try { found.push(JSON.parse(l)); } catch (e) { /* plain text */ } }, { quiet: true })
      .then(() => {
        setDisks(found);
        // a new look at the disk asks for a new confirmation: what was confirmed may have changed
        const now = wiz.get().disk && found.find((d) => d.name === wiz.get().disk.name);
        if (wiz.get().disk) set({ disk: now && now.usable ? now : null, mode: now?.freeOk ? 'free' : 'erase', confirmDisk: '', confirmBox: false, confirmFree: false, manual: emptyManual(), confirmManual: false });
      }, (e) => { setErr(e.message); setDisks([]); });
  };
  useEffect(load, []);
  const content = (d) => (d.parts.length ? d.parts.map((p) => [p.label || p.name, p.fstype, formatBytes(p.size)].filter(Boolean).join(' ')).join(' · ')
    : tr('installer.disk.empty'));
  return (
    <View style={{ gap: 12 }}>
      {err && <Text style={{ fontSize: 12, color: '#ff453a' }}>{err}</Text>}
      {disks === null && <Text style={{ fontSize: 12, color: t.textDim }}>{tr('installer.disk.loading')}</Text>}
      {disks && disks.length === 0 && !err && <Text style={{ fontSize: 13, color: t.text }}>{tr('installer.disk.none')}</Text>}
      {(disks || []).map((d) => (
        <View key={d.name} onClick={() => d.usable && s.disk?.name !== d.name && set({ disk: d, mode: d.freeOk ? 'free' : 'erase', confirmDisk: '', confirmBox: false, confirmFree: false, manual: emptyManual(), confirmManual: false })}
          style={{ padding: 14, borderRadius: 10, gap: 4, opacity: d.usable ? 1 : 0.5,
            border: `2px solid ${s.disk?.name === d.name ? t.accent : t.separator}`, backgroundColor: t.hover }}>
          <View style={{ flexDirection: 'row', alignItems: 'center', gap: 10 }}>
            <Text style={{ flex: 1, fontSize: typeScale.body, fontWeight: 600, color: t.text }}>{[d.vendor, d.model].filter(Boolean).join(' ') || d.name}</Text>
            <Text style={{ fontSize: 13, fontWeight: 600, color: t.text }}>{formatBytes(d.size)}</Text>
          </View>
          <Text style={{ fontSize: 11, color: t.textDim }}>{`${d.path} · ${d.transport.toUpperCase()}${d.removable ? ` · ${tr('installer.disk.removable')}` : ''}`}</Text>
          <Text style={{ fontSize: 11, color: t.textDim }}>{content(d)}</Text>
          {!d.usable && <Text style={{ fontSize: 11, color: '#ff9f0a' }}>{tr('installer.disk.small')}</Text>}
        </View>
      ))}
      {s.disk && !s.disk.freeOk && s.disk.freeWhy && s.disk.ptable !== 'none' && (
        <Text style={{ fontSize: typeScale.caption, color: t.textDim, lineHeight: 1.35 }}>{tr('installer.disk.freeNo', { why: errorText(s.disk.freeWhy) })}</Text>
      )}
      {s.disk && !s.disk.manualOk && s.disk.manualWhy && s.disk.ptable !== 'none' && (
        <Text style={{ fontSize: typeScale.caption, color: t.textDim, lineHeight: 1.35 }}>{tr('installer.manual.none', { why: errorText(s.disk.manualWhy) })}</Text>
      )}
      {s.disk && (s.disk.freeOk || s.disk.manualOk) && (
        <Field label={tr('installer.disk.modeLabel')}>
          <Segmented value={s.mode} onChange={(mode) => set({ mode, confirmDisk: '', confirmBox: false, confirmFree: false, confirmManual: false })}
            options={[...(s.disk.freeOk ? [['free', tr('installer.disk.modeFree')]] : []), ...(s.disk.manualOk ? [['manual', tr('installer.disk.modeManual')]] : []),
              ['erase', tr('installer.disk.modeErase')]]} />
        </Field>
      )}
      {s.disk && s.mode === 'manual' && <ManualPanel />}
      {s.disk && s.mode === 'free' && (
        <Card style={{ borderColor: t.accent }}>
          <Text style={{ fontSize: typeScale.body, fontWeight: 600, color: t.text }}>{tr('installer.disk.freeTitle', { disk: s.disk.path })}</Text>
          <Text style={{ fontSize: 12, color: t.text, lineHeight: 1.4 }}>{tr('installer.disk.freeText', { size: formatBytes(s.disk.freeBytes) })}</Text>
          <Text style={{ fontSize: 12, color: t.text, lineHeight: 1.4 }}>{tr('installer.disk.freeKeep', { parts: content(s.disk), esp: s.disk.espPart })}</Text>
          <Text style={{ fontSize: 12, color: t.textDim, lineHeight: 1.4 }}>{tr('installer.disk.freeBoot')}</Text>
          <View style={{ flexDirection: 'row', alignItems: 'center', gap: 10 }}>
            <Toggle on={s.confirmFree} onChange={(confirmFree) => set({ confirmFree })} />
            <Text style={{ fontSize: 12, color: t.text }}>{tr('installer.disk.freeUnderstand')}</Text>
          </View>
          <Field label={tr('installer.disk.swap')} hint={tr('installer.disk.swapHint')}>
            <Segmented value={String(s.swap)} onChange={(v) => set({ swap: Number(v) })}
              options={[['0', tr('installer.disk.swapNone')], ['2048', '2 GB'], ['4096', '4 GB'], ['8192', '8 GB']]} />
          </Field>
        </Card>
      )}
      {s.disk && s.mode === 'erase' && (
        <Card style={{ borderColor: '#ff453a' }}>
          <Text style={{ fontSize: typeScale.body, fontWeight: 600, color: '#ff453a' }}>{tr('installer.disk.eraseTitle', { disk: s.disk.path })}</Text>
          <Text style={{ fontSize: 12, color: t.text, lineHeight: 1.4 }}>{tr('installer.disk.eraseText')}</Text>
          <View style={{ flexDirection: 'row', alignItems: 'center', gap: 10 }}>
            <Toggle on={s.confirmBox} onChange={(confirmBox) => set({ confirmBox })} />
            <Text style={{ fontSize: 12, color: t.text }}>{tr('installer.disk.understand')}</Text>
          </View>
          <Field label={tr('installer.disk.typeName', { name: s.disk.name })}>
            <TextInput value={s.confirmDisk} onChange={(confirmDisk) => set({ confirmDisk: confirmDisk.trim() })} placeholder={s.disk.name} />
          </Field>
          <Field label={tr('installer.disk.swap')} hint={tr('installer.disk.swapHint')}>
            <Segmented value={String(s.swap)} onChange={(v) => set({ swap: Number(v) })}
              options={[['0', tr('installer.disk.swapNone')], ['2048', '2 GB'], ['4096', '4 GB'], ['8192', '8 GB']]} />
          </Field>
        </Card>
      )}
      <Button label={tr('installer.disk.reload')} onClick={load} style={{ alignSelf: 'flex-start' }} />
    </View>
  );
}

// ---- manual partitioning (docs/ISO.md, "Manual partitioning"; the model is manualmodel.js) ----

const partText = (d, p) => [p.name, formatBytes((p.end - p.start + 1) * d.lsec), p.fs || p.fstype, p.label].filter(Boolean).join(' · ');
const FS_OF = { root: 'ext4', home: 'ext4', swap: 'swap', esp: 'FAT32' };

// Every change of a manual plan, as the person reads it (the disk step and the summary).
export function manualChanges(d, m, tr) {
  const r = manualReview(d, m, 0);
  return r.changes.map((c) => {
    if (c.kind === 'delete') return tr('installer.manual.ch.delete', { part: partText(d, c.part) });
    if (c.kind === 'create') return tr('installer.manual.ch.create', { size: formatBytes(c.bytes), role: tr(`installer.manual.role.${c.role}`), fs: FS_OF[c.role] });
    return tr(`installer.manual.ch.${c.act}`, { part: partText(d, c.part) });
  });
}

function ManualRow({ d, row, m }) {
  const t = useTheme();
  const { t: tr } = useLocale();
  const [adding, setAdding] = useState(false);
  const [role, setRole] = useState('root');
  const [size, setSize] = useState('');
  const line = { flexDirection: 'row', alignItems: 'center', gap: 10, paddingVertical: 8, paddingHorizontal: 12 };
  if (row.kind === 'part') {
    const p = row.p, gone = row.act === 'delete';
    const trigger = () => (
      <View style={{ height: 24, paddingHorizontal: 10, borderRadius: 6, justifyContent: 'center', backgroundColor: row.act === 'keep' ? t.control : gone ? '#ff453a' : t.accent }}>
        <Text style={{ fontSize: 12, color: row.act === 'keep' ? t.text : '#fff', whiteSpace: 'nowrap' }}>{`${tr(`installer.manual.act.${row.act}`)} ▾`}</Text>
      </View>
    );
    return (
      <View style={line}>
        <View style={{ flex: 1, minWidth: 0, gap: 2, opacity: gone ? 0.5 : 1 }}>
          <Text style={{ fontSize: typeScale.callout, fontWeight: 600, color: t.text, whiteSpace: 'nowrap', overflow: 'hidden' }}>{partText(d, p)}</Text>
          <Text style={{ fontSize: 11, color: t.textDim, whiteSpace: 'nowrap', overflow: 'hidden' }}>
            {[p.esp ? tr('installer.manual.espBadge') : null, p.loaders?.length ? tr('installer.manual.loaders', { list: p.loaders.join(', ') }) : null].filter(Boolean).join(' · ') || ' '}</Text>
        </View>
        <MenuButton trigger={trigger} minWidth={260} items={actionsFor(p).map((a) => ({
          label: tr(`installer.manual.act.${a}`), checked: a === row.act, destructive: a === 'delete',
          onClick: () => set({ manual: setAction(wiz.get().manual, p.num, a), confirmManual: false }),
        }))} />
      </View>
    );
  }
  if (row.kind === 'new') {
    return (
      <View style={[line, { backgroundColor: t.mode === 'dark' ? 'rgba(48,209,88,0.10)' : 'rgba(48,209,88,0.08)' }]}>
        <View style={{ flex: 1, minWidth: 0 }}>
          <Text style={{ fontSize: typeScale.callout, fontWeight: 600, color: t.text }}>
            {`${tr('installer.manual.new')} · ${formatBytes(row.bytes)} · ${tr(`installer.manual.role.${row.a.role}`)} · ${FS_OF[row.a.role]}`}</Text>
        </View>
        <Button small label={tr('installer.manual.remove')} onClick={() => set({ manual: removeAdded(wiz.get().manual, row.index), confirmManual: false })} />
      </View>
    );
  }
  // free space: create a partition in it
  const used = new Set([...Object.values(m.acts).map((a) => ({ root: 'root', home: 'home', homeFormat: 'home', swap: 'swap', esp: 'esp', espFormat: 'esp' }[a])), ...m.adds.map((a) => a.role)]);
  const create = () => {
    const mib = Number(String(size).replace(',', '.')) * 1024;
    if (!Number.isFinite(mib) || mib < 0) return;
    const at = placeIn(d, row.gap, mib);
    set({ manual: addPartition(wiz.get().manual, role, at.start, at.end), confirmManual: false });
    setAdding(false); setSize('');
  };
  return (
    <View style={{ gap: 8, paddingVertical: 8, paddingHorizontal: 12 }}>
      <View style={{ flexDirection: 'row', alignItems: 'center', gap: 10 }}>
        <Text style={{ flex: 1, fontSize: typeScale.callout, color: t.textDim }}>{`${tr('installer.manual.free')} · ${formatBytes(row.bytes)}`}</Text>
        {!adding && row.bytes >= 32 * 1024 * 1024 && <Button small label={tr('installer.manual.add')} onClick={() => {
          setRole(['root', 'esp', 'home', 'swap'].find((r) => !used.has(r)) || 'root');
          setAdding(true);
        }} />}
      </View>
      {adding && (
        <View style={{ gap: 8 }}>
          <Segmented small value={role} onChange={setRole} options={['root', 'home', 'swap', 'esp'].map((r) => [r, tr(`installer.manual.role.${r}`)])} />
          <View style={{ flexDirection: 'row', alignItems: 'center', gap: 8 }}>
            <Text style={{ fontSize: 12, color: t.text }}>{tr('installer.manual.size')}</Text>
            <TextInput value={size} onChange={setSize} placeholder={formatBytes(row.bytes)} style={{ width: 110 }} />
            <Button small primary label={tr('installer.manual.create')} onClick={create} />
            <Button small label={tr('installer.manual.cancel')} onClick={() => setAdding(false)} />
          </View>
          <Text style={{ fontSize: 11, color: t.textDim }}>{tr('installer.manual.sizeHint', { max: formatBytes(row.bytes) })}</Text>
        </View>
      )}
    </View>
  );
}

function ManualPanel() {
  const t = useTheme();
  const { t: tr } = useLocale();
  const s = useStore(wiz);
  const d = s.disk, m = s.manual;
  const r = manualReview(d, m, 0);        // a swap partition drops the swap file (startInstall)
  const list = manualRows(d, m);
  const changes = manualChanges(d, m, tr);
  return (
    <Card style={{ borderColor: t.accent, gap: 10 }}>
      <Text style={{ fontSize: typeScale.body, fontWeight: 600, color: t.text }}>{tr('installer.manual.title', { disk: d.path })}</Text>
      <Text style={{ fontSize: 12, color: t.text, lineHeight: 1.4 }}>{tr('installer.manual.intro')}</Text>
      <View style={{ borderRadius: 8, border: `1px solid ${t.separator}` }}>
        {list.map((row, i) => (
          <View key={`${row.kind}-${row.start}-${i}`}>
            {i > 0 && <View style={{ height: 1, backgroundColor: t.separator }} />}
            <ManualRow d={d} row={row} m={m} />
          </View>
        ))}
      </View>
      {r.where.swap ? <Text style={{ fontSize: 12, color: t.textDim }}>{tr('installer.manual.swapByPart')}</Text> : (
        <Field label={tr('installer.disk.swap')} hint={tr('installer.disk.swapHint')}>
          <Segmented value={String(s.swap)} onChange={(v) => set({ swap: Number(v) })}
            options={[['0', tr('installer.disk.swapNone')], ['2048', '2 GB'], ['4096', '4 GB'], ['8192', '8 GB']]} />
        </Field>
      )}
      <Text style={{ fontSize: typeScale.callout, fontWeight: 600, color: t.text }}>{tr('installer.manual.changes')}</Text>
      {changes.length ? changes.map((c, i) => <Text key={i} style={{ fontSize: 12, color: t.text, lineHeight: 1.4 }}>{`• ${c}`}</Text>)
        : <Text style={{ fontSize: 12, color: t.textDim }}>{tr('installer.manual.noChanges')}</Text>}
      <Text style={{ fontSize: 12, color: t.textDim, lineHeight: 1.4 }}>{tr('installer.manual.rest')}</Text>
      {r.errors.map((e) => <Text key={e} style={{ fontSize: 12, color: '#ff9f0a', lineHeight: 1.4 }}>{errorText(e)}</Text>)}
      {!r.errors.length && (
        <View style={{ gap: 10 }}>
          <View style={{ flexDirection: 'row', alignItems: 'center', gap: 10 }}>
            <Toggle on={s.confirmManual} onChange={(confirmManual) => set({ confirmManual })} />
            <Text style={{ fontSize: 12, color: t.text }}>{tr('installer.manual.understand')}</Text>
          </View>
          {r.destructive && (
            <Field label={tr('installer.disk.typeName', { name: d.name })}>
              <TextInput value={s.confirmDisk} onChange={(confirmDisk) => set({ confirmDisk: confirmDisk.trim() })} placeholder={d.name} />
            </Field>
          )}
        </View>
      )}
    </Card>
  );
}

const userErrors = (s, tr) => ({
  fullname: !s.fullname.trim() ? tr('installer.user.errFull') : /[:,\n]/.test(s.fullname) ? tr('installer.user.errFullChars') : null,
  user: !RE_USER.test(s.user) ? tr('installer.user.errUser') : RESERVED.includes(s.user) || (FIRST_ACCOUNT && s.user === 'lordos') ? tr('installer.user.errReserved') : null,
  pass: !s.pass ? tr('installer.user.errPass') : null,
  pass2: s.pass !== s.pass2 ? tr('installer.user.errPass2') : null,
  host: !RE_HOST.test(s.host) ? tr('installer.user.errHost') : null,
});

function UserStep() {
  const { t: tr } = useLocale();
  const s = useStore(wiz);
  const e = userErrors(s, tr);
  const show = (k) => (s[k] || (k === 'pass2' && s.pass) ? e[k] : null);
  return (
    <View>
      <Field label={tr('installer.user.fullname')} error={show('fullname')}>
        <TextInput value={s.fullname} onChange={(fullname) => {
          const user = s.userEdited ? s.user : suggestUser(fullname);
          set({ fullname, user, host: s.hostEdited ? s.host : (user ? `${user}-pc` : '') });
        }} placeholder={tr('installer.user.fullnameHint')} />
      </Field>
      <Field label={tr('installer.user.user')} hint={tr('installer.user.userHint')} error={show('user')}>
        <TextInput value={s.user} onChange={(user) => set({ user: user.toLowerCase(), userEdited: true })} />
      </Field>
      <View style={{ flexDirection: 'row', gap: 12 }}>
        <View style={{ flex: 1 }}>
          <Field label={tr('installer.user.pass')} error={show('pass')}>
            <TextInput secure value={s.pass} onChange={(pass) => set({ pass })} />
          </Field>
        </View>
        <View style={{ flex: 1 }}>
          <Field label={tr('installer.user.pass2')} error={show('pass2')}>
            <TextInput secure value={s.pass2} onChange={(pass2) => set({ pass2 })} />
          </Field>
        </View>
      </View>
      <Field label={tr('installer.user.host')} hint={tr('installer.user.hostHint')} error={show('host')}>
        <TextInput value={s.host} onChange={(host) => set({ host: host.toLowerCase(), hostEdited: true })} />
      </Field>
    </View>
  );
}

// Layout name for the summary ("Spanish (Latin American)"), as xkb gives it.
const kbName = (kb) => {
  const l = keyboard.list().find((x) => x.id === kb.layout);
  const v = l?.variants.find((x) => x.id === kb.variant);
  return v ? `${l.name} — ${v.name}` : l ? l.name : kb.layout;
};

const plan = (s) => {
  const lang = s.lang || locale.get().lang.split('-')[0];
  const region = s.region || (locale.get().region && locale.get().region.startsWith(`${lang}_`) ? locale.get().region : '');
  const kb = s.kb || keyboard.active() || { layout: 'us', variant: '' };
  return {
    lang, region: region || ({ es: 'es_ES', en: 'en_US', pt: 'pt_BR', fr: 'fr_FR', it: 'it_IT' }[lang] || 'en_US'),
    zone: s.zone || lord.time.zone() || 'UTC', kb,
  };
};

function Summary() {
  const t = useTheme();
  const { t: tr } = useLocale();
  const s = useStore(wiz);
  const p = plan(s);
  const arm64 = useSystemInfo(60000).arch === 'aarch64';     // UEFI only: no BIOS boot partition (install.c)
  const beside = s.mode === 'free', manual = s.mode === 'manual';
  const mr = manual && s.disk ? manualReview(s.disk, s.manual, 0) : null;
  const rows = [
    ...(FIRST_ACCOUNT ? [] : [
      [tr('installer.summary.disk'), `${s.disk?.path} (${formatBytes(s.disk?.size || 0)}) — ${tr(beside ? 'installer.summary.kept' : manual ? 'installer.summary.manual' : 'installer.summary.erased')}`],
      [tr('installer.summary.layout'), manual ? [...manualChanges(s.disk, s.manual, tr), tr('installer.manual.rest')]
        : beside ? tr('installer.summary.layoutFree', { size: formatBytes(s.disk?.freeBytes || 0) }) : tr(arm64 ? 'installer.summary.layoutValueEfi' : 'installer.summary.layoutValue')],
      [tr('installer.summary.swap'), mr?.where.swap ? tr('installer.summary.swapPart') : s.swap ? `${s.swap / 1024} GB` : tr('installer.disk.swapNone')],
    ]),
    [tr('installer.summary.lang'), `${p.lang} · ${localeOf(p.region)} · ${p.zone}`],
    [tr('installer.summary.keyboard'), kbName(p.kb)],
    [tr('installer.summary.user'), `${s.fullname} · ${s.user} · ${tr('installer.summary.admin')}`],
    [tr('installer.summary.host'), s.host],
  ];
  return (
    <View style={{ gap: 12 }}>
      <GroupBox>
        {rows.map(([k, v]) => (
          <FormRow key={k} label={k} labelWidth={140} minHeight={34}>
            {Array.isArray(v) ? <View style={{ flex: 1, gap: 3, paddingVertical: 6 }}>{v.map((x, i) => <Text key={i} style={{ fontSize: typeScale.callout, color: t.text, lineHeight: 1.35 }}>{x}</Text>)}</View>
              : <Text style={{ fontSize: typeScale.callout, color: t.text, lineHeight: 1.35 }}>{v}</Text>}
          </FormRow>
        ))}
      </GroupBox>
      <Text style={{ fontSize: typeScale.caption, color: t.textDim, lineHeight: 1.4, paddingHorizontal: 8 }}>{tr('installer.summary.login')}</Text>
    </View>
  );
}

const PHASES = [['partition', 0.05], ['copy', 0.7], ['configure', 0.15], ['bootloader', 0.1]];
const overall = (pr) => {
  if (!pr) return 0;
  let base = 0;
  for (const [ph, w] of PHASES) {
    if (ph === pr.phase) return base + w * (pr.total ? pr.done / pr.total : 0);
    base += w;
  }
  return base;
};

function Install() {
  const t = useTheme();
  const { t: tr } = useLocale();
  const s = useStore(wiz);
  const f = s.done ? 1 : overall(s.progress);
  return (
    <View style={{ gap: 14, paddingTop: 32, alignItems: 'center' }}>
      <Icon size={72} />
      <Text style={{ fontSize: typeScale.largeTitle, fontWeight: 600, color: t.text }}>
        {s.done ? tr(FIRST_ACCOUNT ? 'installer.account.doneTitle' : 'installer.done.title') : s.error ? tr('installer.error.title') : tr(FIRST_ACCOUNT ? 'installer.account.working' : 'installer.install.title')}</Text>
      {!s.error && (
        <View style={{ width: 460, gap: 6 }}>
          <View style={{ height: 8, borderRadius: 4, backgroundColor: t.control, direction: 'ltr' }}>
            <View style={{ width: `${Math.round(f * 100)}%`, height: 8, borderRadius: 4, backgroundColor: s.done ? '#30d158' : t.accent }} />
          </View>
          <Text style={{ fontSize: 12, color: t.textDim, textAlign: 'center' }}>
            {s.done ? tr('installer.done.text') : `${s.progress ? tr(`installer.phase.${s.progress.phase}`) : tr('installer.phase.start')} · ${Math.round(f * 100)} %`}</Text>
        </View>
      )}
      {s.error && <Text style={{ fontSize: 12, color: '#ff453a', textAlign: 'center', maxWidth: 480 }}>{s.error}</Text>}
      <View style={{ width: 520, gap: 2 }}>
        {s.log.slice(-4).map((l, i) => <Text key={i} style={{ fontSize: 11, color: t.textDim, whiteSpace: 'nowrap', overflow: 'hidden' }}>{statusText(l)}</Text>)}
      </View>
      {s.done && <Text style={{ fontSize: 12, color: t.text, maxWidth: 480, textAlign: 'center' }}>{tr(FIRST_ACCOUNT ? 'installer.account.doneText' : 'installer.done.usb')}</Text>}
    </View>
  );
}

function startInstall() {
  const s = wiz.get();
  const p = plan(s);
  set({ step: STEPS.indexOf('install'), progress: null, log: [], error: null, done: false });
  const job = FIRST_ACCOUNT ? {
    user: s.user, fullname: s.fullname.trim(), password: s.pass, hostname: s.host,
    lang: p.lang, locale: localeOf(p.region), timezone: p.zone, kblayout: p.kb.layout, kbvariant: p.kb.variant || '',
  } : {
    mode: s.mode, ...(s.mode === 'free' ? { freeStart: s.disk.freeStart, freeEnd: s.disk.freeEnd } : {}),
    ...(s.mode === 'manual' ? manualReview(s.disk, s.manual, 0).fields : {}),
    disk: s.disk.name, user: s.user, fullname: s.fullname.trim(), password: s.pass, hostname: s.host,
    lang: p.lang, locale: localeOf(p.region), timezone: p.zone, kblayout: p.kb.layout, kbvariant: p.kb.variant || '',
    swap: s.mode === 'manual' && manualReview(s.disk, s.manual, 0).where.swap ? 0 : s.swap,
  };
  set({ pass: '', pass2: '' });            // the password doesn't stay in the UI
  admin.run(FIRST_ACCOUNT ? 'install-account' : 'install-run', JSON.stringify(job), (line) => {
    const m = /^progress (\w+) (\d+) (\d+)$/.exec(line);
    if (m) set({ progress: { phase: m[1], done: Number(m[2]), total: Number(m[3]) } });
    else wiz.set((st) => ({ ...st, log: [...st.log, line].slice(-30) }));
  }).then(() => set({ done: true }), (e) => set({ error: e.message }));
}

// The manual plan has no problems left, was confirmed, and a plan that loses data also has the disk's name typed.
const manualReady = (s) => {
  const r = manualReview(s.disk, s.manual, 0);
  return !r.errors.length && s.confirmManual && (!r.destructive || s.confirmDisk === s.disk.name);
};

export default function Installer() {
  const t = useTheme();
  const { t: tr } = useLocale();
  const { id } = useAppWindow();
  const s = useStore(wiz);
  const name = STEPS[s.step];
  // the first boot isn't over until the account exists: without it there is nothing to log in with
  useEffect(() => wm.setCloseGuard(id, () => (FIRST_ACCOUNT ? s.done : !(name === 'install' && !s.done && !s.error))), [id, name, s.done, s.error]);
  useEffect(() => { if (FIRST_ACCOUNT) wm.setTitle(id, tr('installer.account.title')); }, [id, tr]);
  const e = userErrors(s, tr);
  const canNext = {
    welcome: admin.available(), language: true, keyboard: true,
    disk: s.mode === 'free' ? !!s.disk?.freeOk && s.confirmFree
      : s.mode === 'manual' ? !!s.disk?.manualOk && manualReady(s)
        : !!s.disk && s.confirmBox && s.confirmDisk === s.disk.name,
    user: !Object.values(e).some(Boolean), summary: true,
  }[name];
  const Body = { welcome: Welcome, language: Language, keyboard: Keyboard, disk: DiskStep, user: UserStep, summary: Summary, install: Install }[name];
  const steps = STEPS.slice(1, -1);
  return (
    <View style={{ flex: 1, flexDirection: 'row' }}>
      <View style={{ width: 200, backgroundColor: t.sidebarBg, paddingHorizontal: 16, paddingTop: 20, gap: 4 }}>
        <Hairline edge="end" />
        <Text style={{ fontSize: typeScale.body, fontWeight: 600, color: t.text, marginBottom: 12, paddingHorizontal: 4 }}>{tr(FIRST_ACCOUNT ? 'installer.account.title' : 'installer.title')}</Text>
        {steps.map((st) => {
          const i = STEPS.indexOf(st);
          const cur = i === s.step, past = i < s.step;
          return (
            <View key={st} style={{ flexDirection: 'row', alignItems: 'center', gap: 10, height: 30, paddingHorizontal: 4, borderRadius: 6,
              backgroundColor: cur ? t.hover : 'transparent' }}>
              <View style={{ width: 20, height: 20, borderRadius: 10, alignItems: 'center', justifyContent: 'center',
                backgroundColor: cur ? t.accent : past ? '#30d158' : t.control }}>
                <Text style={{ fontSize: 11, fontWeight: 600, color: cur || past ? '#fff' : t.textDim }}>{past ? '✓' : String(i)}</Text>
              </View>
              <Text style={{ flex: 1, fontSize: typeScale.body, fontWeight: cur ? 600 : 400, color: cur ? t.text : t.textDim, whiteSpace: 'nowrap', overflow: 'hidden' }}>{tr(`installer.step.${st}`)}</Text>
            </View>
          );
        })}
      </View>
      <View style={{ flex: 1, minWidth: 0 }}>
        <ScrollView style={{ flex: 1, paddingHorizontal: 24, paddingTop: 20 }}>
          {name !== 'welcome' && name !== 'install' && <Text style={{ fontSize: typeScale.largeTitle, fontWeight: 600, color: t.text, marginBottom: 16 }}>{tr(`installer.step.${name}`)}</Text>}
          <Body />
        </ScrollView>
        <View style={{ flexDirection: 'row', justifyContent: 'flex-end', gap: 8, paddingHorizontal: 16, paddingVertical: 14 }}>
          <Hairline edge="top" />
          {name !== 'install' && s.step > 0 && <Button label={tr('installer.back')} onClick={() => set({ step: s.step - 1 })} />}
          {name === 'summary' && <Button primary label={tr(FIRST_ACCOUNT ? 'installer.account.create' : 'installer.summary.install')} onClick={startInstall} />}
          {name !== 'summary' && name !== 'install' && <Button primary disabled={!canNext} label={tr('installer.next')} onClick={() => canNext && set({ step: s.step + 1 })} />}
          {name === 'install' && s.error && <Button label={tr('installer.back')} onClick={() => set({ step: STEPS.indexOf('summary'), error: null })} />}
          {name === 'install' && s.done && <Button primary label={s.rebooting ? tr('installer.done.rebooting') : tr('installer.done.reboot')}
            onClick={() => { if (!s.rebooting) { set({ rebooting: true }); admin.run('install-reboot').catch((e) => set({ rebooting: false, error: e.message })); } }} />}
        </View>
      </View>
    </View>
  );
}
