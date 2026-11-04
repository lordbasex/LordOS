// Step 4 — customise the first boot (Raspberry Pi only): what goes into lordos-firstrun.txt. Every
// field is checked with the board's own rules (core/validate.js = server/src/firstrun/firstrun.c).
// Secrets stay in memory until the write starts; then only a hash and a derived key are kept.
import { View, Text, Button, ScrollView, useTheme, useLocale, typeScale, spacing, tOptional } from '@lordos/sdk';
import { useState } from 'react';
import { platform } from '../platform.js';
import { app, set, setCfg, setSecrets } from '../state.js';
import * as v from '../core/validate.js';
import * as firstrun from '../core/firstrun.js';
import { COOLERS, LANGUAGES, ACCENTS, THEMES, WALLPAPERS, profilesFor, defaultProfile, overclockAllowed, coolerHasFan, defaultConfig, langOnImage, deviceById } from '../core/catalog.js';
import { Field, SwitchRow, Choice, Card, Group, Notice, PickList, Press, Badge } from './kit.jsx';
import { CoolerArt, ThemeArt } from './art.jsx';
import countries from '../data/countries.js';

export const TABS = ['general', 'user', 'wifi', 'remote', 'look', 'cooling', 'more'];

export function customiseProblems(s) {
  const out = firstrun.check(s.cfg, s.secrets);
  if (!langOnImage(LANGUAGES.find((l) => l.id === s.cfg.lang), s.image)) out.push({ field: 'lang', key: 'imager.err.langImage' });
  return out;
}
const errFor = (s, field) => {
  const p = customiseProblems(s).find((x) => x.field === field);
  return p ? p.key : null;
};
function useErr(s) {
  const { t: tr } = useLocale();
  // a field shows its problem once it has a value, or after Next was pressed
  return (field, value) => {
    const k = errFor(s, field);
    return k && (s.showErrors || (value !== undefined && value !== '' && value !== null)) ? tr(k) : null;
  };
}

// The new system's language: English, Español and Português as chips, then "+ More languages"
// shows the others (Français, Italiano…). A language the chosen image can't start in is shown
// disabled, with the first image version that can.
function LanguagePick({ s }) {
  const t = useTheme();
  const { t: tr } = useLocale();
  const c = s.cfg;
  const [more, setMore] = useState(() => !LANGUAGES.find((l) => l.id === c.lang)?.core);
  const shown = LANGUAGES.filter((l) => l.core || more);
  const later = LANGUAGES.filter((l) => !l.core && !langOnImage(l, s.image));
  return (
    <View style={{ gap: 6 }}>
      <View {...platform.a11y({ label: tr('imager.general.language'), role: 'radiogroup' })} style={{ flexDirection: 'row', flexWrap: 'wrap', gap: 8 }}>
        {shown.map((l) => {
          const on = c.lang === l.id, ok = langOnImage(l, s.image);
          return (
            <Press key={l.id} id={`lang-${l.id}`} label={l.name} role="radio" selected={on} disabled={!ok} onClick={() => setCfg({ lang: l.id })}
              style={{ height: 28, paddingHorizontal: 14, borderRadius: 14, justifyContent: 'center', backgroundColor: on ? t.accent : t.control,
                border: `1px solid ${on ? t.accent : t.separator}` }}>
              <Text style={{ fontSize: 12, fontWeight: 600, whiteSpace: 'nowrap', color: on ? '#fff' : t.text }}>{l.name}</Text>
            </Press>
          );
        })}
        {!more ? (
          <Press id="lang-more" label={tr('imager.general.moreLanguages')} onClick={() => setMore(true)}
            style={{ height: 28, paddingHorizontal: 14, borderRadius: 14, justifyContent: 'center', border: `1px dashed ${t.textDim}` }}>
            <Text style={{ fontSize: 12, fontWeight: 600, whiteSpace: 'nowrap', color: t.text }}>{`+ ${tr('imager.general.moreLanguages')}`}</Text>
          </Press>
        ) : null}
      </View>
      {more && later.length ? (
        <Text style={{ fontSize: typeScale.caption, color: t.textDim, lineHeight: 1.4 }}>
          {later.map((l) => tr('imager.general.langSince', { name: l.name, v: l.since })).join(' ')}</Text>
      ) : null}
    </View>
  );
}

