// Settings — the window (a 220 px sidebar with a search field, the user card and colored section
// icons, and the open page under a title bar) plus the appearance, wallpaper, language and region,
// keyboard, themes and session pages. The other pages are in their own files; all of them are built
// from ./ui.jsx (docs/DESIGN.md, "Settings").
import {
  View, Text, Image, useTheme, useSettings, shortcutList, shortcutsRev, themes, session, keyboard, admin, languages, setLanguage, useLocale,
  reloadCatalogs, tOptional, statusText, fs, useStore, SidebarItem, Toggle, Button, TextInput, ScrollView, SearchField, Hairline, useSystemInfo, useAppWindow, useTrackpads,
  spacing, typeScale, registerDockActions, hasTouchpad, findWallpaper, wallpaperThumb, WALLPAPER_CATEGORIES, GREETER_WALLPAPER, setLoginStill, MOTION_SECTIONS, MOTION_RATES, motionBusy, motionItems, motionThumb, motionDownloads, useCodecs, downloadMotion, removeMotion, setLoginMotion, variantFor, userVideoProblem, media, FilePicker, screenDisplays, errorText, wm, Segmented, osk,
  restoreMode, setRestoreMode, restoreBrowsers, setRestoreBrowsers,
} from '@lordos/sdk';
import { useEffect, useState } from 'react';
import { SoundSettings } from './Sound.jsx';
import { SharingSettings } from './Sharing.jsx';
import { UsersSettings } from './Users.jsx';
import { NetworkSettings } from './Network.jsx';
import { General, Updates } from './General.jsx';
import { ClipboardSettings } from './Clipboard.jsx';
import { PrivacySettings } from './Privacy.jsx';
import { PrintersSettings } from './Printers.jsx';
import { TrackpadSettings } from './Trackpad.jsx';
import { DockSettings } from './Dock.jsx';
import { SwitcherSettings } from './Switcher.jsx';
import { BrowserFrameSettings } from './BrowserFrame.jsx';
// hardware pages (docs/SETTINGS.md)
import { DisplaysSettings } from './Displays.jsx';
import { BluetoothSettings } from './Bluetooth.jsx';
import { PowerSettings } from './Power.jsx';
import { CoolingSettings } from './Cooling.jsx';
import { NotificationsSettings } from './Notifications.jsx';
import { DateTimeSettings } from './DateTime.jsx';
import { DefaultAppsSettings } from './DefaultApps.jsx';
import { FirmwareSettings } from './Firmware.jsx';
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

// A small picture of a desktop style: a menu bar and a Dock ('default') or a centered taskbar
// ('classic'). Drawn with views: our own sketch, no screenshots.
function StyleThumb({ id, accent, dark }) {
  const glass = dark ? 'rgba(30,30,36,0.85)' : 'rgba(255,255,255,0.82)';
  const win = dark ? '#2c2c30' : '#ffffff';
  const classic = id === 'classic';
  return (
    <View style={{ width: 132, height: 82, borderRadius: 7, overflow: 'hidden', direction: 'ltr',
      background: dark ? 'linear-gradient(160deg, #2b2d4a, #121218)' : 'linear-gradient(160deg, #cfe0ff, #eef1f7)' }}>
      {classic ? null : <View style={{ position: 'absolute', left: 0, right: 0, top: 0, height: 6, backgroundColor: glass }} />}
      <View style={{ position: 'absolute', left: 22, top: classic ? 10 : 14, width: 86, height: 50, borderRadius: classic ? 3 : 5, backgroundColor: win,
        boxShadow: '0 1px 4px rgba(0,0,0,0.3)' }}>
        <View style={{ flexDirection: 'row', gap: 3, padding: 4, justifyContent: classic ? 'flex-end' : 'flex-start' }}>
          {classic ? [0, 1, 2].map((i) => <View key={i} style={{ width: 5, height: 3, borderRadius: 1, backgroundColor: dark ? '#6a6a72' : '#b8b8c0' }} />)
            : ['#ff5f57', '#febc2e', '#28c840'].map((c) => <View key={c} style={{ width: 4, height: 4, borderRadius: 2, backgroundColor: c }} />)}
        </View>
        <View style={{ marginLeft: 6, height: 4, borderRadius: 2, backgroundColor: accent, width: 30 }} />
      </View>
      {classic ? (
        <View style={{ position: 'absolute', left: 0, right: 0, bottom: 0, height: 11, backgroundColor: glass, flexDirection: 'row',
          alignItems: 'center', justifyContent: 'center', gap: 3 }}>
          {[accent, '#8e8e93', '#8e8e93', '#8e8e93', '#8e8e93'].map((c, i) => <View key={i} style={{ width: 6, height: 6, borderRadius: 1.5, backgroundColor: c }} />)}
        </View>
      ) : (
        <View style={{ position: 'absolute', left: 36, right: 36, bottom: 3, height: 9, borderRadius: 3, backgroundColor: glass, flexDirection: 'row',
          alignItems: 'center', justifyContent: 'center', gap: 3 }}>
          {[0, 1, 2, 3].map((i) => <View key={i} style={{ width: 5, height: 5, borderRadius: 1.5, backgroundColor: i ? '#8e8e93' : accent }} />)}
        </View>
      )}
    </View>
  );
}

