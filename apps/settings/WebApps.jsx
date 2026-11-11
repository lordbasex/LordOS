// Settings → Web apps (0.8.8.8, docs/WEBAPPS.md): each installed web app with its icon and origin, its profiles
// (add, rename, remove, with "delete its data"), its permissions (notifications, camera, microphone, screen,
// location: allow / ask / deny; what the person answered when asked shows here), run in background, how much each
// profile's data takes, and uninstall. Each profile has a color (the tag colors of Files): a dot on its windows, its
// notifications and its tray icon. Also: the tray icon while the window shows, the unread count on the Dock, one Dock
// icon per profile, whose sound a notification makes, and the site's own icon fetched again. Every change goes through lord.webapps, which validates it and writes the
// registry (the folder is closed to lord.fs). Apps come from the Store's Web apps section.
import {
  View, Text, useLocale, useTheme, useAppWindow, useWebApps, webAppsAvailable, webAppCall, webAppIcon, webAppsModel as M, launchWebApp,
  Button, Toggle, Segmented, TextInput, ConfirmDialog, Layer, fs, formatBytes, errorText, wm, spacing, typeScale, useSettings,
  webRunning, refreshSiteIcon, siteIconBusy, webProfileIcon, webBadges, useStore,
} from '@lordos/sdk';
import { useEffect, useState } from 'react';
import { Stack, Section, SettingRow, ValueRow, Note, Actions, Message, PopUp } from './ui.jsx';

// The running profiles, read again every 5 s while the page is open (lord.webapps.running is a table lookup).
function useRunning() {
  const [running, setRunning] = useState(() => webRunning());
  useEffect(() => {
    const id = setInterval(() => setRunning(webRunning()), 5000);
    return () => clearInterval(id);
  }, []);
  return running;
}

// Each profile's data (bounded: lord.fs.du stops after 300 ms and says so). Measured again whenever a profile is
// added or removed or starts running (M.sizesKey): a profile added during the visit has no folder until its first
// start, and measuring only once showed it as "none yet" while in use. Also every 30 s while the page is open.
function useSizes(entry, running) {
  const [sizes, setSizes] = useState({});
  const [tick, setTick] = useState(0);
  useEffect(() => { const id = setInterval(() => setTick((n) => n + 1), 30000); return () => clearInterval(id); }, []);
  const key = M.sizesKey(entry, running);
  useEffect(() => {
    const out = {};
    for (const p of entry.profiles) {
      try { out[p.id] = fs.du(`${fs.home()}/${M.dataDir(entry.id, p.id)}`, 300); } catch (e) { out[p.id] = null; }
    }
    setSizes(out);
  }, [entry.id, key, tick]);
  return sizes;
}

// The tag colors of Files and Databases as dots; the chosen one has a ring, "none" is the empty circle.
function ColorDots({ value, onPick }) {
  const { t: tr } = useLocale();
  const t = useTheme();
  const dot = (id, hex) => (
    <View key={id} onClick={() => onPick(id)} title={id === 'none' ? tr('webapps.settings.noColor') : tr(`files.tag.${id}`)}
      style={{ width: 20, height: 20, borderRadius: 10, alignItems: 'center', justifyContent: 'center', border: `2px solid ${value === id ? (hex || t.textDim) : 'transparent'}` }}>
      <View style={{ width: 12, height: 12, borderRadius: 6, backgroundColor: hex || 'transparent', border: hex ? '0 solid transparent' : `1.5px solid ${t.textDim}` }} />
    </View>
  );
  return (
    <View style={{ flexDirection: 'row', gap: 2, direction: 'ltr', alignItems: 'center' }}>
      {dot('none', null)}
      {M.PROFILE_COLORS.map((c) => dot(c, M.profileHex(c)))}
    </View>
  );
}