function General({ s }) {
  const { t: tr } = useLocale();
  const err = useErr(s);
  const c = s.cfg;
  const zones = platform.zones().map((z) => [z, z.replace(/_/g, ' ')]);
  const layouts = platform.layouts().flatMap((l) => [[l.id, l.name], ...l.variants.map(([id, name]) => [`${l.id}(${id})`, name])]);
  const kbId = c.keyboard ? (c.variant ? `${c.keyboard}(${c.variant})` : c.keyboard) : '';
  const pickKb = (id) => { const m = /^([a-z]+)(?:\(([\w-]+)\))?$/.exec(id); if (m) setCfg({ keyboard: m[1], variant: m[2] || '' }); };
  const cs = Object.keys(countries).map((k) => [k, `${tOptional(`countries.${k}`) || countries[k]} (${k})`]).sort((a, b) => a[1].localeCompare(b[1]));
  return (
    <View style={{ gap: spacing.xl }}>
      <Field id="host" label={tr('imager.general.hostname')} value={c.hostname} placeholder={v.suggestHostname(c.user.fullname || c.user.name)}
        onChange={(x) => setCfg({ hostname: x.toLowerCase().replace(/[^a-z0-9-]/g, '').slice(0, 63) })}
        error={err('hostname', c.hostname)} hint={tr('imager.general.hostnameHint', { name: c.hostname || v.suggestHostname(c.user.fullname || c.user.name) })} suffix=".local" />
      <View style={{ gap: 6 }}>
        <Text style={{ fontSize: typeScale.callout, fontWeight: 600, color: useTheme().text }}>{tr('imager.general.language')}</Text>
        <LanguagePick s={s} />
        {err('lang', c.lang) ? <Text style={{ fontSize: typeScale.caption, color: '#ff453a' }}>{err('lang', c.lang)}</Text> : null}
      </View>
      <View style={{ flexDirection: 'row', gap: spacing.l }}>
        <View style={{ flex: 1, gap: 6 }}>
          <Text style={{ fontSize: typeScale.callout, fontWeight: 600, color: useTheme().text }}>{tr('imager.general.zone')}</Text>
          <PickList id="zone" items={zones} value={c.timezone} onPick={(timezone) => setCfg({ timezone })} placeholder={tr('imager.search')} />
        </View>
        <View style={{ flex: 1, gap: 6 }}>
          <Text style={{ fontSize: typeScale.callout, fontWeight: 600, color: useTheme().text }}>{tr('imager.general.keyboard')}</Text>
          <PickList id="kb" items={layouts} value={kbId} onPick={pickKb} placeholder={tr('imager.search')} />
        </View>
      </View>
      <View style={{ gap: 6 }}>
        <Text style={{ fontSize: typeScale.callout, fontWeight: 600, color: useTheme().text }}>{tr('imager.general.country')}</Text>
        <PickList id="country" items={cs} value={c.wifiCountry} onPick={(wifiCountry) => setCfg({ wifiCountry })} placeholder={tr('imager.search')} height={110} />
        <Text style={{ fontSize: typeScale.caption, color: useTheme().textDim }}>{tr('imager.general.countryHint')}</Text>
      </View>
    </View>
  );
}

function Strength({ p }) {
  const t = useTheme();
  const { t: tr } = useLocale();
  if (!p) return null;
  const st = v.strength(p);
  const colors = ['#ff453a', '#ff6b3d', '#ff9f0a', '#a8d84b', '#30d158'];
  return (
    <View style={{ gap: 4 }}>
      <View style={{ flexDirection: 'row', gap: 4 }}>
        {[0, 1, 2, 3].map((i) => <View key={i} style={{ flex: 1, height: 4, borderRadius: 2, backgroundColor: i < Math.max(1, st.score) ? colors[st.score] : t.control }} />)}
      </View>
      <Text style={{ fontSize: typeScale.caption, color: t.textDim }}>
        {tr(`imager.user.strength.${st.score}`)}{st.hints.length ? ` · ${st.hints.map((h) => tr(h)).join(' · ')}` : ''}
      </Text>
    </View>
  );
}