// Desktop style: every installed theme (Lord Flow and Lord Classic first; the login screen's is hidden),
// switched live. The open app windows come back in the new one (sdk/index.js themes.use), Settings on
// this page. Since 0.8.5 this is the only place to choose a theme (there is no separate Themes page).
function StylePicker() {
  const t = useTheme();
  const { t: tr } = useLocale();
  const order = (id) => (id === 'default' ? 0 : id === 'classic' ? 1 : 2);
  const list = [...themes.list()].sort((a, b) => order(a.id) - order(b.id) || a.name.localeCompare(b.name));
  if (!list.length) return null;
  const pick = (id) => {
    wm.open('settings', { args: { section: 'appearance' } });
    themes.use(id);
  };
  return (
    <Section title={tr('settings.style.title')} footer={tr('settings.style.hint')}>
      <View style={{ flexDirection: 'row', gap: spacing.xxl, paddingVertical: spacing.m, justifyContent: 'center' }}>
        {list.map((th) => {
          const id = th.id;
          return (
            <View key={id} onClick={() => { if (!th.current) pick(id); }} style={{ alignItems: 'center', gap: 6 }}>
              <View style={{ padding: 2, borderRadius: 10, border: `2px solid ${th.current ? t.accent : 'transparent'}` }}>
                <StyleThumb id={id} accent={t.accent} dark={t.mode === 'dark'} />
              </View>
              <Text style={{ fontSize: typeScale.callout, fontWeight: th.current ? 600 : 400, color: t.text }}>{th.name}</Text>
            </View>
          );
        })}
      </View>
    </Section>
  );
}

function Appearance() {
  const t = useTheme();
  const { t: tr } = useLocale();
  const [s, set] = useSettings();
  return (
    <Stack>
      <StylePicker />
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
                {s.accent === name ? <View style={{ width: 8, height: 8, borderRadius: 4, backgroundColor: '#fff' }} /> : null}
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
      <BrowserFrameSettings />
    </Stack>
  );
}

// A theme names its wallpapers with a locale key (nameKey) and an English fallback (name).
const wallpaperName = (w) => (w.nameKey && tOptional(w.nameKey)) || w.name;

// While a chosen wallpaper is starting; it turns only then.
function TileSpinner() {
  const [a, setA] = useState(0);
  useEffect(() => { const id = setInterval(() => setA((x) => (x + 30) % 360), 80); return () => clearInterval(id); }, []);
  return (
    <View style={{ position: 'absolute', left: 0, top: 0, right: 0, bottom: 0, alignItems: 'center', justifyContent: 'center', backgroundColor: 'rgba(0,0,0,0.45)' }}>
      <Text style={{ fontSize: 22, color: '#fff', transform: `rotate(${a}deg)` }}>⟳</Text>
    </View>
  );
}

function MotionTile({ it, on, avail, percent, busy, onClick }) {
  const t = useTheme();
  const { t: tr } = useLocale();
  const name = tOptional(`settings.motion.item.${it.id}`) || it.name;
  return (
    <View onClick={avail ? onClick : undefined} style={{ alignItems: 'center', gap: 6, width: 108, opacity: avail ? 1 : 0.45 }}>
      <View style={{ padding: 2, borderRadius: 10, border: `2px solid ${on ? t.accent : 'transparent'}` }}>
        <View style={{ width: 100, height: 56, borderRadius: 7, overflow: 'hidden', border: `1px solid ${t.separator}` }}>
          <Image src={motionThumb(it)} style={{ width: '100%', height: '100%', objectFit: 'cover' }} />
          {busy || percent != null ? <TileSpinner /> : null}
          {percent != null ? <View style={{ position: 'absolute', left: 0, bottom: 0, height: 4, width: `${percent}%`, backgroundColor: t.accent }} /> : null}
        </View>
      </View>
      <Text style={{ fontSize: typeScale.caption, color: on ? t.text : t.textDim, fontWeight: on ? 600 : 400, whiteSpace: 'nowrap', overflow: 'hidden', maxWidth: 108 }}>{name}</Text>
    </View>
  );
}