// A dialog over this Settings window (the page itself scrolls).
function Over({ children }) {
  const win = useAppWindow();
  const g = wm.frame(wm.get(win.id) || {}) || {};
  return <Layer name="modal">{children({ x: g.x || 0, y: g.y || 0, w: g.w || 640, h: g.h || 480 })}</Layer>;
}

function ProfileRow({ entry, p, size, onMsg, onRemove, live, badge }) {
  const { t: tr } = useLocale();
  const [name, setName] = useState(null);           // renaming: the text being typed
  const rename = () => {
    const n = String(name || '').trim();
    if (!M.nameOk(n)) { onMsg({ ok: false, text: tr('webapps.settings.badName') }); return; }
    try { webAppCall('renameProfile', entry.id, p.id, n); setName(null); } catch (e) { onMsg({ ok: false, text: errorText(e) }); }
  };
  // no folder: the profile never ran (a running one is measured again once its folder is there)
  const used = size ? (size.complete ? formatBytes(size.size) : tr('webapps.settings.sizeAtLeast', { size: formatBytes(size.size) })) : tr(live ? 'webapps.settings.sizeSoon' : 'webapps.settings.sizeNone');
  const color = M.profileColor(entry, p.id) || 'none';
  const Icon = webProfileIcon(entry, p.id);
  if (name !== null) {
    return (
      <SettingRow label={tr('webapps.settings.renameTo')}>
        <View style={{ width: 180 }}><TextInput value={name} onChange={setName} onSubmit={rename} onCancel={() => setName(null)} autoFocus selectAll /></View>
        <Button small label={tr('webapps.settings.cancel')} onClick={() => setName(null)} />
        <Button small primary label={tr('webapps.settings.save')} onClick={rename} />
      </SettingRow>
    );
  }
  return (
    <SettingRow icon={<Icon size={24} />} label={p.name}
      hint={[tr('webapps.settings.profileData', { size: used }), badge ? tr('webapps.settings.unread', { count: badge }) : ''].filter(Boolean).join(' · ')}>
      <ColorDots value={color} onPick={(c) => { try { webAppCall('setProfileColor', entry.id, p.id, c); } catch (e) { onMsg({ ok: false, text: errorText(e) }); } }} />
      <Button small label={tr('webapps.settings.open')} onClick={() => { try { launchWebApp(entry.id, p.id); } catch (e) { onMsg({ ok: false, text: errorText(e) }); } }} />
      <Button small label={tr('webapps.settings.rename')} onClick={() => setName(p.name)} />
      {M.canRemoveProfile(entry) ? <Button small destructive label={tr('webapps.settings.remove')} onClick={() => onRemove(p)} /> : null}
    </SettingRow>
  );
}