function User({ s }) {
  const { t: tr } = useLocale();
  const err = useErr(s);
  const c = s.cfg, sec = s.secrets;
  const [show, setShow] = useState(false);
  const setUser = (p) => setCfg({ user: { ...c.user, ...p } });
  return (
    <View style={{ gap: spacing.xl }}>
      <Notice tone="info" title={tr('imager.user.introTitle')}>{tr('imager.user.intro')}</Notice>
      <Field id="full" label={tr('imager.user.fullname')} value={c.user.fullname} placeholder={tr('imager.user.fullnamePh')}
        onChange={(fullname) => setUser({ fullname, ...(c.user.edited ? {} : { name: v.suggestUser(fullname) }) })} error={err('fullname', c.user.fullname)} />
      <Field id="user" label={tr('imager.user.name')} value={c.user.name} placeholder="ana" mono
        onChange={(name) => setUser({ name: name.toLowerCase().replace(/[^a-z0-9_-]/g, '').slice(0, 32), edited: true })}
        error={err('user', c.user.name)} hint={tr('imager.user.nameHint')} />
      <View style={{ flexDirection: 'row', gap: spacing.l }}>
        <View style={{ flex: 1 }}>
          <Field id="pass" label={tr('imager.user.password')} value={sec.password} secure={!show} onChange={(password) => setSecrets({ password })}
            error={c.user.name ? err('password', sec.password || (s.showErrors ? '' : undefined)) : null} />
        </View>
        <View style={{ flex: 1 }}>
          <Field id="pass2" label={tr('imager.user.confirm')} value={sec.confirm} secure={!show} onChange={(confirm) => setSecrets({ confirm })}
            error={sec.confirm && sec.confirm !== sec.password ? tr('imager.err.passMismatch') : null} />
        </View>
      </View>
      <Strength p={sec.password} />
      <Press id="showpass" label={tr(show ? 'imager.user.hide' : 'imager.user.show')} onClick={() => setShow(!show)} style={{ alignSelf: 'flex-start', paddingVertical: 2, paddingHorizontal: 4, borderRadius: 5 }}>
        <Text style={{ fontSize: typeScale.callout, color: useTheme().accent }}>{tr(show ? 'imager.user.hide' : 'imager.user.show')}</Text>
      </Press>
      <Group footer={tr('imager.user.hashNote')}>
        <SwitchRow id="autologin" label={tr('imager.user.autologin')} detail={tr('imager.user.autologinHint')} on={c.user.autologin} onChange={(autologin) => setUser({ autologin })} />
      </Group>
    </View>
  );
}

