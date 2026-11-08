// "greeter" theme: the LordOS login screen (docs/SESSIONS.md).
//
// It is not a theme you choose in Settings: lord-sessiond loads it with `lord-server --greeter`,
// as the system user lordos-greeter, without the administration helper. It shows the date,
// a big clock, the users and the password field. Everything that needs root (verifying the
// password, opening the session, rebooting, powering off) is requested with lord.greeter.send() and the answer
// arrives through __lordGreeter: this JS never sees /etc/shadow.
//
// Requests: {t:'hello'} · {t:'login', user, pass} · {t:'power', a:'reboot'|'poweroff'|'suspend'} · {t:'bootTestCancel'}
// Notices:  {t:'hello', mode, user, last, host, layouts, variants, lang, users:[{name, full, color, image, admin, open}],
//            bootTest?:{k, n, in}} (a boot time test restarts in `in` seconds; a login cancels it)
//           (last: the user whose session was last shown on the monitor)
//           {t:'mode', mode:'login'|'lock'|'switch', user, dark, off, sleep, sleepLeft} · {t:'ok'} · {t:'fail', wait} · {t:'error'}
//           · {t:'busy'} (dark, off, sleep, sleepLeft: a locked session's idle state, see idleOnLock)
import { render, screen, mainBox, onScreenChange, addListener } from '@lordos/runtime';
import {
  View, Text, Image, TextInput, ThemeProvider, useStore, locale, t, formatDate, formatTime, setLanguage, languages, CORE_LANGUAGES, keyboard, settings, LordOSWordmark,
  lordosWallpapers, findWallpaper, wallpaperSrc, GREETER_WALLPAPER, fs, osk, oskState, startOsk, OskPanel, OskIcon, oskLift, motionCodecs, media, MediaView,
  regionOf, regionCountry,
} from '@lordos/sdk';
import { useEffect, useRef, useState } from 'react';
import { createElement } from 'react';

const send = (msg) => { try { return lord.greeter.send(JSON.stringify(msg)); } catch (e) { return false; } };

// A locked session no longer has the monitor, so its idle deadlines move here (docs/SETTINGS.md): the
// screen goes off after `off` seconds without input, and the computer sleeps when the session's time is up
// (`sleepLeft` seconds after the lock, then `sleep` after the last input; lord-sessiond suspends,
// sdk/hardware.jsx onIdle). Locked because the screen had turned off (`dark`): this screen stays off
// until a key, the mouse or the trackpad. Any other showing has none.
function idleOnLock(m) {
  try {
    const lock = m.mode === 'lock';
    lord.display.setIdle(lock ? m.off || 0 : 0, 0, lock ? m.sleep || 0 : 0, lock ? m.sleepLeft || 0 : 0);
    if (lock && m.dark) lord.display.blank();
  } catch (e) { /* an older lord-server */ }
}

const listeners = new Set();
globalThis.__lordGreeter = (json) => {
  let m = null;
  try { m = JSON.parse(json); } catch (e) { return; }
  for (const fn of listeners) fn(m);
};

const PALETTE = [['#5b9cff', '#2f6fe0'], ['#f0915a', '#d0602b'], ['#4cc37a', '#269653'], ['#e0609d', '#b83574'],
  ['#a77bf3', '#7a4fd6'], ['#4cc4d3', '#2391a3'], ['#f2c14e', '#d0961d']];

