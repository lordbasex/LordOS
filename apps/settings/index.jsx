// Settings — the window (a 220 px sidebar with a search field, the user card and colored section
// icons, and the open page under a title bar) plus the appearance, language and region,
// keyboard, themes and session pages. The other pages are in their own files; all of them are built
// from ./ui.jsx (docs/DESIGN.md, "Settings"); the wallpaper page is ./WallpaperGallery.jsx.
import { BatterySettings } from './Battery.jsx';
import {
  View, Text, useTheme, useSettings, shortcutList, shortcutsRev, themes, session, keyboard, admin, languages, setLanguage, useLocale,
  reloadCatalogs, tOptional, statusText, fs, useStore, SidebarItem, Toggle, Button, TextInput, ScrollView, SearchField, Hairline, useSystemInfo, useAppWindow, useVisitHistory, useTrackpads,
  spacing, typeScale, registerDockActions, hasTouchpad, errorText, wm, Segmented, osk,
  restoreMode, setRestoreMode, restoreBrowsers, setRestoreBrowsers, preferredLanguages, setPreferredLanguages, LANGUAGE_NAMES, Overlay, ToolbarButton, regionOf, localeOf, regionLabel, compareNames,
  useSidebar, SidebarToggle, SidebarOverlay, battery, deviceList,
} from '@lordos/sdk';
import { useEffect, useRef, useState } from 'react';
import { SoundSettings } from './Sound.jsx';
import { SharingSettings } from './Sharing.jsx';
import { CloudSettings, CloudHeaderAction, CloudSheets } from './Cloud.jsx';
import { UsersSettings } from './Users.jsx';
import { NetworkSettings } from './Network.jsx';
import { General, Updates } from './General.jsx';
import { ClipboardSettings } from './Clipboard.jsx';
import { PrivacySettings } from './Privacy.jsx';
import { PrintersSettings } from './Printers.jsx';
import { TrackpadSettings } from './Trackpad.jsx';
import { WindowPrefs } from './WindowPrefs.jsx';
import { DockSettings } from './Dock.jsx';
import { ThemeOptions } from './ThemeExtras.jsx';
import { ThemeGallery } from './ThemeGallery.jsx';   // Desktop style: the gallery of themes (docs/THEMES-PLAN.md, stage 1)
import { WallpaperGallery } from './WallpaperGallery.jsx';
import { subPage, placeOf, splitPlace } from './subpage.js';   // Wallpaper: the gallery (0.8.9.6.2)
import { SwitcherSettings } from './Switcher.jsx';
import { GlobalShortcutsSettings } from './GlobalShortcuts.jsx';
import { BrowserFrameSettings } from './BrowserFrame.jsx';
// hardware pages (docs/SETTINGS.md)
import { DisplaysSettings } from './Displays.jsx';
import { BluetoothSettings } from './Bluetooth.jsx';
import { PowerSettings } from './Power.jsx';
import { CoolingSettings } from './Cooling.jsx';
import { AISettings } from './AI.jsx';
import { SpellingSettings } from './Spelling.jsx';
import { NotificationsSettings } from './Notifications.jsx';
import { DateTimeSettings } from './DateTime.jsx';
import { DefaultAppsSettings } from './DefaultApps.jsx';
import { WebAppsSettings } from './WebApps.jsx';
import { FlatpakSettings } from './Flatpak.jsx';
import { FirmwareSettings } from './Firmware.jsx';
import { GpusSettings, anyGpus } from './Gpus.jsx';
import { Stack, Section, SettingRow, ValueRow, Badge, Note, Message, Actions, PopUp, shade } from './ui.jsx';

export function Icon({ size: s }) {
  const dark = '#3b3e46';
  return (
    <View style={{ width: s, height: s, borderRadius: s * 0.225, background: 'linear-gradient(180deg, #c4c8d0, #6c717c)',
      boxShadow: `0 ${s * 0.04}px ${s * 0.12}px rgba(0,0,0,0.35)`, overflow: 'hidden' }}>
      <View style={{ position: 'absolute', left: s * 0.44, top: s * 0.14, width: s * 0.12, height: s * 0.72, borderRadius: s * 0.04, backgroundColor: dark }} />
      <View style={{ position: 'absolute', left: s * 0.14, top: s * 0.44, width: s * 0.72, height: s * 0.12, borderRadius: s * 0.04, backgroundColor: dark }} />
      <View style={{ position: 'absolute', left: s * 0.21, top: s * 0.21, width: s * 0.58, height: s * 0.58, borderRadius: s * 0.29, backgroundColor: dark }} />
      <View style={{ position: 'absolute', left: s * 0.36, top: s * 0.36, width: s * 0.28, height: s * 0.28, borderRadius: s * 0.14,
        background: 'linear-gradient(180deg, #e5e7eb, #9ca3af)' }} />
    </View>
  );
}

// List with search (time zones, regions, keyboard layouts), shown as a row inside a section.
function SearchList({ items, value, onPick, placeholder, height = 180 }) {
  const t = useTheme();
  const [q, setQ] = useState('');
  const needle = q.trim().toLowerCase();
  const shown = (needle ? items.filter(([id, label]) => `${id} ${label}`.toLowerCase().includes(needle)) : items).slice(0, 300);
  return (
    <View style={{ gap: spacing.s, paddingVertical: spacing.s }}>
      <SearchField value={q} onChange={setQ} placeholder={placeholder} autoFocus />
      <View style={{ height, borderRadius: 8, border: `1px solid ${t.separator}`, backgroundColor: t.mode === 'dark' ? 'rgba(0,0,0,0.12)' : '#ffffff' }}>
        <ScrollView style={{ padding: 4 }}>
          {shown.map(([id, label, extra]) => (
            <View key={id} onClick={() => onPick(id)} style={{ height: 26, flexDirection: 'row', alignItems: 'center', gap: spacing.s, paddingHorizontal: spacing.s,
              borderRadius: 5, backgroundColor: id === value ? t.accent : 'transparent' }}>
              <Text style={{ flex: 1, minWidth: 0, fontSize: typeScale.callout, color: id === value ? '#fff' : t.text, whiteSpace: 'nowrap', overflow: 'hidden', textAlign: 'start' }}>{label}</Text>
              {extra ? <Text style={{ fontSize: typeScale.caption, color: id === value ? '#fff' : t.textDim }}>{extra}</Text> : null}
            </View>
          ))}
        </ScrollView>
      </View>
    </View>
  );
}