function Wifi({ s }) {
  const t = useTheme();
  const { t: tr } = useLocale();
  const err = useErr(s);
  const w = s.cfg.wifi, sec = s.secrets;
  const [scan, setScan] = useState(null);
  const [show, setShow] = useState(false);
  const setW = (p) => setCfg({ wifi: { ...w, ...p } });
  const doScan = async () => {
    setScan('busy');
    try { const r = await platform.scanWifi(); setScan(r === null ? 'noRadio' : r.length ? r : 'none'); } catch { setScan('none'); }
  };
  return (
    <View style={{ gap: spacing.xl }}>
      <Group>
        <SwitchRow id="wifi" label={tr('imager.wifi.enable')} detail={tr('imager.wifi.enableHint')} on={w.enabled} onChange={(enabled) => setW({ enabled })} />
      </Group>
      {w.enabled ? (
        <>
          <View style={{ flexDirection: 'row', gap: spacing.s, alignItems: 'flex-end' }}>
            <View style={{ flex: 1 }}>
              <Field id="ssid" label={tr('imager.wifi.ssid')} value={w.ssid} onChange={(ssid) => setW({ ssid })} error={err('ssid', w.ssid)} placeholder={tr('imager.wifi.ssidPh')} />
            </View>
            {platform.scanWifi ? <Button label={tr(scan === 'busy' ? 'imager.wifi.scanning' : 'imager.wifi.scan')} onClick={doScan} disabled={scan === 'busy'} /> : null}
          </View>
          {Array.isArray(scan) && scan.length ? (
            <View style={{ flexDirection: 'row', flexWrap: 'wrap', gap: 6 }}>
              {scan.slice(0, 12).map((n) => (
                <Press key={n.ssid} label={n.ssid} onClick={() => setW({ ssid: n.ssid, open: n.security === 'open' })}
                  style={{ paddingHorizontal: 10, height: 24, borderRadius: 12, justifyContent: 'center', backgroundColor: n.ssid === w.ssid ? t.accent : t.control }}>
                  <Text style={{ fontSize: typeScale.caption, color: n.ssid === w.ssid ? '#fff' : t.text }}>{n.security === 'open' ? `${n.ssid} · ${tr('imager.wifi.openShort')}` : n.ssid}</Text>
                </Press>
              ))}
            </View>
          ) : scan === 'none' || scan === 'noRadio' ? <Text style={{ fontSize: typeScale.caption, color: t.textDim }}>{tr(scan === 'noRadio' ? 'imager.wifi.noRadio' : 'imager.wifi.scanNone')}</Text> : null}
          <Choice id="wsec" label={tr('imager.wifi.security')} value={w.open ? 'open' : 'wpa'} onChange={(x) => setW({ open: x === 'open' })}
            options={[['wpa', tr('imager.wifi.wpa')], ['open', tr('imager.wifi.open')]]} />
          {!w.open ? (
            <View style={{ gap: 6 }}>
              <Field id="wpass" label={tr('imager.wifi.password')} value={sec.wifiPassword} secure={!show} onChange={(wifiPassword) => setSecrets({ wifiPassword })}
                error={err('wifiPassword', sec.wifiPassword || (s.showErrors ? '' : undefined))} hint={tr('imager.wifi.passwordHint')} />
              <Press id="wshow" label={tr(show ? 'imager.user.hide' : 'imager.user.show')} onClick={() => setShow(!show)} style={{ alignSelf: 'flex-start', paddingVertical: 2, paddingHorizontal: 4, borderRadius: 5 }}>
                <Text style={{ fontSize: typeScale.callout, color: t.accent }}>{tr(show ? 'imager.user.hide' : 'imager.user.show')}</Text>
              </Press>
            </View>
          ) : <Notice tone="warn" title={tr('imager.wifi.openWarnTitle')}>{tr('imager.wifi.openWarn')}</Notice>}
          <Group>
            <SwitchRow id="hidden" label={tr('imager.wifi.hidden')} detail={tr('imager.wifi.hiddenHint')} on={w.hidden} onChange={(hidden) => setW({ hidden })} />
          </Group>
          <Text style={{ fontSize: typeScale.caption, color: t.textDim, lineHeight: 1.45 }}>{tr('imager.wifi.pskNote')}</Text>
        </>
      ) : <Text style={{ fontSize: typeScale.callout, color: t.textDim, lineHeight: 1.45 }}>{tr('imager.wifi.ethernet')}</Text>}
    </View>
  );
}