function AppDetails({ entry }) {
  const { t: tr } = useLocale();
  const th = useTheme();
  const [s, setS] = useSettings();
  const prefs = M.webPrefs(s, entry.id);
  const setPrefs = (x) => setS({ webAppPrefs: { ...(s.webAppPrefs || {}), [entry.id]: { ...prefs, ...x } } });
  const running = useRunning();
  const badges = useStore(webBadges);
  const [iconBusy, setIconBusy] = useState(false);
  const refreshIcon = () => {
    refreshSiteIcon(entry.id);
    setIconBusy(true);
    const id = setInterval(() => { if (!siteIconBusy(entry.id)) { clearInterval(id); setIconBusy(false); } }, 1000);
  };
  const sounds = M.NOTIFY_SOUNDS.map((v) => [v, tr(`webapps.sound.${v}`)]);
  const [msg, setMsg] = useState(null);
  const [adding, setAdding] = useState(null);       // the new profile's name being typed
  const [removing, setRemoving] = useState(null);   // { profile } or { app: true }
  const [wipe, setWipe] = useState(true);
  const sizes = useSizes(entry, running);
  const live = new Set(running.filter((r) => r.app === entry.id).map((r) => r.profile));
  const call = (...args) => { try { webAppCall(...args); setMsg(null); return true; } catch (e) { setMsg({ ok: false, text: errorText(e) }); return false; } };
  const add = () => {
    const n = String(adding || '').trim();
    if (!M.nameOk(n)) { setMsg({ ok: false, text: tr('webapps.settings.badName') }); return; }
    if (call('addProfile', entry.id, n)) setAdding(null);
  };
  const confirmRemove = () => {
    const r = removing;
    setRemoving(null);
    if (r.app) { if (call('uninstall', entry.id, wipe)) setMsg({ ok: true, text: tr('webapps.settings.uninstalled', { name: entry.name }) }); }
    else call('removeProfile', entry.id, r.profile.id, wipe);
  };
  const perms = M.permissionRows(entry);
  const options = [['allow', tr('webapps.perm.allow')], ['ask', tr('webapps.perm.ask')], ['deny', tr('webapps.perm.deny')]];
  return (
    <>
      <Section title={tr('webapps.settings.profiles')} footer={tr('webapps.settings.profilesFooter')}>
        {entry.profiles.map((p) => <ProfileRow key={p.id} entry={entry} p={p} size={sizes[p.id]} live={live.has(p.id)} badge={M.badgeLabel(M.badgeTotal(badges, entry.id, p.id))}
          onMsg={setMsg} onRemove={(profile) => { setWipe(true); setRemoving({ profile }); }} />)}
        {adding !== null ? (
          <SettingRow label={tr('webapps.settings.newProfile')}>
            <View style={{ width: 180 }}><TextInput value={adding} onChange={setAdding} onSubmit={add} onCancel={() => setAdding(null)} autoFocus selectAll /></View>
            <Button small label={tr('webapps.settings.cancel')} onClick={() => setAdding(null)} />
            <Button small primary label={tr('webapps.settings.add')} onClick={add} />
          </SettingRow>
        ) : M.canAddProfile(entry) ? (
          <Actions style={{ paddingVertical: spacing.s }}>
            <Button small label={tr('webapps.settings.addProfile')} onClick={() => setAdding(M.nextProfileName(entry, tr('webapps.profile.work')))} />
          </Actions>
        ) : null}
      </Section>
      <Section title={tr('webapps.settings.permissions')} footer={tr('webapps.settings.permissionsFooter')}>
        {perms.map(({ kind, value }) => (
          <SettingRow key={kind} label={tr(`webapps.perm.${kind}`)}>
            <Segmented small value={value} options={options} onChange={(v) => call('setPermission', entry.id, kind, v)} />
          </SettingRow>
        ))}
      </Section>
      <Section footer={tr('webapps.settings.backgroundFooter')}>
        <SettingRow label={tr('webapps.settings.background')}>
          <Toggle on={!!entry.background} onChange={(on) => call('setBackground', entry.id, on)} />
        </SettingRow>
        <SettingRow label={tr('webapps.settings.tray')} hint={tr('webapps.settings.trayHint')}>
          <Toggle on={!!entry.tray} onChange={(on) => call('setTray', entry.id, on)} />
        </SettingRow>
      </Section>
      <Section title={tr('webapps.settings.dockTitle')}>
        <SettingRow label={tr('webapps.settings.badge')} hint={tr('webapps.settings.badgeHint')}>
          <Toggle on={prefs.badge} onChange={(on) => setPrefs({ badge: on })} />
        </SettingRow>
        <SettingRow label={tr('webapps.settings.dockPerProfile')} hint={tr('webapps.settings.dockPerProfileHint')} disabled={entry.profiles.length < 2}>
          <Toggle on={prefs.dockPerProfile} onChange={(on) => setPrefs({ dockPerProfile: on })} />
        </SettingRow>
      </Section>
      <Section title={tr('webapps.settings.notifications')} footer={tr('webapps.settings.notifySoundFooter')}>
        <SettingRow label={tr('webapps.settings.notifySound')} hint={tr(`webapps.sound.${M.notifySoundOf(entry)}Hint`)}>
          <PopUp value={M.notifySoundOf(entry)} options={sounds} onChange={(v) => call('setNotifySound', entry.id, v)} />
        </SettingRow>
        <SettingRow label={tr('webapps.settings.moreNotify')}>
          <Button small label={tr('webapps.settings.openNotify')} onClick={() => wm.open('settings', { args: { section: 'notifications', nonce: Date.now() } })} />
        </SettingRow>
      </Section>
      <Section>
        <ValueRow label={tr('webapps.settings.origin')} value={entry.origin} mono />
        <ValueRow label={tr('webapps.settings.source')} value={tr(entry.source === 'catalog' ? 'webapps.settings.fromCatalog' : 'webapps.settings.fromAddress')} />
        <SettingRow label={tr('webapps.settings.icon')} hint={tr(entry.siteIcon ? 'webapps.settings.iconSite' : 'webapps.settings.iconTile')}>
          <Button small label={tr(iconBusy ? 'webapps.settings.iconRefreshing' : 'webapps.settings.iconRefresh')} disabled={iconBusy || !webAppsAvailable()} onClick={refreshIcon} />
        </SettingRow>
        <Actions style={{ paddingVertical: spacing.s }}>
          <Button small destructive label={tr('webapps.settings.uninstall')} onClick={() => { setWipe(true); setRemoving({ app: true }); }} />
        </Actions>
      </Section>
      {msg ? <Message msg={msg} /> : null}
      {removing ? (
        <Over>{(area) => (
          <ConfirmDialog area={area} destructive
            title={removing.app ? tr('webapps.settings.uninstallTitle', { name: entry.name }) : tr('webapps.settings.removeTitle', { name: removing.profile.name })}
            detail={tr(wipe ? 'webapps.settings.wipeOn' : 'webapps.settings.wipeOff')}
            okLabel={removing.app ? tr('webapps.settings.uninstall') : tr('webapps.settings.remove')} cancelLabel={tr('webapps.settings.cancel')}
            onOk={confirmRemove} onCancel={() => setRemoving(null)}>
            <View style={{ flexDirection: 'row', alignItems: 'center', gap: spacing.s, marginTop: spacing.xs }}>
              <Toggle small on={wipe} onChange={setWipe} />
              <Text style={{ flex: 1, fontSize: typeScale.body, color: th.text, textAlign: 'start' }}>{tr('webapps.settings.deleteData')}</Text>
            </View>
          </ConfirmDialog>
        )}</Over>
      ) : null}
    </>
  );
}

