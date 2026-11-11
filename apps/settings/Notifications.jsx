// Settings → Notifications: Do Not Disturb (now, or every day on a schedule), grouping, and per app:
// allow, style (none / banners / alerts), sound and lock screen. Packages (.lpk) use the sandbox's
// "notifications" permission for "allow", the same switch as Privacy & Security. Linux apps notify
// through org.freedesktop.Notifications (server/src/hw/notifyd.c). See docs/SETTINGS.md.
// Sound: Default, None or one of LordOS's sounds (sdk/notifytones.js), heard when chosen. A web app's row
// (lordos-web.<app>) shows its Notification sound from Settings → Web apps (the registry's notifySound, one choice for
// both pages) and, when LordOS's sound plays, which one.
import {
  View, Text, useTheme, useLocale, useSettings, Toggle, Segmented, Button, useStore, permsRev, packages, getApp, appName, useLauncherApps,
  notifyPrefs, appNotifyPrefs, setAppNotifyPrefs, dndActive, notify, direction,
  useWebApps, webAppIcon, webAppCall, webAppsModel as M, notifyTones as N, sound, errorText,
} from '@lordos/sdk';
import { useState } from 'react';
import { GroupBox, SettingRow, PopUp, Note, Stack, TimeField } from './ui.jsx';
import { compareNames } from '../../sdk/collate.js';

// Apps that can notify: the ones that already did, LordOS apps that use notifications and packages
// that declare the permission.
const BUILTIN = ['settings', 'backup', 'clock', 'terminal', 'store', 'mail', 'passwords', 'recorder', 'screenshots'];

function AppIcon({ app, linux, web }) {
  const t = useTheme();
  if (web) { const I = webAppIcon(web); return <I size={28} />; }
  if (app?.Icon) return <app.Icon size={28} />;
  if (linux?.Icon) return <linux.Icon size={28} />;
  return <View style={{ width: 28, height: 28, borderRadius: 7, backgroundColor: t.control }} />;
}

function AppRow({ id, name, app, linux, web, tr }) {
  const t = useTheme();
  const [s] = useSettings();
  useStore(permsRev);
  const [open, setOpen] = useState(false);
  const p = appNotifyPrefs(id, s);
  // whose sound: a web app's own choice (registry), then LordOS's sound when it plays one
  const webSound = web ? M.notifySoundOf(web) : null;
  const lordSound = !web || webSound === 'auto' || webSound === 'system';
  const tone = N.toneChoice(p.sound);
  const summary = !p.allow ? tr('prefs.notif.off')
    : [tr(`prefs.notif.style.${p.style}`), web ? tr(`webapps.sound.${webSound}`) : tone !== 'none' ? tr('prefs.notif.sound') : null].filter(Boolean).join(', ');
  const set = (x) => setAppNotifyPrefs(id, x);
  const pickTone = (c) => {
    if (!p.allow) return;
    set({ sound: N.prefOfChoice(c) });
    const play = N.toneOf(N.prefOfChoice(c));
    if (play) sound.play(play);                       // heard when chosen
  };
  const toneOptions = (web ? N.TONE_CHOICES.filter((c) => c !== 'none') : N.TONE_CHOICES).map((c) => [c, tr(`prefs.notif.tone.${c}`)]);
  return (
    <View>
      <SettingRow icon={<AppIcon app={app} linux={linux} web={web} />} label={name} hint={summary} onClick={() => setOpen(!open)}>
        <Text style={{ fontSize: 12, color: t.textDim }}>{open ? '⌃' : '⌄'}</Text>
      </SettingRow>
      {open && (
        <View style={{ paddingBottom: 10, [direction() === 'rtl' ? 'paddingRight' : 'paddingLeft']: 40, gap: 2 }}>
          <SettingRow label={tr('prefs.notif.allow')} hint={app?.package ? tr('prefs.notif.allowPackage') : ''}>
            <Toggle small on={p.allow} onChange={(allow) => set({ allow })} />
          </SettingRow>
          <SettingRow label={tr('prefs.notif.styleLabel')} hint={tr(`prefs.notif.styleHint.${p.style}`)} disabled={!p.allow}>
            <Segmented small value={p.style} onChange={(style) => p.allow && set({ style })}
              options={['none', 'banner', 'alert'].map((x) => [x, tr(`prefs.notif.style.${x}`)])} />
          </SettingRow>
          {web ? (
            <SettingRow label={tr('webapps.settings.notifySound')} hint={tr(`webapps.sound.${webSound}Hint`)} disabled={!p.allow}>
              <PopUp value={webSound} options={M.NOTIFY_SOUNDS.map((v) => [v, tr(`webapps.sound.${v}`)])}
                onChange={(v) => { if (p.allow) { try { webAppCall('setNotifySound', web.id, v); } catch (e) { notify({ app: 'settings', title: errorText(e) }); } } }} />
            </SettingRow>
          ) : null}
          {lordSound ? (
            <SettingRow label={tr(web ? 'prefs.notif.lordSound' : 'prefs.notif.sound')} disabled={!p.allow}>
              <PopUp value={web && tone === 'none' ? 'default' : tone} options={toneOptions} onChange={pickTone} />
            </SettingRow>
          ) : null}
          <SettingRow label={tr('prefs.notif.lockScreen')} hint={tr('prefs.notif.lockScreenHint')} disabled>
            <Toggle small on={p.lock} onChange={() => {}} />
          </SettingRow>
        </View>
      )}
    </View>
  );
}