// Moving wallpapers: downloaded on demand, played only with a compatible GPU.
function MotionWallpapers({ login }) {
  const t = useTheme();
  const { t: tr } = useLocale();
  const [s, set] = useSettings();
  const codecs = useCodecs();
  const downloads = useStore(motionDownloads);
  const busy = useStore(motionBusy);
  const [scope, setScope] = useState('*');
  const [msg, setMsg] = useState('');
  const [pick, setPick] = useState(false);
  const [credits, setCredits] = useState(false);
  const can = codecs.length > 0;
  const displays = screenDisplays() || [];
  const main = displays[0] || { w: 1920, h: 1080 };
  const motion = s.motion || {};
  const cur = motion[scope] || null;
  const choose = (id) => { const m = { ...motion }; if (id) m[scope] = id; else delete m[scope]; set({ motion: m }); };
  const select = (it) => {
    setMsg('');
    const apply = () => (login ? setLoginMotion(it).then(() => { set({ loginWallpaper: it.id }); setMsg(tr('settings.motion.loginDone')); }, (e) => setMsg(errorText(e))) : choose(it.id));
    if ((s.motionFiles || {})[it.id]) { apply(); return; }
    const v = variantFor(it, codecs, main);
    if (!v) return;
    downloadMotion(it, v).then(apply, (e) => setMsg(errorText(e)));
  };
  const curItem = motionItems.find((x) => x.id === cur);
  const addFile = (path) => {
    setPick(false);
    media.probe(path).then((info) => {
      const why = userVideoProblem(info, codecs);
      if (why) { setMsg(tr(`settings.motion.problem.${why}`)); return; }
      const id = `user:${Date.now()}`;
      set({ motionUser: [...(s.motionUser || []), { id, name: path.split('/').pop(), path }] });
      choose(id);
    });
  };
  return (
    <>
      <Section title={tr('settings.motion.title')} footer={can ? tr('settings.motion.hint') : undefined}>
        {!can ? <Note>{tr('settings.motion.needsGpu')}</Note> : null}
        {msg ? <Note tone="warn">{msg}</Note> : null}
        {can && displays.length > 1 ? (
          <SettingRow label={tr('settings.motion.display')}>
            <Segmented small value={scope} onChange={setScope} options={[['*', tr('settings.motion.allDisplays')], ...displays.map((d) => [d.id, d.name || String(d.id)])]} />
          </SettingRow>
        ) : null}
        {can ? (
          <>
            {login ? null : <><SettingRow label={tr('settings.motion.speed')}>
              <Segmented small options={MOTION_RATES.map((r) => [r, `${r / 100}×`])} value={MOTION_RATES.includes(s.motionRate) ? s.motionRate : 100} onChange={(v) => set({ motionRate: v })} />
            </SettingRow></>}
            {!login && cur ? (
              <SettingRow label={curItem ? (tOptional(`settings.motion.item.${curItem.id}`) || curItem.name) : tr('settings.motion.random')}>
                <Actions>
                  {curItem ? <Button small label={tr('settings.motion.remove')} onClick={() => removeMotion(curItem).catch((e) => setMsg(errorText(e)))} /> : null}
                  <Button small label={tr('settings.motion.none')} onClick={() => choose(null)} />
                </Actions>
              </SettingRow>
            ) : null}
            <Actions style={{ paddingVertical: spacing.s }}>
              {login ? <Button small label={tr('settings.motion.loginDefault')} onClick={() => setLoginMotion(null).then(() => { set({ loginWallpaper: null }); setMsg(tr('settings.motion.loginReset')); }, (e) => setMsg(errorText(e)))} /> : null}
              {login ? null : <Button small label={tr('settings.motion.random')} disabled={Object.keys(s.motionFiles || {}).length < 2} onClick={() => choose('random')} />}
              {login ? null : <Button small label={tr('settings.motion.add')} onClick={() => setPick(true)} />}
              <Button small label={tr('settings.motion.credits')} onClick={() => setCredits(!credits)} />
            </Actions>
          </>
        ) : null}
      </Section>
      {MOTION_SECTIONS.map((sec) => (
        <Section key={sec} title={tr(`settings.wallpaperCategory.${sec}`)}>
          <View style={{ flexDirection: 'row', flexWrap: 'wrap', gap: 10, paddingVertical: spacing.m }}>
            {motionItems.filter((it) => it.section === sec).map((it) => (
              <MotionTile key={it.id} it={it} avail={can} on={(login ? s.loginWallpaper : cur) === it.id} percent={downloads[it.id]} busy={!login && busy && cur === it.id} onClick={() => select(it)} />
            ))}
          </View>
        </Section>
      ))}
      {credits ? (
        <Section title={tr('settings.motion.credits')}>
          <View style={{ gap: 6, paddingVertical: spacing.s }}>
            {motionItems.map((it) => (
              <Text key={it.id} style={{ fontSize: typeScale.caption, color: t.textDim, textAlign: 'start' }}>{`${tOptional(`settings.motion.item.${it.id}`) || it.name} — ${it.author} — ${it.license} — ${it.source}`}</Text>
            ))}
          </View>
        </Section>
      ) : null}
      {pick ? <FilePicker onPick={(p) => addFile(Array.isArray(p) ? p[0] : p)} onClose={() => setPick(false)} /> : null}
    </>
  );
}