function Remote({ s }) {
  const t = useTheme();
  const { t: tr } = useLocale();
  const c = s.cfg;
  const [paste, setPaste] = useState('');
  const [msg, setMsg] = useState(null);
  const addKeys = (text) => {
    const r = v.sshKeys(text);
    const merged = [...c.sshKeys];
    for (const k of r.keys) if (!merged.some((x) => x.line === k.line) && merged.length < 16) merged.push(k);
    setCfg({ sshKeys: merged });
    setMsg(r.errors.length ? tr(r.errors[0]) : r.keys.length ? null : tr('imager.err.keyEmpty'));
    if (r.keys.length) setPaste('');
  };
  const noUser = !c.user.name;
  return (
    <View style={{ gap: spacing.xl }}>
      {noUser ? <Notice tone="warn" title={tr('imager.remote.needUserTitle')}>{tr('imager.remote.needUser')}</Notice> : null}
      <View style={{ gap: 8, opacity: noUser ? 0.5 : 1 }}>
        <Text style={{ fontSize: typeScale.headline, fontWeight: 600, color: t.text }}>{tr('imager.remote.ssh')}</Text>
        <Text style={{ fontSize: typeScale.callout, color: t.textDim, lineHeight: 1.45 }}>{tr('imager.remote.sshText')}</Text>
        <Choice id="ssh" label={tr('imager.remote.ssh')} value={c.ssh} onChange={(ssh) => !noUser && setCfg({ ssh })}
          options={[['off', tr('imager.remote.sshOff')], ['keys', tr('imager.remote.sshKeys')], ['password', tr('imager.remote.sshPassword')]]} />
        {c.ssh === 'password' ? <Notice tone="warn" title={tr('imager.remote.passwordWarnTitle')}>{tr('imager.remote.passwordWarn')}</Notice> : null}
        {c.ssh !== 'off' ? (
          <View style={{ gap: 8, marginTop: 4 }}>
            {c.sshKeys.map((k) => (
              <View key={k.line} style={{ flexDirection: 'row', alignItems: 'center', gap: 8, padding: 8, borderRadius: 8, backgroundColor: t.hover }}>
                <Badge tone="ok">{k.type.replace('sk-', '').replace('@openssh.com', '').replace('ecdsa-sha2-', 'ecdsa ')}</Badge>
                <Text style={{ flex: 1, fontSize: typeScale.caption, color: t.text, fontFamily: 'monospace', whiteSpace: 'nowrap', overflow: 'hidden', textOverflow: 'ellipsis' }}>
                  {k.comment || `${k.line.split(' ')[1].slice(0, 24)}…`}
                </Text>
                <Button small plain label={tr('imager.remote.remove')} onClick={() => setCfg({ sshKeys: c.sshKeys.filter((x) => x.line !== k.line) })} />
              </View>
            ))}
            <Field id="keypaste" label={tr('imager.remote.addKey')} value={paste} onChange={setPaste} placeholder="ssh-ed25519 AAAA… you@computer" mono onSubmit={() => addKeys(paste)}
              error={msg} hint={tr('imager.remote.addKeyHint')} />
            <View style={{ flexDirection: 'row', gap: spacing.s }}>
              <Button small label={tr('imager.remote.add')} onClick={() => addKeys(paste)} disabled={!paste.trim()} />
              <Button small label={tr('imager.remote.pickKey')} onClick={async () => { const txt = await platform.pickKeyFile(); if (txt) addKeys(txt); }} />
            </View>
          </View>
        ) : null}
      </View>
      <View style={{ height: 1, backgroundColor: t.separator }} />
      <View style={{ gap: 8, opacity: noUser ? 0.5 : 1 }}>
        <Text style={{ fontSize: typeScale.headline, fontWeight: 600, color: t.text }}>{tr('imager.remote.desktop')}</Text>
        <Group footer={tr('imager.remote.desktopFooter')}>
          <SwitchRow id="rdp" label={tr('imager.remote.desktopOn')} detail={tr('imager.remote.desktopHint', { host: c.hostname || 'lordos-pi' })} on={c.remote}
            onChange={(remote) => !noUser && setCfg({ remote })} disabled={noUser} />
        </Group>
      </View>
    </View>
  );
}

function Look({ s }) {
  const t = useTheme();
  const { t: tr } = useLocale();
  const c = s.cfg;
  const accent = ACCENTS.find((a) => a.id === c.accent)?.color || '#0a84ff';
  return (
    <View style={{ gap: spacing.xl }}>
      <View style={{ gap: 8 }}>
        <Text style={{ fontSize: typeScale.callout, fontWeight: 600, color: t.text }}>{tr('imager.look.style')}</Text>
        <View style={{ flexDirection: 'row', gap: spacing.l }}>
          {THEMES.map((th) => (
            <Card key={th} id={`theme-${th}`} label={tr(`imager.look.theme.${th}`)} selected={c.theme === th} onClick={() => setCfg({ theme: th })} style={{ alignItems: 'center', padding: 12 }}>
              <ThemeArt theme={th} mode={c.mode} accent={accent} width={170} />
              <Text style={{ fontSize: typeScale.body, fontWeight: 600, color: t.text }}>{tr(`imager.look.theme.${th}`)}</Text>
              <Text style={{ fontSize: typeScale.caption, color: t.textDim, textAlign: 'center', maxWidth: 170 }}>{tr(`imager.look.theme.${th}Hint`)}</Text>
            </Card>
          ))}
        </View>
      </View>
      <View style={{ gap: 8 }}>
        <Text style={{ fontSize: typeScale.callout, fontWeight: 600, color: t.text }}>{tr('imager.look.mode')}</Text>
        <Choice id="mode" label={tr('imager.look.mode')} value={c.mode} onChange={(mode) => setCfg({ mode })} options={[['light', tr('imager.look.light')], ['dark', tr('imager.look.dark')]]} />
      </View>
      <View style={{ gap: 8 }}>
        <Text style={{ fontSize: typeScale.callout, fontWeight: 600, color: t.text }}>{tr('imager.look.accent')}</Text>
        <View style={{ flexDirection: 'row', gap: 10 }} {...platform.a11y({ role: 'radiogroup', label: tr('imager.look.accent') })}>
          {ACCENTS.map((a) => (
            <Press key={a.id} id={`acc-${a.id}`} label={tr(`imager.look.accents.${a.id}`)} role="radio" selected={c.accent === a.id} onClick={() => setCfg({ accent: a.id })} ringRadius={14}
              style={{ width: 28, height: 28, borderRadius: 14, backgroundColor: a.color, alignItems: 'center', justifyContent: 'center', border: c.accent === a.id ? `3px solid ${t.windowBg}` : 'none',
                boxShadow: c.accent === a.id ? `0 0 0 2px ${a.color}` : 'none' }}>
              {c.accent === a.id ? <Text style={{ fontSize: 12, fontWeight: 700, color: '#fff' }}>✓</Text> : null}
            </Press>
          ))}
        </View>
      </View>
      <View style={{ gap: 8 }}>
        <Text style={{ fontSize: typeScale.callout, fontWeight: 600, color: t.text }}>{tr('imager.look.wallpaper')}</Text>
        <Choice id="wall" label={tr('imager.look.wallpaper')} value={c.wallpaper} onChange={(wallpaper) => setCfg({ wallpaper })}
          options={WALLPAPERS.map((w) => [w, tOptional(`shell.wallpaper.${w}`) || w])} />
      </View>
    </View>
  );
}