// ---- appearance ----

// A small picture of a window in light or dark mode (the Appearance tiles).
function ModeThumb({ mode, accent }) {
  const dark = mode === 'dark';
  const line = dark ? '#4a4a50' : '#d8d8de';
  return (
    <View style={{ width: 84, height: 56, borderRadius: 7, overflow: 'hidden', direction: 'ltr',
      background: dark ? 'linear-gradient(160deg, #2b2d4a, #121218)' : 'linear-gradient(160deg, #cfe0ff, #eef1f7)' }}>
      <View style={{ position: 'absolute', left: 10, top: 10, right: 10, bottom: -6, borderRadius: 5,
        backgroundColor: dark ? '#2c2c30' : '#ffffff', boxShadow: '0 1px 4px rgba(0,0,0,0.3)' }}>
        <View style={{ flexDirection: 'row', gap: 3, padding: 4 }}>
          {['#ff5f57', '#febc2e', '#28c840'].map((c) => <View key={c} style={{ width: 4, height: 4, borderRadius: 2, backgroundColor: c }} />)}
        </View>
        <View style={{ marginLeft: 6, height: 4, borderRadius: 2, backgroundColor: accent, width: 26 }} />
        <View style={{ marginLeft: 6, marginTop: 4, height: 3, borderRadius: 2, backgroundColor: line, width: 40 }} />
        <View style={{ marginLeft: 6, marginTop: 3, height: 3, borderRadius: 2, backgroundColor: line, width: 32 }} />
      </View>
    </View>
  );
}

function Appearance() {
  const t = useTheme();
  const { t: tr } = useLocale();
  const [s, set] = useSettings();
  return (
    <Stack>
      <ThemeGallery />
      <ThemeOptions theme={themes.info()} />
      <Section title={tr('settings.mode')}>
        <View style={{ flexDirection: 'row', gap: spacing.xxl, paddingVertical: spacing.m, justifyContent: 'center' }}>
          {[['dark', tr('settings.dark')], ['light', tr('settings.light')]].map(([id, label]) => (
            <View key={id} onClick={() => set({ mode: id })} style={{ alignItems: 'center', gap: 6 }}>
              <View style={{ padding: 2, borderRadius: 10, border: `2px solid ${s.mode === id ? t.accent : 'transparent'}` }}>
                <ModeThumb mode={id} accent={t.accent} />
              </View>
              <Text style={{ fontSize: typeScale.callout, fontWeight: s.mode === id ? 600 : 400, color: t.text }}>{label}</Text>
            </View>
          ))}
        </View>
      </Section>
      <Section>
        <SettingRow label={tr('shell.cc.accent')}>
          <View style={{ flexDirection: 'row', gap: spacing.s, direction: 'ltr' }}>
            {Object.entries(t.accents || {}).map(([name, c]) => (
              <View key={name} onClick={() => set({ accent: name })} style={{ width: 20, height: 20, borderRadius: 10, alignItems: 'center', justifyContent: 'center',
                background: `linear-gradient(180deg, ${shade(c, 1.15)}, ${c})`, boxShadow: '0 1px 2px rgba(0,0,0,0.25)' }}>
                {(s.accent || t.accentName) === name ? <View style={{ width: 8, height: 8, borderRadius: 4, backgroundColor: '#fff' }} /> : null}
              </View>
            ))}
          </View>
        </SettingRow>
        <SettingRow label={tr('settings.effects')} hint={tr('settings.effectsHint')}>
          <Toggle small on={s.effects} onChange={(effects) => set({ effects })} />
        </SettingRow>
        <SettingRow label={tr('settings.reduceMotion')} hint={tr('settings.reduceMotionHint')}>
          <Toggle small on={!!s.reduceMotion} onChange={(reduceMotion) => set({ reduceMotion })} />
        </SettingRow>
      </Section>
      <Section footer={tr('settings.scrollBarsHint')}>
        <SettingRow label={tr('settings.scrollBars')}>
          <PopUp width={170} value={s.scrollBars || 'auto'} onChange={(scrollBars) => set({ scrollBars })}
            options={[['auto', tr('settings.scrollBarsAuto')], ['scrolling', tr('settings.scrollBarsScrolling')], ['always', tr('settings.scrollBarsAlways')]]} />
        </SettingRow>
      </Section>
      <WindowPrefs />
      <BrowserFrameSettings />
    </Stack>
  );
}

// ---- language and region ----

// Progress of the administration helper (generate region, install packages…).
function AdminLog() {
  const t = useTheme();
  const { t: tr } = useLocale();
  const busy = useStore(admin.busy);
  if (!busy) return null;
  return (
    <Section>
      <View style={{ paddingVertical: spacing.m, gap: 3 }}>
        <Text style={{ fontSize: typeScale.callout, fontWeight: 600, color: t.text, textAlign: 'start' }}>{tr('settings.admin.running', { action: tr(`settings.admin.${busy.action}`), arg: busy.arg })}</Text>
        {busy.log.slice(-6).map((l, i) => <Text key={i} style={{ fontSize: typeScale.caption, color: t.textDim, whiteSpace: 'nowrap', overflow: 'hidden', direction: 'ltr', textAlign: 'start' }}>{statusText(l)}</Text>)}
      </View>
    </Section>
  );
}