// Grouped by category (Signature, Lumo, Cosmos, then the theme's own); grids use the thumbnails.
function Wallpapers() {
  const t = useTheme();
  const { t: tr } = useLocale();
  const [s, set] = useSettings();
  const list = t.wallpapers || [];
  if (!list.length) return <Stack><Note>{tr('settings.noWallpapers')}</Note></Stack>;
  const [target, setTarget] = useState('desktop');
  const login = target === 'login';
  const [note, setNote] = useState('');
  const shown = login ? s.loginWallpaper : s.wallpaper;
  const cur = findWallpaper(list, login ? (list.some((w) => w.id === shown) ? shown : GREETER_WALLPAPER) : s.wallpaper);
  const stillPick = (id) => {
    if (!login) { set({ wallpaper: id, motion: {} }); return; }
    setLoginStill(id).then(() => { set({ loginWallpaper: id }); setNote(tr('settings.motion.loginDone')); }, (e) => setNote(errorText(e)));
  };
  const cats = [...new Set([...WALLPAPER_CATEGORIES, ...list.map((w) => w.category || 'other')])]
    .map((c) => [c, list.filter((w) => (w.category || 'other') === c)]).filter(([, ws]) => ws.length);
  const catName = (c) => tOptional(`settings.wallpaperCategory.${c}`) || tr('settings.wallpaperCategory.other');
  return (
    <Stack>
      <Segmented value={target} onChange={(v) => { setTarget(v); setNote(''); }} options={[['desktop', tr('settings.wallpaperFor.desktop')], ['login', tr('settings.wallpaperFor.login')]]} />
      {note ? <Note>{note}</Note> : null}
      <Section>
        <View style={{ flexDirection: 'row', alignItems: 'center', gap: spacing.l, paddingVertical: spacing.m }}>
          <View style={{ width: 128, height: 72, borderRadius: 8, overflow: 'hidden', border: `1px solid ${t.separator}` }}>
            <Image src={wallpaperThumb(cur)} style={{ width: '100%', height: '100%', objectFit: 'cover' }} />
          </View>
          <View style={{ flex: 1, minWidth: 0, gap: 2 }}>
            <Text style={{ fontSize: typeScale.headline, fontWeight: 600, color: t.text, textAlign: 'start' }}>{wallpaperName(cur)}</Text>
            <Text style={{ fontSize: typeScale.caption, color: t.textDim, textAlign: 'start' }}>{catName(cur.category || 'other')}</Text>
          </View>
        </View>
      </Section>
      <MotionWallpapers login={login} />
      {cats.map(([c, ws]) => (
        <Section key={c} title={catName(c)}>
          {/* 108 px items with 10 px gaps: a category of five fits on one row in the default window */}
          <View style={{ flexDirection: 'row', flexWrap: 'wrap', gap: 10, paddingVertical: spacing.m }}>
            {ws.map((w) => (
              <View key={w.id} onClick={() => stillPick(w.id)} style={{ alignItems: 'center', gap: 6, width: 108 }}>
                <View style={{ padding: 2, borderRadius: 10, border: `2px solid ${cur.id === w.id ? t.accent : 'transparent'}` }}>
                  <View style={{ width: 100, height: 56, borderRadius: 7, overflow: 'hidden', border: `1px solid ${t.separator}` }}>
                    <Image src={wallpaperThumb(w)} style={{ width: '100%', height: '100%', objectFit: 'cover' }} />
                  </View>
                </View>
                <Text style={{ fontSize: typeScale.caption, color: cur.id === w.id ? t.text : t.textDim, fontWeight: cur.id === w.id ? 600 : 400,
                  whiteSpace: 'nowrap', overflow: 'hidden', maxWidth: 108 }}>{wallpaperName(w)}</Text>
              </View>
            ))}
          </View>
        </Section>
      ))}
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

const shortLocale = (name) => name.replace(/\.UTF-8$/i, '');

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
  const generated = (name) => locales.find((l) => l.name === name)?.generated;
  async function useRegion(r) {
    const name = `${r}.UTF-8`;
    if (!generated(name)) {
      const ok = await report(admin.run('locale-gen', name), tr('settings.region.generated', { name: r }));
      if (!ok) return;
      const l = locales.find((x) => x.name === name);
      if (l) l.generated = true;
    }
    lord.locale.set({ LANG: name });
    lord.keyboard?.reloadCompose?.();
    set({ region: r });
  }
  const pickLanguage = (id) => {
    setLanguage(id);
    const r = langs.find((l) => l.id === id)?.region;
    if (r && !s.region) useRegion(r);
  };

  const now = new Date();
  const regionItems = locales.map((l) => [shortLocale(l.name), shortLocale(l.name), l.generated ? '✓' : '']);
  const dim = { fontSize: typeScale.body, color: t.textDim, textAlign: 'end' };
  const langName = langs.find((l) => l.id === lang)?.name || lang;
  return (
    <Stack>
      <Section title={tr('settings.lang.title')} footer={tr('settings.lang.hint')}>
        {langs.map((l) => (
          <SettingRow key={l.id} label={l.name} hint={`${l.english}${l.user ? ` · ${tr('settings.lang.pack')}` : ''}`} onClick={() => pickLanguage(l.id)}>
            {l.id === lang ? <Text style={{ fontSize: 14, fontWeight: 600, color: t.accent }}>✓</Text> : null}
          </SettingRow>
        ))}
      </Section>
      <Section title={tr('settings.region.title')} footer={tr('settings.region.hint')}>
        <SettingRow label={regionName || '—'}>
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
          <Button small label={tr('settings.system.apply', { name: regionName || '—' })} onClick={() => regionName && report(admin.run('system-locale', `${regionName}.UTF-8`)).then((ok) => ok && setSys(`${regionName}.UTF-8`))} />
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

// LordOS language pack (built by tools/i18n-pack.mjs): lordos-lang-<id>.json in Downloads.
function PackImport({ onDone }) {
  const { t: tr } = useLocale();
  const found = [];
  for (const d of ['Descargas', 'Downloads', '.']) {
    try { for (const e of fs.list(d)) if (/^lordos-lang-[\w-]+\.json$/.test(e.name)) found.push(d === '.' ? e.name : `${d}/${e.name}`); } catch (e) { /* does not exist */ }
  }
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
      {themes.current() === 'default' && (
        <Section title={tr('settings.kb.desktopArrows.title')} footer={tr('settings.kb.desktopArrows.hint')}>
          <SettingRow label={tr('settings.kb.desktopArrows.label')}>
            <Toggle value={s.desktopArrows !== false} onChange={(v) => set({ desktopArrows: v })} />
          </SettingRow>
        </Section>
      )}
      <SwitcherSettings tr={tr} />
      <ShortcutsList />
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

// Sidebar groups (as in TangoOS): [id, component, icon color, symbol (a glyph or a component)].
const GROUPS = [
  [['general', General, '#8e8e93', 'i'], ['appearance', Appearance, '#3a3a3c', '◐'], ['wallpaper', Wallpapers, '#30b0c7', '▣'], ['dock', DockSettings, '#3a3a3c', '▁'], ['displays', DisplaysSettings, '#0a84ff', '▭']],
  [['network', NetworkSettings, '#0a84ff', '⇅'], ['bluetooth', BluetoothSettings, '#0a84ff', 'ᛒ'], ['sound', SoundSettings, '#ff375f', '♪'], ['notifications', NotificationsSettings, '#ff3b30', '◉'], ['printers', PrintersSettings, '#636366', '⎙'], ['clipboard', ClipboardSettings, '#5e5ce6', '⎘'], ['sharing', SharingSettings, '#34c759', '⇄']],
  [['language', LanguageRegion, '#30b0c7', 'A'], ['datetime', DateTimeSettings, '#0a84ff', '◷'], ['keyboard', Keyboard, '#8e8e93', '⌨'], ['trackpad', TrackpadSettings, '#8e8e93', '▭'], ['defaultapps', DefaultAppsSettings, '#8e8e93', '◫']],
  [['power', PowerSettings, '#30d158', BoltGlyph], ['cooling', CoolingSettings, '#32ade6', FanGlyph], ['firmware', FirmwareSettings, '#5e5ce6', ChipGlyph], ['privacy', PrivacySettings, '#0a84ff', LockGlyph], ['users', UsersSettings, '#0a84ff', '☺'], ['updates', Updates, '#8e8e93', '↻'], ['session', Session, '#ff453a', '↪']],
];
const SECTIONS = GROUPS.flat();
// Trackpad only if there is a touchpad (lord-sessiond lends them with the monitor); Board firmware
// only on a BCM2712 board (Raspberry Pi 5, 500, CM5: lord.system.info().board)
const isPi5 = () => { try { return lord.system.info().board === 'pi5'; } catch (e) { return false; } };
const sectionShown = (id, hasPad, pi5 = isPi5()) => (id !== 'trackpad' || hasPad) && (id !== 'firmware' || pi5);

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

// Other apps open a section: wm.open('settings', { args: { section: 'network', page: 'vpn' } }).
export default function Settings() {
  const t = useTheme();
  const { t: tr } = useLocale();
  const { args } = useAppWindow();
  const [section, setSectionState] = useState(lastSection);
  const [query, setQuery] = useState('');
  const setSection = (s) => { lastSection = s; setSectionState(s); };
  // 'themes' (the old Themes page, links from older help) now opens Appearance
  useEffect(() => { const sec = args?.section === 'themes' ? 'appearance' : args?.section; if (sec && SECTIONS.some(([id]) => id === sec)) setSection(sec); }, [args?.nonce]);
  const [, Body] = SECTIONS.find(([id]) => id === section) || SECTIONS[0];
  const q = query.trim().toLowerCase();
  // Trackpad only if there is a touchpad (lord-sessiond lends them with the monitor)
  const hasPad = useTrackpads().some((d) => d.kind === 'touchpad');
  const [pi5] = useState(isPi5);
  const visible = (id) => sectionShown(id, hasPad, pi5) && `${tr(`settings.section.${id}`)} ${tr(`settings.keywords.${id}`)}`.toLowerCase().includes(q);
  const groups = GROUPS.map((g) => g.filter(([id]) => visible(id))).filter((g) => g.length);
  return (
    <View style={{ flex: 1, flexDirection: 'row' }}>
      <View style={{ width: 220, backgroundColor: t.sidebarBg, paddingTop: spacing.m }}>
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
      <View style={{ flex: 1, minWidth: 0 }}>
        <View style={{ height: 48, flexDirection: 'row', alignItems: 'center', gap: spacing.s, paddingHorizontal: spacing.xxl }}>
          <Hairline edge="bottom" />
          <Text style={{ flex: 1, minWidth: 0, fontSize: typeScale.title, fontWeight: 600, color: t.text, whiteSpace: 'nowrap', overflow: 'hidden', textAlign: 'start' }}>
            {tr(`settings.section.${section}`)}</Text>
        </View>
        <ScrollView style={{ flex: 1 }}>
          <View style={{ paddingHorizontal: spacing.xxl, paddingTop: spacing.xl, alignItems: 'center' }}>
            <View style={{ width: '100%', maxWidth: 680 }}>
              <Body page={section === 'network' ? args?.page : undefined} />
            </View>
          </View>
        </ScrollView>
      </View>
    </View>
  );
}