function Cooling({ s }) {
  const t = useTheme();
  const { t: tr } = useLocale();
  const c = s.cfg;
  const profiles = c.cooler ? profilesFor(c.cooler) : [];
  const noFan = deviceById(s.device)?.fan === false;   // the Pi 500: a heatsink inside, nothing to choose
  const pick = (cooler) => setCfg({ cooler, performance: profilesFor(cooler).includes(c.performance) ? c.performance : defaultProfile(cooler), overclock: overclockAllowed(cooler) && c.overclock });
  return (
    <View style={{ gap: spacing.xl }}>
      {noFan ? <Notice tone="info" title={tr('imager.cooling.noFanTitle')}>{tr('imager.cooling.noFan')}</Notice> : <Text style={{ fontSize: typeScale.callout, color: t.textDim, lineHeight: 1.45 }}>{tr('imager.cooling.text')}</Text>}
      {noFan ? null : <View style={{ flexDirection: 'row', gap: spacing.m, flexWrap: 'wrap' }}>
        {COOLERS.map((k) => (
          <Card key={k.id} id={`cool-${k.id}`} label={tr(`imager.cooling.${k.id}`)} selected={c.cooler === k.id} onClick={() => pick(k.id)} style={{ width: 150, alignItems: 'center', padding: 12 }}>
            <CoolerArt kind={k.id} size={96} />
            <Text style={{ fontSize: typeScale.body, fontWeight: 600, color: t.text, textAlign: 'center' }}>{tr(`imager.cooling.${k.id}`)}</Text>
            <Text style={{ fontSize: typeScale.caption, color: t.textDim, textAlign: 'center', lineHeight: 1.35 }}>{tr(`imager.cooling.${k.id}Hint`)}</Text>
          </Card>
        ))}
      </View>}
      {c.cooler ? (
        <View style={{ gap: 8 }}>
          <Text style={{ fontSize: typeScale.callout, fontWeight: 600, color: t.text }}>{tr('imager.cooling.profile')}</Text>
          <Choice id="perf" label={tr('imager.cooling.profile')} value={c.performance} onChange={(performance) => setCfg({ performance })}
            options={profiles.map((p) => [p, tr(`imager.cooling.p.${p}`)])} />
          <Text style={{ fontSize: typeScale.caption, color: t.textDim, lineHeight: 1.45 }}>{tr(`imager.cooling.p.${c.performance}${c.performance === 'balanced' && !coolerHasFan(c.cooler) ? 'Passive' : ''}Hint`)}</Text>
        </View>
      ) : <Text style={{ fontSize: typeScale.caption, color: t.textDim }}>{tr('imager.cooling.unset')}</Text>}
      {coolerHasFan(c.cooler) ? (
        <View style={{ gap: spacing.s }}>
          <Group>
            <SwitchRow id="oc" label={tr('imager.cooling.overclock')} detail={tr('imager.cooling.overclockHint')} on={c.overclock} onChange={(overclock) => setCfg({ overclock })} />
          </Group>
          {c.overclock ? <Notice tone="warn" title={tr('imager.cooling.overclockWarnTitle')}>{tr('imager.cooling.overclockWarn')}</Notice> : null}
        </View>
      ) : c.cooler === 'none' ? <Notice tone="warn" title={tr('imager.cooling.noneWarnTitle')}>{tr('imager.cooling.noneWarn')}</Notice> : null}
    </View>
  );
}