const shortLocale = regionOf;    // "en_GB.UTF-8" → "en_GB", "en_IN" stays (sdk/localename.js)

function LanguageRegion() {
  const t = useTheme();
  const { t: tr, lang, region, formatDate, formatTime, formatNumber } = useLocale();
  const [s, set] = useSettings();
  const [msg, setMsg] = useState(null);
  const [showZones, setShowZones] = useState(false);
  const [showRegions, setShowRegions] = useState(false);
  const langs = languages();
  const [locales] = useState(() => lord.locale.available());
  const [zone, setZone] = useState(() => lord.time.zone());
  const [sys, setSys] = useState(() => lord.locale.get().system.LANG || 'C.UTF-8');
  const isAdmin = admin.available();
  const report = (p, ok) => p.then((m) => { setMsg({ ok: true, text: ok || m }); return true; }, (e) => { setMsg({ ok: false, text: e.message }); return false; });

  // Region for formats and for programs (LANG): generated with glibc if needed.
  const regionName = region || langs.find((l) => l.id === lang)?.region || '';
  const generated = (r) => locales.find((l) => regionOf(l.name) === r)?.generated;
  async function useRegion(r) {
    const name = localeOf(r);
    if (!generated(r)) {
      const ok = await report(admin.run('locale-gen', name), tr('settings.region.generated', { name: r }));
      if (!ok) return;
      const l = locales.find((x) => regionOf(x.name) === r);
      if (l) l.generated = true;
    }
    lord.locale.set({ LANG: name });
    lord.keyboard?.reloadCompose?.();
    set({ region: r });
  }
  // the first preferred language changed: its region too, if none was chosen
  const primaryChanged = (id) => {
    const r = langs.find((l) => l.id === id)?.region;
    if (r && !s.region) useRegion(r);
  };

  const now = new Date();
  // the language and the country by the code ("Basque · France (eu_FR)", #239), so a search by either finds it (#53),
  // sorted by that label: a language's regions stay together
  const regionItems = [...new Map(locales.map((l) => [shortLocale(l.name), l])).values()]
    .map((l) => { const r = shortLocale(l.name); return [r, regionLabel(r, tOptional), l.generated ? '✓' : '']; })
    .sort((a, b) => compareNames(a[1], b[1]));
  const dim = { fontSize: typeScale.body, color: t.textDim, textAlign: 'end' };
  const langName = langs.find((l) => l.id === lang)?.name || lang;
  return (
    <Stack>
      <PreferredLanguages langs={langs} onPrimary={primaryChanged} />
      <Section title={tr('settings.region.title')} footer={tr('settings.region.hint')}>
        <SettingRow label={regionName ? regionLabel(regionName, tOptional) : '—'}>
          <Button small label={tr('settings.change')} onClick={() => setShowRegions(!showRegions)} />
        </SettingRow>
        {showRegions && <SearchList items={regionItems} value={regionName} placeholder={tr('settings.region.search')}
          onPick={(r) => { setShowRegions(false); useRegion(r); }} />}
        <SettingRow label={tr('settings.region.preview')}>
          <View style={{ alignItems: 'flex-end', gap: 2, paddingVertical: 4 }}>
            <Text style={dim}>{`${formatDate(now, 'long')} · ${formatTime(now)}`}</Text>
            <Text style={dim}>{`${formatDate(now, 'numeric')} · ${formatNumber(1234567.89, 2)}`}</Text>
          </View>
        </SettingRow>
      </Section>
      <Section title={tr('settings.tz.title')}>
        <SettingRow label={zone.split('/').pop().replace(/_/g, ' ')} hint={zone.replace(/_/g, ' ')}>
          <Button small label={tr('settings.change')} onClick={() => setShowZones(!showZones)} />
        </SettingRow>
        {showZones && <SearchList items={lord.time.zones().sort().map((z) => [z, z.replace(/_/g, ' ')])} value={zone} placeholder={tr('settings.tz.search')}
          onPick={(z) => { setShowZones(false); report(admin.run('timezone', z)).then((ok) => { if (ok) { lord.time.reload(); setZone(z); } }); }} />}
      </Section>
      <Section title={tr('settings.system.title')} footer={tr('settings.system.hint', { name: sys })}>
        <ValueRow label="LANG" value={sys} mono>
          <Button small label={tr('settings.system.apply', { name: regionName || '—' })} onClick={() => regionName && report(admin.run('system-locale', localeOf(regionName))).then((ok) => ok && setSys(localeOf(regionName)))} />
        </ValueRow>
      </Section>
      <Section title={tr('settings.pack.title')} footer={tr('settings.pack.hint')}>
        <SettingRow label={langName}>
          <Button small label={tr('settings.pack.install', { lang: langName })}
            onClick={() => report(admin.run('langpack', regionName.startsWith(`${lang.split('-')[0]}_`) ? regionName : lang.split('-')[0]))} />
        </SettingRow>
        <PackImport onDone={(text) => { reloadCatalogs(); setMsg({ ok: true, text }); }} />
      </Section>
      {!isAdmin && <Note tone="warn">{tr('settings.admin.unavailable')}</Note>}
      <AdminLog />
      <Message msg={msg} />
    </Stack>
  );
}