// A looping video behind the clock, on the main display, when the file is there. It has no sound, and it only
// covers the wallpaper once it plays: no file, no helper or a crash leave the still image, with nothing on screen.
const LOGIN_PICKED = '/var/lib/lordos/wallpapers/login.mp4';      // chosen in Settings (wallpaper-login)
const LOGIN_BUILTIN = '/usr/share/backgrounds/lordos/motion/crown';
const LOGIN_STILL = '/var/lib/lordos/wallpapers/login-still';     // a still wallpaper's id: no video at all
const loginStill = () => { try { return String(fs.read(LOGIN_STILL)).trim(); } catch (e) { return ''; } };
// A picture of the user's own, kept there by wallpaper-login-image; the greeter never sees a home.
function loginImage(still) {
  const m = /^image:([0-9a-f]{16})$/.exec(still);
  if (!m) return null;
  const file = `/var/lib/lordos/wallpapers/login-${m[1]}.jpg`;
  try { return fs.stat(file) ? file : null; } catch (e) { return null; }
}
const IDLE_MS = 300000;
function LoginVideo({ box, stop }) {
  const [id, setId] = useState(0);
  useEffect(() => {
    if (stop || settings.get().reduceMotion || !media.available()) return undefined;
    let cur = 0, off = null, polls = 0, timer = 0, idle = 0, paused = false, last = Date.now();
    const start = (codecs) => {
      const files = [LOGIN_PICKED, ...codecs.map((c) => `${LOGIN_BUILTIN}.${c}.mp4`)];
      for (const f of files) {
        try { cur = media.play('greeter', f, { loop: true, audio: 0, hwOnly: 1, appName: 'LordOS' }); } catch (e) { cur = 0; }
        if (cur) break;
      }
      if (!cur) return;
      off = media.on(cur, (type, d) => {
        if (type === 'state' && d.state === 'playing') setId(cur);
        else if (type === 'error' || type === 'closed') setId(0);
      });
      // nobody at the screen for five minutes: it stops decoding until someone is back
      idle = setInterval(() => {
        const away = Date.now() - last > IDLE_MS;
        if (away !== paused) { paused = away; media.control(cur, away ? 'pause' : 'play'); }
      }, 5000);
    };
    const poll = () => {
      let c = [];
      try { c = motionCodecs(lord.media.hwDecode(), lord.media.composition()); } catch (e) { c = []; }
      if (c.length) start(c);                // hardware only: otherwise the still image stays
      else if (++polls < 40) timer = setTimeout(poll, 250);
    };
    poll();
    const seen = () => { last = Date.now(); };
    const offs = ['pointerdown', 'pointermove', 'keydown'].map((e) => addListener(e, seen));
    return () => {
      clearTimeout(timer); clearInterval(idle); offs.forEach((x) => x && x());
      if (off) off();
      setId(0);
      if (cur) media.close(cur);
    };
  }, [stop]);
  if (!id) return null;
  return <MediaView media={id} style={{ position: 'absolute', left: box.left, top: box.top, width: box.width, height: box.height }} />;
}

function hash(s) { let h = 0; for (const c of s) h = (h * 31 + c.codePointAt(0)) >>> 0; return h; }

function initials(u) {
  const words = (u.full || u.name).trim().split(/\s+/).filter(Boolean);
  const s = words.length > 1 ? words[0][0] + words[1][0] : (words[0] || u.name).slice(0, 1);
  return s.toUpperCase();
}

function shade(hex, f) {
  const n = parseInt(hex.slice(1), 16);
  const c = (v) => Math.max(0, Math.min(255, Math.round(v * f)));
  return `#${[n >> 16, (n >> 8) & 255, n & 255].map((v) => c(v).toString(16).padStart(2, '0')).join('')}`;
}

function colors(u) { return u.color ? [u.color, shade(u.color, 0.78)] : PALETTE[hash(u.name) % PALETTE.length]; }

function Avatar({ user, size, dim, onClick }) {
  const [a, b] = colors(user);
  return (
    <View onClick={onClick} style={{
      width: size, height: size, borderRadius: size / 2, alignItems: 'center', justifyContent: 'center', overflow: 'hidden',
      background: `linear-gradient(170deg, ${a}, ${b})`, opacity: dim ? 0.82 : 1,
      border: `${size > 60 ? 2 : 1}px solid rgba(255,255,255,${size > 60 ? 0.35 : 0.25})`,
      boxShadow: size > 60 ? '0 10px 30px rgba(0,0,0,0.35)' : '0 3px 10px rgba(0,0,0,0.3)',
    }}>
      {user.image
        ? <Image src={`/var/lib/lordos/users/${user.name}.png`} style={{ width: size, height: size, objectFit: 'cover' }} />
        : <Text style={{ fontSize: Math.round(size * 0.4), fontWeight: 600, color: 'rgba(255,255,255,0.95)' }}>{initials(user)}</Text>}
    </View>
  );
}