function More({ s }) {
  const t = useTheme();
  const { t: tr } = useLocale();
  const c = s.cfg;
  return (
    <View style={{ gap: spacing.xl }}>
      <Group>
        <SwitchRow id="bt" label={tr('imager.more.bluetooth')} detail={tr('imager.more.bluetoothHint')} on={c.bluetooth} onChange={(bluetooth) => setCfg({ bluetooth })} />
        <SwitchRow id="upd" label={tr('imager.more.updates')} detail={tr('imager.more.updatesHint')} on={c.autoUpdates} onChange={(autoUpdates) => setCfg({ autoUpdates })} />
      </Group>
      <Group title={tr('imager.more.privacy')}>
        <View style={{ paddingVertical: 10, gap: 4 }}>
          <Text style={{ fontSize: typeScale.body, fontWeight: 600, color: t.text }}>{tr('imager.more.telemetryTitle')}</Text>
          <Text style={{ fontSize: typeScale.callout, color: t.textDim, lineHeight: 1.45 }}>{tr('imager.more.telemetry')}</Text>
        </View>
      </Group>
    </View>
  );
}

function ProfilesSheet({ s, onClose }) {
  const t = useTheme();
  const { t: tr } = useLocale();
  const [list, setList] = useState(() => platform.loadProfiles());
  const [name, setName] = useState(s.cfg.hostname || '');
  const save = () => {
    if (!name.trim()) return;
    const p = firstrun.toProfile(name.trim(), s.cfg);
    const next = [...list.filter((x) => x.name !== p.name), p].slice(-20);
    platform.saveProfiles(next);
    setList(next);
  };
  const load = (p) => {
    const cfg = firstrun.fromProfile(p, defaultConfig());
    if (cfg) { set({ cfg }); onClose(); }
  };
  const del = (p) => { const next = list.filter((x) => x.name !== p.name); platform.saveProfiles(next); setList(next); };
  return (
    <View onClick={onClose} style={{ position: 'absolute', left: 0, top: 0, right: 0, bottom: 0, backgroundColor: 'rgba(0,0,0,0.3)', alignItems: 'center', justifyContent: 'center' }}>
      <View onClick={(e) => e.stopPropagation?.()} {...platform.a11y({ role: 'dialog', label: tr('imager.profiles.title') })}
        style={{ width: 440, padding: 20, gap: 12, borderRadius: t.groupRadius + 2, backgroundColor: t.windowBg, border: `1px solid ${t.separator}`, boxShadow: '0 14px 36px rgba(0,0,0,0.45)' }}>
        <Text style={{ fontSize: typeScale.headline, fontWeight: 600, color: t.text }}>{tr('imager.profiles.title')}</Text>
        <Text style={{ fontSize: typeScale.callout, color: t.textDim, lineHeight: 1.45 }}>{tr('imager.profiles.text')}</Text>
        <View style={{ flexDirection: 'row', gap: 8, alignItems: 'flex-end' }}>
          <View style={{ flex: 1 }}><Field id="pname" label={tr('imager.profiles.name')} value={name} onChange={(x) => setName(x.slice(0, 64))} onSubmit={save} /></View>
          <Button primary label={tr('imager.profiles.save')} onClick={save} disabled={!name.trim()} />
        </View>
        <View style={{ maxHeight: 220 }}>
          <ScrollView style={{ maxHeight: 220 }}>
            {list.length ? list.map((p) => (
              <View key={p.name} style={{ flexDirection: 'row', alignItems: 'center', gap: 8, paddingVertical: 6, borderTopWidth: 1, borderTopColor: t.separator }}>
                <Text style={{ flex: 1, fontSize: typeScale.body, color: t.text }}>{p.name}</Text>
                <Button small label={tr('imager.profiles.load')} onClick={() => load(p)} />
                <Button small plain label={tr('imager.profiles.delete')} onClick={() => del(p)} />
              </View>
            )) : <Text style={{ fontSize: typeScale.caption, color: t.textDim }}>{tr('imager.profiles.none')}</Text>}
          </ScrollView>
        </View>
        <View style={{ flexDirection: 'row', justifyContent: 'flex-end' }}><Button label={tr('imager.profiles.close')} onClick={onClose} /></View>
      </View>
    </View>
  );
}