// Preferred languages (docs/LANGUAGES.md): the first one is LordOS's language (its apps too; the
// login screen keeps its own); when a text isn't translated the next ones are used, in order, then English. A row is
// selected by clicking it; + adds a language (it becomes the first), − removes the selected one,
// ↑ ↓ move it. Without a saved list: the current language, then English, Spanish and Portuguese.
function PreferredLanguages({ langs, onPrimary }) {
  const t = useTheme();
  const { t: tr, lang } = useLocale();
  useSettings();
  const list = preferredLanguages().filter((id) => langs.some((l) => l.id === id));
  const [sel, setSel] = useState(lang);
  const [adding, setAdding] = useState(false);
  const plus = useRef(null);
  const save = (next) => {
    const first = list[0];
    setPreferredLanguages(next);
    if (next[0] !== first) onPrimary(next[0]);
  };
  const at = list.indexOf(sel);
  const move = (d) => {
    const j = at + d;
    if (at < 0 || j < 0 || j >= list.length) return;
    const next = [...list];
    [next[at], next[j]] = [next[j], next[at]];
    save(next);
  };
  const remove = () => {
    if (at < 0 || list.length < 2) return;
    const next = list.filter((x) => x !== sel);
    save(next);
    setSel(next[Math.max(0, at - 1)]);
  };
  const add = (id) => { setAdding(false); setSel(id); save([id, ...list.filter((x) => x !== id)]); };
  return (
    <Section title={tr('settings.lang.preferred')} footer={tr('settings.lang.preferredHint')}>
      {list.map((id, i) => {
        const l = langs.find((x) => x.id === id);
        const on = id === sel;
        return (
          <View key={id} onClick={() => setSel(id)} style={{ minHeight: 40, flexDirection: 'row', alignItems: 'center', gap: spacing.m,
            marginHorizontal: -6, paddingHorizontal: 6, borderRadius: 6, backgroundColor: on ? (t.mode === 'dark' ? 'rgba(10,132,255,0.22)' : 'rgba(10,132,255,0.12)') : 'transparent' }}>
            <Text style={{ width: 130, fontSize: typeScale.body, fontWeight: i === 0 ? 600 : 400, color: t.text, textAlign: 'start' }}>{l.name}</Text>
            <Text style={{ flex: 1, minWidth: 0, fontSize: typeScale.caption, color: t.textDim, whiteSpace: 'nowrap', overflow: 'hidden', textAlign: 'start' }}>
              {`${l.english}${l.user ? ` · ${tr('settings.lang.pack')}` : ''}${l.partial ? ` · ${tr('settings.lang.partial')}` : ''}`}</Text>
            {i === 0 ? <Text style={{ fontSize: typeScale.caption, fontWeight: 600, color: t.accent }}>{tr('settings.lang.primary')}</Text> : null}
          </View>
        );
      })}
      <View style={{ flexDirection: 'row', alignItems: 'center', gap: 2, paddingVertical: 4 }}>
        <View ref={plus}><ToolbarButton small icon="+" title={tr('settings.lang.add')} onClick={() => setAdding(!adding)} /></View>
        <ToolbarButton small icon="−" title={tr('settings.lang.remove')} disabled={list.length < 2 || at < 0} onClick={remove} />
        <View style={{ flex: 1 }} />
        <ToolbarButton small icon="↑" title={tr('settings.lang.up')} disabled={at <= 0} onClick={() => move(-1)} />
        <ToolbarButton small icon="↓" title={tr('settings.lang.down')} disabled={at < 0 || at >= list.length - 1} onClick={() => move(1)} />
      </View>
      {adding ? (
        <Overlay anchor={plus} onClose={() => setAdding(false)}>
          <AddLanguage langs={langs} have={list} onAdd={add} />
        </Overlay>
      ) : null}
    </Section>
  );
}

// The + popover: a search field, the languages ready to use (bundled or installed packs not in the
// list yet) and the ones LordOS isn't translated into, which can't be added: picking one says so and
// how to ask for it.
function AddLanguage({ langs, have, onAdd }) {
  const t = useTheme();
  const { t: tr } = useLocale();
  const [q, setQ] = useState('');
  const [missing, setMissing] = useState(null);
  const fold = (x) => String(x).toLowerCase().normalize('NFD').replace(/[\u0300-\u036f]/g, '');
  const needle = fold(q.trim());
  const match = (...xs) => !needle || xs.some((x) => fold(x).includes(needle));
  const ready = langs.filter((l) => !have.includes(l.id) && match(l.name, l.english, l.id));
  // zh is listed while neither zh-Hans nor zh-Hant is installed
  const later = LANGUAGE_NAMES.filter(([id, name, en]) => !langs.some((l) => l.id === id || l.id.startsWith(`${id}-`)) && match(name, en, id));
  const head = (text) => <Text style={{ paddingHorizontal: 8, paddingTop: 8, paddingBottom: 3, fontSize: 11, fontWeight: 600, color: t.textDim, textAlign: 'start' }}>{text}</Text>;
  const row = (key, name, en, right, onClick) => (
    <View key={key} onClick={onClick} style={{ height: 28, flexDirection: 'row', alignItems: 'center', gap: spacing.s, paddingHorizontal: 8, borderRadius: 5 }}>
      <Text style={{ fontSize: typeScale.callout, fontWeight: 600, color: t.text, textAlign: 'start' }}>{name}</Text>
      <Text style={{ flex: 1, minWidth: 0, fontSize: typeScale.callout, color: t.textDim, whiteSpace: 'nowrap', overflow: 'hidden', textAlign: 'start' }}>{`— ${en}`}</Text>
      {right ? <Text style={{ fontSize: typeScale.caption, color: t.textDim }}>{right}</Text> : null}
    </View>
  );
  return (
    <View style={{ position: 'absolute', left: 0, top: 30, width: 380, padding: 10, gap: 8, borderRadius: 10, backgroundColor: t.mode === 'dark' ? '#2a2b30' : '#ffffff',
      border: `1px solid ${t.separator}`, boxShadow: '0 16px 40px rgba(0,0,0,0.35)' }}>
      <Text style={{ fontSize: typeScale.body, fontWeight: 600, color: t.text, textAlign: 'start' }}>{tr('settings.lang.add')}</Text>
      <SearchField value={q} onChange={(v) => { setQ(v); setMissing(null); }} placeholder={tr('settings.lang.search')} autoFocus />
      <View style={{ height: 260, borderRadius: 8, border: `1px solid ${t.separator}`, backgroundColor: t.mode === 'dark' ? 'rgba(0,0,0,0.15)' : '#fafafb' }}>
        <ScrollView style={{ padding: 4 }}>
          {ready.length ? head(tr('settings.lang.ready')) : null}
          {ready.map((l) => row(l.id, l.name, l.english, l.user ? tr('settings.lang.pack') : l.partial ? tr('settings.lang.partial') : '', () => onAdd(l.id)))}
          {later.length ? head(tr('settings.lang.notYet')) : null}
          {later.map(([id, name, en]) => row(id, name, en, '', () => setMissing(name)))}
          {!ready.length && !later.length ? <Text style={{ padding: 10, fontSize: typeScale.callout, color: t.textDim, textAlign: 'start' }}>{tr('settings.lang.none')}</Text> : null}
        </ScrollView>
      </View>
      {missing ? <Note>{tr('settings.lang.notYetHint', { name: missing })}</Note> : null}
    </View>
  );
}