// Drawn power icon (doesn't depend on the font having ⏻)
function PowerIcon({ color }) {
  return (
    <View style={{ width: 14, height: 14 }}>
      <View style={{ position: 'absolute', left: 0, top: 1, width: 14, height: 14, borderRadius: 7, border: `1.6px solid ${color}` }} />
      <View style={{ position: 'absolute', left: 4, top: -1, width: 6, height: 5, backgroundColor: 'rgba(40,24,44,1)' }} />
      <View style={{ position: 'absolute', left: 6.2, top: -1, width: 1.6, height: 7, borderRadius: 1, backgroundColor: color }} />
    </View>
  );
}

function RoundButton({ children, onClick, label }) {
  const [hover, setHover] = useState(false);
  return (
    <View onClick={onClick} onMouseEnter={() => setHover(true)} onMouseLeave={() => setHover(false)} title={label}
      style={{ width: 32, height: 32, borderRadius: 16, alignItems: 'center', justifyContent: 'center',
        backgroundColor: hover ? 'rgba(255,255,255,0.22)' : 'rgba(255,255,255,0.1)', border: '1px solid rgba(255,255,255,0.12)' }}>
      {children}
    </View>
  );
}

function Pill({ label, icon, onClick, keepFocus }) {
  return (
    <View keepFocus={keepFocus} onClick={onClick} style={{ height: 26, paddingHorizontal: 10, borderRadius: 13, flexDirection: 'row', alignItems: 'center', gap: 5,
      backgroundColor: 'rgba(255,255,255,0.1)', border: '1px solid rgba(255,255,255,0.12)' }}>
      {icon || <Text style={{ fontSize: 12, fontWeight: 600, color: 'rgba(255,255,255,0.85)' }}>{label}</Text>}
    </View>
  );
}

// The language button: a drawn globe and the language's own name.
function GlobeLabel({ text }) {
  const c = 'rgba(255,255,255,0.85)';
  return (
    <View style={{ flexDirection: 'row', alignItems: 'center', gap: 6 }}>
      <View style={{ width: 13, height: 13, borderRadius: 7, border: `1.3px solid ${c}`, alignItems: 'center', justifyContent: 'center', overflow: 'hidden' }}>
        <View style={{ position: 'absolute', width: 5, height: 13, borderRadius: 3, border: `1.1px solid ${c}` }} />
        <View style={{ position: 'absolute', width: 13, height: 1.1, backgroundColor: c }} />
      </View>
      <Text style={{ fontSize: 12, fontWeight: 600, color: c }}>{text}</Text>
    </View>
  );
}

function PopupList({ items, onPick, onClose, right }) {
  return (
    <View onClick={onClose} style={{ position: 'absolute', left: 0, top: 0, width: screen.width, height: screen.height }}>
      <View onClick={(e) => e.stopPropagation()} style={{ position: 'absolute', right, bottom: 58, minWidth: 170, padding: 6, borderRadius: 12,
        backgroundColor: 'rgba(30,22,40,0.92)', border: '1px solid rgba(255,255,255,0.14)', boxShadow: '0 10px 30px rgba(0,0,0,0.45)' }}>
        {items.map(([id, label, on], i) => (id === '-'
          ? <View key={`sep${i}`} style={{ height: 1, marginVertical: 4, marginHorizontal: 6, backgroundColor: 'rgba(255,255,255,0.14)' }} />
          : <View key={id} onClick={() => { onPick(id); onClose(); }} style={{ height: 30, paddingHorizontal: 10, borderRadius: 7, flexDirection: 'row',
            alignItems: 'center', gap: 8, backgroundColor: on ? 'rgba(255,255,255,0.12)' : 'transparent' }}>
            <Text style={{ width: 12, fontSize: 12, color: '#fff' }}>{on ? '✓' : ''}</Text>
            <Text style={{ fontSize: 13, color: 'rgba(255,255,255,0.92)' }}>{label}</Text>
          </View>
        ))}
      </View>
    </View>
  );
}

function lanAddress() {
  try {
    for (const i of lord.net.interfaces()) {
      if (i.type === 'loopback' || /^(wg|ovpn|ips|ppp|oc|fvpn)-/.test(i.name)) continue;
      for (const a of i.addresses) if (a.family === 4 && a.scope === 'global') return a.address;
    }
  } catch (e) { /* no network */ }
  return '';
}

