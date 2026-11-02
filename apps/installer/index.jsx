// Install LordOS: a wizard that installs the live system (ISO) on the disk.
// It only shows up in the live session ("live": true in the manifest). What needs root is done by
// the administration helper with closed actions (server/src/install): install-disks, install-run
// (validates everything again on the root side) and install-reboot. See docs/ISO.md.
// On the OVA image's first boot the same wizard, without the disk, makes the person's account on the
// running system (install-account): lord.system.info().firstAccount.
import {
  View, Text, Button, TextInput, ScrollView, Toggle, Segmented, SearchField, Hairline, GroupBox, FormRow, useTheme, useLocale, useStore, createStore,
  admin, keyboard, languages, setLanguage, formatBytes, locale, wm, useAppWindow, useSystemInfo, typeScale, statusText, errorText, Lumo,
} from '@lordos/sdk';
import { useEffect, useState } from 'react';

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
function Pick({ items, value, onPick, placeholder, height = 170 }) {
  const t = useTheme();
  const [q, setQ] = useState('');
  const n = q.trim().toLowerCase();
  const shown = (n ? items.filter(([id, label]) => `${id} ${label}`.toLowerCase().includes(n)) : items).slice(0, 300);
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
  const [locales] = useState(() => lord.locale.available().map((l) => l.name.replace(/\.UTF-8$/i, '')));
  const [zones] = useState(() => lord.time.zones());
  const lang = s.lang || locale.get().lang.split('-')[0];
  const regions = locales.filter((r) => r.startsWith(`${lang}_`)).map((r) => [r, r]);
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
            <Pick items={zones.map((z) => [z, z.replace(/_/g, ' ')])} value={s.zone} onPick={(zone) => set({ zone })} placeholder={tr('installer.search')} height={116} />
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
        if (wiz.get().disk) set({ disk: now && now.usable ? now : null, mode: now?.freeOk ? 'free' : 'erase', confirmDisk: '', confirmBox: false, confirmFree: false });
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
        <View key={d.name} onClick={() => d.usable && set({ disk: d, mode: d.freeOk ? 'free' : 'erase', confirmDisk: '', confirmBox: false, confirmFree: false })}
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
      {s.disk?.freeOk && (
        <Field label={tr('installer.disk.modeLabel')}>
          <Segmented value={s.mode} onChange={(mode) => set({ mode, confirmDisk: '', confirmBox: false, confirmFree: false })}
            options={[['free', tr('installer.disk.modeFree')], ['erase', tr('installer.disk.modeErase')]]} />
        </Field>
      )}
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
      {s.disk && s.mode !== 'free' && (
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
    lang, region: region || ({ es: 'es_ES', en: 'en_US', pt: 'pt_BR', fr: 'fr_FR' }[lang] || 'en_US'),
    zone: s.zone || lord.time.zone() || 'UTC', kb,
  };
};

function Summary() {
  const t = useTheme();
  const { t: tr } = useLocale();
  const s = useStore(wiz);
  const p = plan(s);
  const arm64 = useSystemInfo(60000).arch === 'aarch64';     // UEFI only: no BIOS boot partition (install.c)
  const beside = s.mode === 'free';
  const rows = [
    ...(FIRST_ACCOUNT ? [] : [
      [tr('installer.summary.disk'), `${s.disk?.path} (${formatBytes(s.disk?.size || 0)}) — ${tr(beside ? 'installer.summary.kept' : 'installer.summary.erased')}`],
      [tr('installer.summary.layout'), beside ? tr('installer.summary.layoutFree', { size: formatBytes(s.disk?.freeBytes || 0) }) : tr(arm64 ? 'installer.summary.layoutValueEfi' : 'installer.summary.layoutValue')],
      [tr('installer.summary.swap'), s.swap ? `${s.swap / 1024} GB` : tr('installer.disk.swapNone')],
    ]),
    [tr('installer.summary.lang'), `${p.lang} · ${p.region}.UTF-8 · ${p.zone}`],
    [tr('installer.summary.keyboard'), kbName(p.kb)],
    [tr('installer.summary.user'), `${s.fullname} · ${s.user} · ${tr('installer.summary.admin')}`],
    [tr('installer.summary.host'), s.host],
  ];
  return (
    <View style={{ gap: 12 }}>
      <GroupBox>
        {rows.map(([k, v]) => (
          <FormRow key={k} label={k} labelWidth={140} minHeight={34}>
            <Text style={{ fontSize: typeScale.callout, color: t.text, lineHeight: 1.35 }}>{v}</Text>
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
    lang: p.lang, locale: `${p.region}.UTF-8`, timezone: p.zone, kblayout: p.kb.layout, kbvariant: p.kb.variant || '',
  } : {
    mode: s.mode, ...(s.mode === 'free' ? { freeStart: s.disk.freeStart, freeEnd: s.disk.freeEnd } : {}),
    disk: s.disk.name, user: s.user, fullname: s.fullname.trim(), password: s.pass, hostname: s.host,
    lang: p.lang, locale: `${p.region}.UTF-8`, timezone: p.zone, kblayout: p.kb.layout, kbvariant: p.kb.variant || '', swap: s.swap,
  };
  set({ pass: '', pass2: '' });            // the password doesn't stay in the UI
  admin.run(FIRST_ACCOUNT ? 'install-account' : 'install-run', JSON.stringify(job), (line) => {
    const m = /^progress (\w+) (\d+) (\d+)$/.exec(line);
    if (m) set({ progress: { phase: m[1], done: Number(m[2]), total: Number(m[3]) } });
    else wiz.set((st) => ({ ...st, log: [...st.log, line].slice(-30) }));
  }).then(() => set({ done: true }), (e) => set({ error: e.message }));
}

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
    disk: s.mode === 'free' ? !!s.disk?.freeOk && s.confirmFree : !!s.disk && s.confirmBox && s.confirmDisk === s.disk.name,
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