// LordOS language pack (built by tools/i18n-pack.mjs): lordos-lang-<id>.json in Downloads. The folders are listed off
// the main thread, once per visit (they were listed on every render of the page, with every file in them).
const PACK_DIRS = ['Descargas', 'Downloads', '.'];
function PackImport({ onDone }) {
  const { t: tr } = useLocale();
  const [found, setFound] = useState([]);
  useEffect(() => {
    let live = true;
    const home = fs.home();
    Promise.all(PACK_DIRS.map((d) => fs.listAsync(d === '.' ? home : `${home}/${d}`).then((list) => list
      .filter((e) => /^lordos-lang-[\w-]+\.json$/.test(e.name)).map((e) => (d === '.' ? e.name : `${d}/${e.name}`)), () => [])))
      .then((lists) => { if (live) setFound(lists.flat()); });
    return () => { live = false; };
  }, []);
  if (!found.length) return null;
  const install = (p) => {
    try {
      const pack = JSON.parse(fs.read(p));
      const id = String(pack.meta?.id || '');
      const files = { meta: JSON.stringify(pack.meta, null, 2) };
      for (const [ns, obj] of Object.entries(pack.catalogs || {})) files[ns] = JSON.stringify(obj, null, 2);
      if (!lord.locale.installPack(id, files)) throw new Error(tr('settings.pack.invalid'));
      onDone(tr('settings.pack.imported', { name: pack.meta.name || id }));
    } catch (e) { onDone(`${p}: ${e.message}`); }
  };
  return (
    <SettingRow label={tr('settings.pack.lordos')} hint={found.join(' · ')}>
      {found.map((p) => <Button small key={p} label={tr('settings.pack.add', { file: p.split('/').pop() })} onClick={() => install(p)} />)}
    </SettingRow>
  );
}

// ---- keyboard ----

// The short label of a layout or input method (US, LA, 한…) in a small key cap.
function KeyCap({ label, active }) {
  const t = useTheme();
  return (
    <View style={{ minWidth: 30, height: 22, paddingHorizontal: 5, borderRadius: 5, alignItems: 'center', justifyContent: 'center',
      backgroundColor: active ? t.accent : t.control }}>
      <Text style={{ fontSize: typeScale.caption, fontWeight: 600, color: active ? '#fff' : t.text }}>{label}</Text>
    </View>
  );
}

// The shortcuts in use with this theme (sdk/shortcuts.js): read only.
function ShortcutsList() {
  const { t: tr } = useLocale();
  const t = useTheme();
  useStore(shortcutsRev);
  return shortcutList().map(({ group, items }) => (
    <Section key={group} title={tr(`settings.kb.shortcuts.${group}`)} footer={group === 'apps' ? tr('settings.kb.shortcuts.note') : undefined}>
      {items.map((it) => (
        <SettingRow key={it.id} label={it.label}>
          <Text style={{ fontSize: typeScale.body, color: t.textDim }}>{it.keys}</Text>
        </SettingRow>
      ))}
    </Section>
  ));
}