export function WebAppsSettings() {
  const { t: tr } = useLocale();
  const t = useTheme();
  const { args } = useAppWindow();
  const list = useWebApps();
  const [sel, setSel] = useState(args?.app || null);
  useEffect(() => { if (args?.app) setSel(args.app); }, [args?.nonce]);
  const available = webAppsAvailable();
  const store = () => wm.open('store', { args: { page: 'webapps', nonce: Date.now() } });
  const entry = list.find((e) => e.id === sel) || null;
  return (
    <Stack>
      {!available ? <Note tone="warn">{tr('webapps.settings.noRuntime')}</Note> : null}
      <Section title={tr('webapps.settings.installed')} footer={list.length ? tr('webapps.settings.footer') : tr('webapps.settings.none')}
        accessory={<Button small label={tr('webapps.settings.getMore')} onClick={store} />}>
        {list.map((e) => {
          const Icon = webAppIcon(e);
          const on = e.id === sel;
          return (
            <SettingRow key={e.id} icon={<Icon size={28} />} label={e.name}
              hint={`${M.hostOf(e.origin)} · ${tr('webapps.settings.profileCount', { count: e.profiles.length })}`} onClick={() => setSel(on ? null : e.id)}>
              <Text style={{ fontSize: typeScale.body, color: t.textDim }}>{on ? '⌄' : '›'}</Text>
            </SettingRow>
          );
        })}
      </Section>
      {entry ? <AppDetails key={entry.id} entry={entry} /> : null}
    </Stack>
  );
}