export function CustomiseStep({ s }) {
  const t = useTheme();
  const { t: tr } = useLocale();
  const [profiles, setProfiles] = useState(false);
  const Body = { general: General, user: User, wifi: Wifi, remote: Remote, look: Look, cooling: Cooling, more: More }[s.tab];
  const problems = s.customise ? customiseProblems(s) : [];
  const tabOf = { hostname: 'general', timezone: 'general', user: 'user', fullname: 'user', password: 'user', ssid: 'wifi', wifiPassword: 'wifi',
    ssh: 'remote', remote: 'remote', overclock: 'cooling', performance: 'cooling' };
  const bad = new Set(problems.map((p) => tabOf[p.field]));
  return (
    <View style={{ flex: 1 }}>
      <View style={{ paddingHorizontal: 28, paddingTop: 24, gap: 12 }}>
        <View style={{ flexDirection: 'row', alignItems: 'flex-start', gap: spacing.l }}>
          <View style={{ flex: 1, gap: 6 }}>
            <Text style={{ fontSize: typeScale.largeTitle, fontWeight: 600, color: t.text }}>{tr('imager.custom.title')}</Text>
            <Text style={{ fontSize: typeScale.body, color: t.textDim, lineHeight: 1.45 }}>{tr('imager.custom.text')}</Text>
          </View>
          <Button small label={tr('imager.profiles.button')} onClick={() => setProfiles(true)} />
        </View>
        <Group>
          <SwitchRow id="custom-on" label={tr('imager.custom.toggle')} detail={tr(s.customise ? 'imager.custom.toggleOn' : 'imager.custom.toggleOff')} on={s.customise} onChange={(customise) => set({ customise })} />
        </Group>
        {s.customise ? (
          <View style={{ flexDirection: 'row', gap: 4, flexWrap: 'wrap', borderBottomWidth: 1, borderBottomColor: t.separator, paddingBottom: 8 }}
            {...platform.a11y({ role: 'tablist', label: tr('imager.custom.title') })}>
            {TABS.map((id) => (
              <Press key={id} id={`tab-${id}`} label={tr(`imager.tab.${id}`)} role="tab" selected={s.tab === id} onClick={() => set({ tab: id })}
                style={{ paddingHorizontal: 12, height: 28, borderRadius: 7, justifyContent: 'center', flexDirection: 'row', alignItems: 'center', gap: 5,
                  backgroundColor: s.tab === id ? (t.mode === 'dark' ? 'rgba(255,255,255,0.10)' : 'rgba(0,0,0,0.06)') : 'transparent' }}>
                <Text style={{ fontSize: typeScale.callout, fontWeight: s.tab === id ? 600 : 500, color: s.tab === id ? t.text : t.textDim }}>{tr(`imager.tab.${id}`)}</Text>
                {s.showErrors && bad.has(id) ? <View style={{ width: 6, height: 6, borderRadius: 3, backgroundColor: '#ff453a' }} /> : null}
              </Press>
            ))}
          </View>
        ) : null}
      </View>
      {s.customise ? (
        <ScrollView key={s.tab} style={{ flex: 1 }} contentStyle={{ paddingHorizontal: 28, paddingTop: 18, paddingBottom: 24 }}>
          {s.showErrors && problems.length ? <Notice tone="bad" title={tr('imager.custom.fix', { n: problems.length })} style={{ marginBottom: spacing.l }} /> : null}
          <Body s={s} />
        </ScrollView>
      ) : (
        <View style={{ flex: 1, paddingHorizontal: 28, paddingTop: 18 }}>
          <Notice tone="info" title={tr('imager.custom.skipTitle')}>{tr('imager.custom.skipText')}</Notice>
        </View>
      )}
      {profiles ? <ProfilesSheet s={s} onClose={() => setProfiles(false)} /> : null}
    </View>
  );
}