function Keyboard() {
  const { t: tr } = useLocale();
  const [s, set] = useSettings();
  const [sample, setSample] = useState('');
  const [adding, setAdding] = useState(false);
  const all = keyboard.list();
  const mine = keyboard.configured();
  const nameOf = (k) => {
    if (k.ime) return tr(`settings.ime.${k.ime}`);
    const l = all.find((x) => x.id === k.layout);
    const v = l?.variants.find((x) => x.id === k.variant);
    return v ? v.name : l ? l.name : k.layout;
  };
  const items = all.flatMap((l) => [[l.id, l.name, l.id], ...l.variants.map((v) => [`${l.id}(${v.id})`, v.name, `${l.id}(${v.id})`])]);
  const add = (id) => {
    const m = /^([\w-]+)(?:\(([\w-]+)\))?$/.exec(id);
    if (!m) return;
    const k = { layout: m[1], variant: m[2] || '' };
    if (!mine.some((x) => x.layout === k.layout && x.variant === k.variant && !x.ime)) keyboard.set([...mine, k].slice(0, 6));
    setAdding(false);
  };
  // Input method: it goes on top of the Latin keyboard (us) and is chosen like any layout.
  const addIme = (id) => { if (!mine.some((x) => x.ime === id)) keyboard.set([...mine, { layout: 'us', variant: '', ime: id }].slice(0, 6)); };
  const active = s.keyboardIndex || 0;
  return (
    <Stack>
      <Section title={tr('settings.kb.layouts')} footer={tr('settings.kb.hint')}>
        {mine.map((k, i) => (
          <SettingRow key={`${k.layout}${k.variant}${k.ime || ''}`} icon={<KeyCap label={keyboard.label(k)} active={i === active} />} label={nameOf(k)}>
            {i !== active && <Button small label={tr('settings.kb.use')} onClick={() => set({ keyboardIndex: i })} />}
            {mine.length > 1 && <Button small label="−" onClick={() => keyboard.set(mine.filter((_, j) => j !== i))} />}
          </SettingRow>
        ))}
        {adding && all.length > 0 ? <SearchList items={items} placeholder={tr('settings.kb.search')} onPick={add} height={220} /> : null}
        <Actions style={{ paddingVertical: spacing.s }}>
          <Button small label={tr('settings.kb.add')} onClick={() => setAdding(!adding)} />
        </Actions>
      </Section>
      {!all.length && <Note tone="warn">{tr('settings.kb.noxkb')}</Note>}
      <Section title={tr('settings.ime.title')} footer={tr('settings.ime.hint')}>
        {keyboard.engines.map((e) => (
          <SettingRow key={e.id} icon={<KeyCap label={e.label} />} label={tr(`settings.ime.${e.id}`)}>
            {!mine.some((x) => x.ime === e.id) && <Button small label={tr('settings.ime.add')} onClick={() => addIme(e.id)} />}
          </SettingRow>
        ))}
      </Section>
      {keyboard.active()?.ime && keyboard.missingDictionary() && <Note tone="warn">{tr('settings.ime.noDict')}</Note>}
      <Section title={tr('settings.osk.title')} footer={tr('settings.osk.hint')}>
        <Actions style={{ paddingVertical: spacing.s }}>
          <Button small label={tr('settings.osk.show')} onClick={() => osk.show()} />
        </Actions>
      </Section>
      <Section title={tr('settings.kb.test')} footer={tr('settings.kb.testHint')}>
        <View style={{ paddingVertical: spacing.s }}>
          <TextInput value={sample} onChange={setSample} placeholder={tr('settings.kb.placeholder')} onCancel={() => setSample('')} />
        </View>
      </Section>
      <SpellingSettings />
      <SwitcherSettings tr={tr} />
      <ShortcutsList />
      <GlobalShortcutsSettings tr={tr} />
    </Stack>
  );
}

function Session() {
  const { t: tr } = useLocale();
  useSettings();
  const mode = restoreMode();
  return (
    <Stack>
      <Section footer={tr('settings.session.restoreHint')}>
        <SettingRow label={tr('settings.session.restore')}>
          <PopUp value={mode} onChange={setRestoreMode} options={['always', 'ask', 'off'].map((m) => [m, tr(`settings.session.restore.${m}`)])} />
        </SettingRow>
        <SettingRow label={tr('settings.session.browsers')} hint={tr('settings.session.browsersHint')} disabled={mode === 'off'}>
          <Toggle small on={restoreBrowsers()} onChange={(v) => { if (mode !== 'off') setRestoreBrowsers(v); }} />
        </SettingRow>
      </Section>
      <Section footer={tr(session.managed() ? 'settings.session.hintManaged' : 'settings.session.hint')}>
        <SettingRow label={tr('shell.menu.logout')}>
          <Button small label={tr('shell.menu.logout')} onClick={() => session.quit()} />
        </SettingRow>
      </Section>
    </Stack>
  );
}

// ---- window ----

// A padlock drawn with views (🔒 is a color emoji on the system fonts).
function LockGlyph() {
  return (
    <View style={{ width: 12, height: 13, alignItems: 'center' }}>
      <View style={{ width: 8, height: 9, borderRadius: 4, border: '2px solid #fff' }} />
      <View style={{ position: 'absolute', top: 5, width: 12, height: 8, borderRadius: 2, backgroundColor: '#fff' }} />
    </View>
  );
}

// A lightning bolt: two offset bars joined by a short one (⚡ is a color emoji).
function BoltGlyph() {
  return (
    <View style={{ width: 12, height: 14, direction: 'ltr' }}>
      <View style={{ position: 'absolute', left: 5, top: 0, width: 4, height: 7, borderRadius: 1, backgroundColor: '#fff' }} />
      <View style={{ position: 'absolute', left: 2, top: 5, width: 8, height: 3, borderRadius: 1, backgroundColor: '#fff' }} />
      <View style={{ position: 'absolute', left: 3, top: 7, width: 4, height: 7, borderRadius: 1, backgroundColor: '#fff' }} />
    </View>
  );
}

// A fan: three rounded blades around a hub.
function FanGlyph() {
  return (
    <View style={{ width: 14, height: 14, direction: 'ltr' }}>
      {[0, 120, 240].map((r) => (
        <View key={r} style={{ position: 'absolute', left: 0, top: 0, width: 14, height: 14, transform: `rotate(${r}deg)` }}>
          <View style={{ position: 'absolute', left: 6, top: 0, width: 5, height: 7, borderTopRightRadius: 5, borderTopLeftRadius: 2, borderBottomRightRadius: 2, backgroundColor: '#fff' }} />
        </View>
      ))}
      <View style={{ position: 'absolute', left: 5, top: 5, width: 4, height: 4, borderRadius: 2, backgroundColor: '#fff' }} />
    </View>
  );
}

// A chip: a square body with three pins on each side.
function ChipGlyph() {
  return (
    <View style={{ width: 14, height: 14, direction: 'ltr' }}>
      <View style={{ position: 'absolute', left: 3, top: 3, width: 8, height: 8, borderRadius: 1.5, backgroundColor: '#fff' }} />
      {[4, 6.5, 9].map((y) => (
        <View key={y}>
          <View style={{ position: 'absolute', left: 0, top: y - 0.5, width: 3, height: 1.5, backgroundColor: '#fff' }} />
          <View style={{ position: 'absolute', left: 11, top: y - 0.5, width: 3, height: 1.5, backgroundColor: '#fff' }} />
        </View>
      ))}
    </View>
  );
}