export function NotificationsSettings() {
  const t = useTheme();
  const { t: tr } = useLocale();
  const [s, set] = useSettings();
  const linux = useLauncherApps().filter((a) => a.kind === 'linux');
  const webList = useWebApps();
  const p = notifyPrefs(s);
  const patch = (x) => set({ notifications: { ...p, ...x } });
  const now = dndActive();
  // the list: seen + built-in + packages with the permission
  const ids = [...new Set([...p.seen, ...BUILTIN.filter((id) => getApp(id)), ...packages.list().filter((x) => (x.permissions || []).includes('notifications')).map((x) => x.id),
    ...webList.map((e) => M.notifyKey(e.id))])];
  const rows = ids.map((id) => {
    const webId = M.appOfNotifyKey(id);
    if (webId) {
      const e = webList.find((x) => x.id === webId);
      return e ? { id, name: e.name, web: e } : null;
    }
    if (id.startsWith('linux:')) {
      const did = id.slice(6);
      const la = linux.find((a) => a.desktopId === did || a.desktopId.toLowerCase() === did.toLowerCase());
      return { id, name: la?.name || did, linux: la };
    }
    const app = getApp(id);
    return app ? { id, name: appName(app), app } : null;
  }).filter(Boolean).sort((a, b) => compareNames(a.name, b.name));
  const fmt = (m) => { const d = new Date(); d.setHours(Math.floor(m / 60), m % 60, 0, 0); return `${String(d.getHours()).padStart(2, '0')}:${String(d.getMinutes()).padStart(2, '0')}`; };
  return (
    <Stack>
      <GroupBox title={tr('prefs.notif.focus')} footer={tr('prefs.notif.dndFooter')}>
        <SettingRow icon={<View style={{ width: 28, height: 28, borderRadius: 7, backgroundColor: '#5e5ce6', alignItems: 'center', justifyContent: 'center' }}>
          <Text style={{ fontSize: 15, color: '#fff' }}>☾</Text></View>}
          label={tr('prefs.notif.dnd')} hint={now ? tr('prefs.notif.dndOnHint') : tr('prefs.notif.dndOffHint')}>
          <Toggle small on={p.dnd === true} onChange={(on) => patch({ dnd: on })} />
        </SettingRow>
        <SettingRow label={tr('prefs.notif.schedule')} hint={p.schedule.on ? tr('prefs.notif.scheduleHint', { from: fmt(p.schedule.from), to: fmt(p.schedule.to) }) : ''}>
          <Toggle small on={p.schedule.on} onChange={(on) => patch({ schedule: { ...p.schedule, on } })} />
        </SettingRow>
        {p.schedule.on && (
          <SettingRow label={tr('prefs.notif.from')}>
            <View style={{ flexDirection: 'row', alignItems: 'center', gap: 8 }}>
              <TimeField value={p.schedule.from} onChange={(from) => patch({ schedule: { ...p.schedule, from } })} />
              <Text style={{ fontSize: 12, color: t.textDim }}>{tr('prefs.notif.to')}</Text>
              <TimeField value={p.schedule.to} onChange={(to) => patch({ schedule: { ...p.schedule, to } })} />
            </View>
          </SettingRow>
        )}
      </GroupBox>
      <GroupBox>
        <SettingRow label={tr('prefs.notif.grouping')} hint={tr('prefs.notif.groupingHint')}>
          <PopUp value={p.grouping} onChange={(grouping) => patch({ grouping })} options={[['app', tr('prefs.notif.byApp')], ['off', tr('prefs.notif.noGrouping')]]} />
        </SettingRow>
        <SettingRow label={tr('prefs.notif.test')}>
          <Button small label={tr('prefs.notif.testButton')} onClick={() => notify({ app: 'settings', title: tr('prefs.notif.testTitle'), body: tr('prefs.notif.testBody') })} />
        </SettingRow>
      </GroupBox>
      <GroupBox title={tr('prefs.notif.apps')} footer={tr('prefs.notif.appsFooter')}>
        {rows.map((r) => <AppRow key={r.id} {...r} tr={tr} />)}
      </GroupBox>
    </Stack>
  );
}