const capitalize = (s) => (s ? s[0].toUpperCase() + s.slice(1) : s);

function Greeter() {
  const { lang } = useStore(locale);
  const s = useStore(settings);
  useStore(oskState);                   // the on-screen keyboard: nobody at this screen may have another
  const [now, setNow] = useState(() => new Date());
  // a display plugged in or out while it shows: lay it out again on the main one
  useEffect(() => onScreenChange(() => setNow(new Date())), []);
  const [info, setInfo] = useState({ users: [], host: '', mode: 'login', user: '' });
  const [sel, setSel] = useState('');
  const [pass, setPass] = useState('');
  const [msg, setMsg] = useState('');
  const [busy, setBusy] = useState(false);
  const [shake, setShake] = useState(0);
  const [menu, setMenu] = useState(null);         // 'lang' | 'kb'
  const [entered, setEntered] = useState(false);  // the session is starting: the video stops
  const [confirm, setConfirm] = useState(null);   // 'reboot' | 'poweroff'
  const [ip, setIp] = useState(lanAddress());
  const timers = useRef([]);
  const reselect = useRef(true);                  // the next hello chooses the selected user
  // when the person last clicked an avatar (0: not since the last login). Input only reaches the
  // greeter while it's on the monitor, and until the 'mode' notice is handled the monitor still shows
  // the previous showing's last frame: a click made there a moment before the notice belongs to this
  // showing and must survive it.
  const chosen = useRef(0);
  const [focusRev, setFocusRev] = useState(0);      // when shown again, focus goes back to the field

  const doShake = () => {
    timers.current.forEach(clearTimeout);
    timers.current = [12, -11, 9, -7, 5, -3, 0].map((x, i) => setTimeout(() => setShake(x), i * 45));
  };

  useEffect(() => {
    const on = (m) => {
      if (m.t === 'hello') {
        // a boot time test restarts soon (LordBench, bench/boottest.h): when, by this screen's clock
        setInfo((old) => ({ ...old, ...m, bootTest: m.bootTest ? { ...m.bootTest, end: Date.now() + m.bootTest.in * 1000 } : null }));
        // system keyboard and language (/etc/default/keyboard and /etc/default/locale)
        const layouts = String(m.layouts || 'us').split(',').filter(Boolean);     // no file: US, as the server's (#55)
        const variants = String(m.variants || '').split(',');
        const kbs = layouts.map((l, i) => ({ layout: l, variant: variants[i] || '' }));
        if (JSON.stringify(settings.get().keyboards) !== JSON.stringify(kbs)) keyboard.set(kbs);
        // language: the one chosen here before or the system's (if there is no catalog, English)
        const shipped = languages().filter((l) => !l.user).map((l) => l.id);
        if (!shipped.includes(settings.get().language)) {
          const sys = String(m.lang || '').slice(0, 2);
          setLanguage(shipped.includes(sys) ? sys : 'en');
        }
        // region: the computer's, for the clock and the date (lt_LT: 24 hours, #45). This screen has no
        // choice of its own, so it follows /etc/default/locale even when that changes while it shows.
        const reg = regionOf(m.lang || '');
        const want = regionCountry(reg) ? reg : '';
        if ((settings.get().region || '') !== want) settings.set({ region: want });
        // the selected user: the one from the locked screen, another (switch user), the last one
        // shown on the monitor, one with an open session or the first. Only when the screen is shown
        // again, and never over an avatar the person clicked: a late reply must not undo that click.
        const choose = reselect.current && !chosen.current;
        reselect.current = false;
        setSel((cur) => {
          const has = (name) => !!name && m.users.some((u) => u.name === name);
          if (!choose && has(cur)) return cur;
          if (m.mode === 'lock' && has(m.user)) return m.user;
          if (m.mode === 'switch') return (m.users.find((u) => u.name !== m.user) || m.users[0] || {}).name || '';
          if (has(m.last)) return m.last;
          return (m.users.find((u) => u.open) || m.users[0] || {}).name || '';
        });
      } else if (m.t === 'mode') {
        idleOnLock(m);
        setPass(''); setMsg(''); setBusy(false); setMenu(null); setConfirm(null); setFocusRev((v) => v + 1);
        setInfo((old) => ({ ...old, mode: m.mode, user: m.user }));
        reselect.current = true;
        if (Date.now() - chosen.current > 10000) chosen.current = 0;   // a click from an earlier showing
        send({ t: 'hello' });                       // who has an open session, again
        setIp(lanAddress());
      } else if (m.t === 'ok') { setEntered(true); setPass(''); setMsg(''); setBusy(false); chosen.current = 0; osk.hide(); }
      else if (m.t === 'fail') {
        setPass(''); setBusy(false); doShake();
        setMsg(m.wait > 0 ? t('greeter.wait', { count: m.wait }) : t('greeter.wrong'));
      } else if (m.t === 'error') { setBusy(false); setMsg(t('greeter.error')); }
      else if (m.t === 'busy') { setBusy(false); setMsg(t('greeter.busy')); }
    };
    listeners.add(on);
    send({ t: 'hello' });
    // every 10 s: the network address and the user list (users can be created or deleted meanwhile)
    const id = setInterval(() => { setIp(lanAddress()); send({ t: 'hello' }); }, 10000);
    const tick = setInterval(() => setNow(new Date()), 1000);
    return () => { listeners.delete(on); clearInterval(id); clearInterval(tick); };
  }, []);

  const user = info.users.find((u) => u.name === sel);
  const others = info.users.filter((u) => u.name !== sel);
  const submit = () => {
    if (!user || busy) return;     // empty: only valid for the live ISO user (lord-sessiond decides)
    setBusy(true);
    setMsg('');
    if (!send({ t: 'login', user: user.name, pass })) { setBusy(false); setMsg(t('greeter.error')); }
  };
  const pick = (name) => { chosen.current = Date.now(); reselect.current = false; setSel(name); setPass(''); setMsg(''); };
  const anyOpen = info.users.some((u) => u.open);
  const power = (a) => { if (anyOpen && confirm !== a) setConfirm(a); else { setConfirm(null); send({ t: 'power', a }); } };

  // with several displays the clock, the user and the buttons go on the main one (they used to be laid
  // out over the whole desktop, cut at the seam); the wallpaper covers every display
  const M = mainBox(), H = M.height, W = M.width;
  const clockSize = Math.round(Math.min(H * 0.17, W * 0.12));
  const avatarSize = Math.round(Math.max(76, Math.min(110, H * 0.12)));
  const kbs = keyboard.configured();
  const kbIdx = Math.min(s.keyboardIndex || 0, kbs.length - 1);
  // the languages LordOS ships: English, Español and Português first, then the others (Français,
  // Italiano…) after a separator; a user's packs aren't readable from the login screen
  const shipped = languages().filter((l) => !l.user);
  const core = CORE_LANGUAGES.map((id) => shipped.find((l) => l.id === id)).filter(Boolean);
  const langs = [...core, ...shipped.filter((l) => !CORE_LANGUAGES.includes(l.id))];
  const langName = shipped.find((l) => l.id === lang)?.name || lang.toUpperCase();
  const locked = info.mode === 'lock' && user && user.name === info.user;
  // the column rises so the password stays above the on-screen keyboard
  const lift = oskLift(Math.round(H * 0.6) + 104 + (locked ? 16 : 0), H);

  const marked = loginStill();
  const image = loginImage(marked);
  const still = marked.startsWith('image:') ? '' : marked;
  return (
    <ThemeProvider tokens={{ mode: 'dark', accent: '#ffffff', text: '#ffffff', textDim: 'rgba(255,255,255,0.55)',
      inputBg: 'rgba(255,255,255,0.16)', separator: 'rgba(255,255,255,0.22)',
      panelBg: 'rgba(30,22,40,0.94)', control: 'rgba(255,255,255,0.18)', sidebarBg: 'rgba(255,255,255,0.09)' }}>
      <View style={{ width: screen.width, height: screen.height, direction: 'ltr' }}>
        <Image src={image || wallpaperSrc(findWallpaper(lordosWallpapers, still || GREETER_WALLPAPER), screen.width, screen.height) || 'assets/background.jpg'}
          style={{ position: 'absolute', left: 0, top: 0, width: screen.width, height: screen.height, objectFit: 'cover' }} />
        {still || image ? null : <LoginVideo box={M} stop={entered} />}
        <View style={{ position: 'absolute', left: M.left, top: M.top, width: W, height: H }}>

        {/* date and clock */}
        <View style={{ position: 'absolute', left: 0, top: Math.round(H * 0.085), width: W, alignItems: 'center' }}>
          <Text style={{ fontSize: Math.round(clockSize * 0.16), fontWeight: 500, color: 'rgba(255,255,255,0.88)' }}>
            {capitalize(formatDate(now, 'dayMonth'))}</Text>
          <Text style={{ fontSize: clockSize, fontWeight: 700, color: 'rgba(255,255,255,0.86)', marginTop: -Math.round(clockSize * 0.04),
            letterSpacing: -Math.round(clockSize * 0.03) }}>{formatTime(now)}</Text>
        </View>

        {/* LordBench's boot time test: which boot this was, the restart's countdown and a way to stop it */}
        {info.bootTest ? (
          <View style={{ position: 'absolute', left: Math.round(W / 2 - 230), top: 18, width: 460, padding: 12, gap: 8, borderRadius: 14,
            backgroundColor: 'rgba(30,22,40,0.88)', border: '1px solid rgba(255,255,255,0.18)', alignItems: 'center' }}>
            <Text style={{ fontSize: 13, color: '#fff', textAlign: 'center', lineHeight: 1.35 }}>
              {t('greeter.bootTest', { k: info.bootTest.k, n: info.bootTest.n, s: Math.max(0, Math.ceil((info.bootTest.end - now.getTime()) / 1000)) })}</Text>
            <Pill label={t('greeter.bootTestCancel')} onClick={() => send({ t: 'bootTestCancel' })} />
          </View>
        ) : null}

        {/* selected user and password */}
        <View style={{ position: 'absolute', left: 0, top: Math.round(H * 0.6) - avatarSize - lift, width: W, alignItems: 'center' }}>
          {user ? <Avatar user={user} size={avatarSize} /> : null}
          <Text style={{ marginTop: 14, fontSize: 16, fontWeight: 600, color: '#fff', textShadow: '0 1px 6px rgba(0,0,0,0.4)' }}>
            {user ? user.full || user.name : t('greeter.noUsers')}</Text>
          {locked ? <Text style={{ marginTop: 2, fontSize: 11, color: 'rgba(255,255,255,0.65)' }}>{t('greeter.locked')}</Text> : null}
          {user ? (
            <View style={{ marginTop: 12, flexDirection: 'row', alignItems: 'center', transform: [{ translateX: shake }] }}>
              <TextInput key={`${user.name}#${focusRev}`} secure autoFocus value={pass} onChange={(v) => { setPass(v); if (msg) setMsg(''); }} onSubmit={submit}
                onCancel={() => setPass('')} placeholder={t('greeter.password')} size={14}
                style={{ width: 210, height: 32, borderRadius: 16, paddingLeft: 14, paddingRight: 34, backgroundColor: 'rgba(255,255,255,0.16)',
                  border: '1px solid rgba(255,255,255,0.28)' }} />
              <View onClick={submit} style={{ position: 'absolute', right: 5, top: 5, width: 22, height: 22, borderRadius: 11, alignItems: 'center',
                justifyContent: 'center', backgroundColor: pass && !busy ? 'rgba(255,255,255,0.85)' : 'rgba(255,255,255,0.35)' }}>
                <Text style={{ fontSize: 13, fontWeight: 700, color: '#3a2a4a', marginTop: -1 }}>{busy ? '…' : '→'}</Text>
              </View>
            </View>
          ) : null}
          <Text style={{ marginTop: 10, height: 16, fontSize: 12, color: msg ? '#ffd2d2' : 'transparent' }}>{msg || ' '}</Text>
          {others.length ? (
            <View style={{ marginTop: 18, flexDirection: 'row', gap: 12 }}>
              {others.map((u) => (
                <View key={u.name} style={{ alignItems: 'center' }}>
                  <Avatar user={u} size={36} dim onClick={() => pick(u.name)} />
                  {u.open ? <View style={{ position: 'absolute', right: -1, bottom: -1, width: 9, height: 9, borderRadius: 5,
                    backgroundColor: '#30d158', border: '1.5px solid rgba(30,20,40,0.9)' }} /> : null}
                </View>
              ))}
            </View>
          ) : null}
        </View>

        {/* the LordOS logo: the crown and the wordmark as live text (docs/brand/README.md) */}
        <View style={{ position: 'absolute', left: 0, bottom: 24, width: W, height: 28, alignItems: 'center', justifyContent: 'center' }}>
          <LordOSWordmark size={17} color="#ffffff" />
        </View>

        {/* bottom: the machine's address; language, keyboard, reboot and power off */}
        <View style={{ position: 'absolute', left: 20, bottom: 20, height: 32, flexDirection: 'row', alignItems: 'center', gap: 7 }}>
          <View style={{ width: 13, height: 9, borderRadius: 2, border: '1.4px solid rgba(255,255,255,0.75)' }} />
          <Text style={{ fontSize: 12, color: 'rgba(255,255,255,0.8)' }}>{ip || info.host}</Text>
        </View>
        <View style={{ position: 'absolute', right: 20, bottom: 20, flexDirection: 'row', alignItems: 'center', gap: 10 }}>
          <Pill icon={<GlobeLabel text={langName} />} onClick={() => setMenu(menu === 'lang' ? null : 'lang')} />
          {kbs.length > 1 ? <Pill label={keyboard.label(kbs[kbIdx])} onClick={() => setMenu(menu === 'kb' ? null : 'kb')} /> : null}
          <Pill keepFocus icon={<OskIcon color="rgba(255,255,255,0.85)" size={14} />} onClick={osk.toggle} />
          <RoundButton label={t('greeter.restart')} onClick={() => power('reboot')}>
            <Text style={{ fontSize: 15, color: 'rgba(255,255,255,0.85)' }}>↻</Text>
          </RoundButton>
          <RoundButton label={t('greeter.shutdown')} onClick={() => power('poweroff')}><PowerIcon color="rgba(255,255,255,0.85)" /></RoundButton>
        </View>
        {confirm ? (
          <View style={{ position: 'absolute', right: 20, bottom: 64, width: 280, padding: 14, gap: 10, borderRadius: 14,
            backgroundColor: 'rgba(30,22,40,0.92)', border: '1px solid rgba(255,255,255,0.14)' }}>
            <Text style={{ fontSize: 13, color: '#fff', lineHeight: 1.35 }}>{t(confirm === 'reboot' ? 'greeter.confirmRestart' : 'greeter.confirmShutdown')}</Text>
            <View style={{ flexDirection: 'row', gap: 8, justifyContent: 'flex-end' }}>
              <Pill label={t('greeter.cancel')} onClick={() => setConfirm(null)} />
              <Pill label={t(confirm === 'reboot' ? 'greeter.restart' : 'greeter.shutdown')} onClick={() => power(confirm)} />
            </View>
          </View>
        ) : null}
        {menu === 'lang' ? <PopupList right={20 + (kbs.length > 1 ? 160 : 100)} onClose={() => setMenu(null)} onPick={(id) => setLanguage(id)}
          items={langs.flatMap((l, i) => [...(i === core.length && i > 0 ? [['-']] : []), [l.id, l.name, l.id === lang]])} /> : null}
        {menu === 'kb' ? <PopupList right={20 + 100} onClose={() => setMenu(null)} onPick={(i) => settings.set({ keyboardIndex: Number(i) })}
          items={kbs.map((k, i) => [String(i), keyboard.list().find((x) => x.id === k.layout)?.name || k.layout, i === kbIdx])} /> : null}
        </View>
        <OskPanel />
      </View>
    </ThemeProvider>
  );
}

// Changing the language remounts everything (the strings come from the catalogs).
function Root() {
  const { lang } = useStore(locale);
  return createElement(Greeter, { key: lang });
}

startOsk();
render(createElement(Root));