// A graphics card: a board with a fan and its connector's pins below.
function GpuGlyph() {
  return (
    <View style={{ width: 14, height: 14, direction: 'ltr' }}>
      <View style={{ position: 'absolute', left: 0, top: 3, width: 14, height: 7, borderRadius: 1.5, backgroundColor: '#fff' }} />
      <View style={{ position: 'absolute', left: 6.5, top: 4.5, width: 4, height: 4, borderRadius: 2, backgroundColor: '#5e5ce6' }} />
      <View style={{ position: 'absolute', left: 2, top: 10, width: 7, height: 2, backgroundColor: '#fff' }} />
    </View>
  );
}

// Sidebar groups (as in TangoOS): [id, component, icon color, symbol (a glyph or a component)].
const GROUPS = [
  [['general', General, '#8e8e93', 'i'], ['appearance', Appearance, '#3a3a3c', '◐'], ['wallpaper', WallpaperGallery, '#30b0c7', '▣'], ['dock', DockSettings, '#3a3a3c', '▁'], ['displays', DisplaysSettings, '#0a84ff', '▭']],
  [['network', NetworkSettings, '#0a84ff', '⇅'], ['bluetooth', BluetoothSettings, '#0a84ff', 'ᛒ'], ['sound', SoundSettings, '#ff375f', '♪'], ['notifications', NotificationsSettings, '#ff3b30', '◉'], ['printers', PrintersSettings, '#636366', '⎙'], ['clipboard', ClipboardSettings, '#5e5ce6', '⎘'], ['sharing', SharingSettings, '#34c759', '⇄'], ['cloud', CloudSettings, '#0a84ff', '☁']],
  [['language', LanguageRegion, '#30b0c7', 'A'], ['datetime', DateTimeSettings, '#0a84ff', '◷'], ['keyboard', Keyboard, '#8e8e93', '⌨'], ['trackpad', TrackpadSettings, '#8e8e93', '▭'], ['defaultapps', DefaultAppsSettings, '#8e8e93', '◫'], ['webapps', WebAppsSettings, '#5e5ce6', '◎'], ['flatpak', FlatpakSettings, '#4a86cf', '◇'], ['ai', AISettings, '#bf5af2', '✦']],
  [['battery', BatterySettings, '#30d158', '▮'], ['power', PowerSettings, '#30d158', BoltGlyph], ['cooling', CoolingSettings, '#32ade6', FanGlyph], ['firmware', FirmwareSettings, '#5e5ce6', ChipGlyph], ['gpus', GpusSettings, '#5e5ce6', GpuGlyph], ['privacy', PrivacySettings, '#0a84ff', LockGlyph], ['users', UsersSettings, '#0a84ff', '☺'], ['updates', Updates, '#8e8e93', '↻'], ['session', Session, '#ff453a', '↪']],
];
const SECTIONS = GROUPS.flat();
// Board firmware
// only on a BCM2712 board (Raspberry Pi 5, 500, CM5: lord.system.info().board)
const isPi5 = () => { try { return lord.system.info().board === 'pi5'; } catch (e) { return false; } };
// Battery: on a computer with one, or when something connected has a battery (docs/BATTERY.md)
const hasAnyBattery = () => { try { return !!battery.get()?.hasBattery || deviceList().length > 0; } catch (e) { return false; } };
// Graphics cards: with any GPU (lord.vm.gpus; docs/MACHINES-GPU.md): the compatibility report and NVIDIA's status are for every computer
const sectionShown = (id, hasPad, pi5 = isPi5(), gpus = id === 'gpus' && anyGpus()) => (id !== 'firmware' || pi5) && (id !== 'battery' || hasAnyBattery())
  && (id !== 'gpus' || gpus);

// The Dock's right-click menu lists every section, in the sidebar's order and with its names; one
// opens Settings on that page (sdk/dock.js, quick actions). Built from SECTIONS, so it never drifts.
registerDockActions('settings', () => SECTIONS.filter(([id]) => sectionShown(id, hasTouchpad()))
  .map(([id]) => ({ id, label: `settings.section.${id}`, args: { section: id } })));

function SectionIcon({ color, glyph: G, size = 20 }) {
  return typeof G === 'string'
    ? <Badge glyph={G} color={color} size={size} />
    : <Badge color={color} size={size}><G /></Badge>;
}

// Session user: initials, name and whether they are an administrator.
function UserCard({ onClick, selected }) {
  const t = useTheme();
  const { t: tr } = useLocale();
  const [hover, setHover] = useState(false);
  const info = useSystemInfo(60000);
  const name = info.fullName || info.user || '';
  const initials = name.split(/\s+/).filter(Boolean).slice(0, 2).map((w) => w[0].toUpperCase()).join('') || '?';
  return (
    <View onClick={onClick} onMouseEnter={() => setHover(true)} onMouseLeave={() => setHover(false)}
      style={{ flexDirection: 'row', alignItems: 'center', gap: 10, paddingHorizontal: 8, paddingVertical: 6, borderRadius: 8,
        backgroundColor: selected || hover ? t.hover : 'transparent' }}>
      <View style={{ width: 36, height: 36, borderRadius: 18, alignItems: 'center', justifyContent: 'center', background: 'linear-gradient(180deg, #5ac8fa, #0a6cd6)' }}>
        <Text style={{ fontSize: 14, fontWeight: 600, color: '#fff' }}>{initials}</Text>
      </View>
      <View style={{ flex: 1, minWidth: 0, gap: 1 }}>
        <Text style={{ fontSize: typeScale.body, fontWeight: 600, color: t.text, whiteSpace: 'nowrap', overflow: 'hidden', textAlign: 'start' }}>{name}</Text>
        <Text style={{ fontSize: typeScale.caption, color: t.textDim, whiteSpace: 'nowrap', overflow: 'hidden', textAlign: 'start' }}>
          {info.admin ? tr('settings.user.admin') : tr('settings.user.standard')}</Text>
      </View>
    </View>
  );
}

// The open section survives the interface being remounted when the language changes.
let lastSection = 'general';
// a small window (a Lord Workspaces tile): below SIDE_AT the sections hide behind a button in the title bar
const SIDE_AT = 700;

// Other apps open a section: wm.open('settings', { args: { section: 'network', page: 'vpn' } }).
export default function Settings() {
  const t = useTheme();
  const { t: tr } = useLocale();
  const { args } = useAppWindow();
  const [section, setSectionState] = useState(lastSection);
  const [query, setQuery] = useState('');
  const side = useSidebar(SIDE_AT);
  const setSection = (s) => { lastSection = s; setSectionState(s); subPage.set(null); if (side.narrow) side.close(); };
  // the mouse's back and forward buttons (#234) walk sections and a section's sub-pages (subpage.js: Wallpaper's collections)
  const sub = useStore(subPage);
  useVisitHistory(placeOf(section, sub), (p) => { const [s, sp] = splitPlace(p); lastSection = s; setSectionState(s); subPage.set(sp); });
  // 'themes' (the old Themes page, links from older help) now opens Appearance
  // (0.8.9.6.2) AI's pages are sub-pages: { section: 'ai', page: 'sentinela' } opens Sentinela's (set after setSection clears it)
  useEffect(() => {
    const sec = args?.section === 'themes' ? 'appearance' : args?.section;
    if (!sec || !SECTIONS.some(([id]) => id === sec)) return;
    setSection(sec);
    if (sec === 'ai' && typeof args?.page === 'string' && /^[a-z]+$/.test(args.page)) subPage.set(args.page);
  }, [args?.nonce]);
  const [, Body] = SECTIONS.find(([id]) => id === section) || SECTIONS[0];
  const q = query.trim().toLowerCase();
  // Mouse & Trackpad always shows (its touchpad rows need a touchpad, which lord-sessiond lends with the monitor);
  // hasPad is still passed to sectionShown, which only hides Board firmware off a Raspberry Pi 5
  const hasPad = useTrackpads().some((d) => d.kind === 'touchpad');
  const [pi5] = useState(isPi5);
  const [gpus] = useState(anyGpus);
  const visible = (id) => sectionShown(id, hasPad, pi5, gpus) && `${tr(`settings.section.${id}`)} ${tr(`settings.keywords.${id}`)}`.toLowerCase().includes(q);
  const groups = GROUPS.map((g) => g.filter(([id]) => visible(id))).filter((g) => g.length);
  const pad = side.narrow ? spacing.l : spacing.xxl;
  const sidebar = (
      <View style={{ width: 220, flex: side.narrow ? 1 : undefined, backgroundColor: t.sidebarBg, paddingTop: spacing.m }}>
        <Hairline edge="end" />
        <View style={{ paddingHorizontal: 10, gap: spacing.s }}>
          <SearchField value={query} onChange={setQuery} placeholder={tr('settings.search')}
            onSubmit={() => groups[0] && setSection(groups[0][0][0])} />
          <UserCard onClick={() => setSection('users')} selected={section === 'users'} />
        </View>
        <ScrollView style={{ flex: 1, marginTop: spacing.xs }}>
          <View style={{ paddingHorizontal: 10, paddingBottom: spacing.m }}>
            {groups.map((g, i) => (
              <View key={i} style={{ gap: 1, marginTop: i ? spacing.m : 0 }}>
                {g.map(([id, , color, glyph]) => (
                  <SidebarItem key={id} label={tr(`settings.section.${id}`)} selected={id === section} onClick={() => setSection(id)}
                    icon={<SectionIcon color={color} glyph={glyph} />} />
                ))}
              </View>
            ))}
            {!groups.length && <Text style={{ fontSize: typeScale.callout, color: t.textDim, margin: spacing.s, textAlign: 'start' }}>{tr('settings.noResults')}</Text>}
          </View>
        </ScrollView>
      </View>
  );
  return (
    <View style={{ flex: 1, flexDirection: 'row' }}>
      {side.narrow ? null : sidebar}
      <View style={{ flex: 1, minWidth: 0 }}>
        <View style={{ height: 48, flexDirection: 'row', alignItems: 'center', gap: spacing.s, paddingHorizontal: pad }}>
          <Hairline edge="bottom" />
          {side.narrow ? <SidebarToggle small shown={side.shown} onClick={side.toggle} /> : null}
          <Text style={{ flex: 1, minWidth: 0, fontSize: typeScale.title, fontWeight: 600, color: t.text, whiteSpace: 'nowrap', overflow: 'hidden', textAlign: 'start' }}>
            {tr(`settings.section.${section}`)}</Text>
          {section === 'cloud' ? <CloudHeaderAction /> : null}
        </View>
        <ScrollView resetKey={placeOf(section, sub)} style={{ flex: 1 }}>
          <View style={{ paddingHorizontal: pad, paddingTop: side.narrow ? spacing.l : spacing.xl, alignItems: 'center' }}>
            {/* Cloud's account page has two columns, Wallpaper and AI are galleries: they take a wider column on wide windows */}
            <View style={{ width: '100%', maxWidth: section === 'cloud' ? 980 : section === 'wallpaper' || section === 'ai' ? 1180 : 680 }}>
              <Body page={section === 'network' || section === 'cloud' || section === 'ai' ? args?.page : undefined}
                alert={section === 'ai' ? args?.alert : undefined} />
            </View>
          </View>
        </ScrollView>
      </View>
      {side.narrow && side.shown ? <SidebarOverlay width={220} onClose={side.close}>{sidebar}</SidebarOverlay> : null}
      {/* Cloud's sheets: inside this window, over its content, never in the shell's modal layer (docs/CLOUD.md 18.2) */}
      {section === 'cloud' ? <CloudSheets /> : null}
    </View>
  );
}
