// Settings → Web apps (0.8.8.8, docs/WEBAPPS.md): each installed web app with its icon and origin, its profiles
// (add, rename, remove, with "delete its data"), its permissions (notifications, camera, microphone, screen,
// location: allow / ask / deny; what the person answered when asked shows here), run in background, how much each
// profile's data takes, each profile's Browser identity (the browser its pages are told it is: a closed list, never
// a typed string), and uninstall. Each profile has a color (the tag colors of Files): a dot on its windows, its
// notifications and its tray icon. Also: the tray icon while the window shows, the unread count on the Dock, one Dock
// icon per profile, whose sound a notification makes, and the site's own icon fetched again. Every change goes through lord.webapps, which validates it and writes the
// registry (the folder is closed to lord.fs). Apps come from the Store's Web apps section. Developer mode, one switch for
// every web app, adds Inspect (developer tools) to each web app window's Window menu and to the page's context menu
// (docs/WEBAPPS.md, "Developer tools"). Search the web with: the engine of the context menu's "Search the web for “…”"
// (Google, DuckDuckGo, Bing or Startpage; a closed list).
import {
  View, Text, useLocale, useTheme, useAppWindow, useWebApps, webAppsAvailable, webAppCall, webAppIcon, webAppsModel as M, launchWebApp,
  Button, Toggle, Segmented, TextInput, ConfirmDialog, Layer, fs, formatBytes, errorText, wm, spacing, typeScale, useSettings,
  webRunning, refreshSiteIcon, siteIconStatus, Spinner, webProfileIcon, webBadges, useStore, webDeveloper, setWebDeveloperMode,
  restartWebApp, quitWebApp, restartWebEngine, webSearchEngine, setWebSearchEngine,
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

// Each profile's data, measured on one of lord-server's workers (fs.duAsync, at most 5 s, and says when it stopped):
// a browser profile's cache holds thousands of files, and lord.fs.du here kept the pointer still for up to 300 ms per
// profile, every 30 s (2026-11-23). Measured again whenever a profile is added or removed or starts running
// (M.sizesKey): a profile added during the visit has no folder until its first start, and measuring only once showed it
// as "none yet" while in use. Also every 30 s while the page is open. Until the first answer the last ones stay.
function useSizes(entry, running) {
  const [sizes, setSizes] = useState({});
  const [tick, setTick] = useState(0);
  useEffect(() => { const id = setInterval(() => setTick((n) => n + 1), 30000); return () => clearInterval(id); }, []);
  const key = M.sizesKey(entry, running);
  useEffect(() => {
    let live = true;
    const home = fs.home();
    Promise.all(entry.profiles.map((p) => fs.duAsync(`${home}/${M.dataDir(entry.id, p.id)}`, 5000).then((r) => [p.id, r], () => [p.id, null])))
      .then((pairs) => { if (live) setSizes(Object.fromEntries(pairs)); });
    return () => { live = false; };
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

function ProfileRow({ entry, p, size, onMsg, onRemove, onAct, live, badge }) {
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
      {live ? <Button small label={tr('webapps.restart.ok')} onClick={() => onAct({ kind: 'restart', profile: p })} /> : null}
      {live ? <Button small label={tr('webapps.quit.ok')} onClick={() => onAct({ kind: 'quit', profile: p })} /> : null}
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
  // Refresh icon: a spinner while the probe runs, then the new icon (and a line saying so) or why there is none
  const iconState = useStore(siteIconStatus, (m) => m[entry.id] || null);
  const [iconAsked, setIconAsked] = useState(false);
  const iconBusy = iconState?.state === 'busy';
  const refreshIcon = () => { setIconAsked(true); refreshSiteIcon(entry.id); };
  const sounds = M.NOTIFY_SOUNDS.map((v) => [v, tr(`webapps.sound.${v}`)]);
  // Automatic, Light and Dark (webappsmodel.js, COLOR_SCHEMES_AVAILABLE; Light and Dark since 0.8.9.6.2, on WPEPlatform)
  const schemes = M.COLOR_SCHEMES.map((v) => [v, tr(`webapps.appearance.${v}`), !M.COLOR_SCHEMES_AVAILABLE.includes(v)]);
  const scheme = M.shownColorScheme(entry);
  const identities = M.USER_AGENTS.map((v) => [v, tr(`webapps.identity.${v}`)]);
  const one = entry.profiles.length < 2;
  const [msg, setMsg] = useState(null);
  const [adding, setAdding] = useState(null);       // the new profile's name being typed
  const [removing, setRemoving] = useState(null);   // { profile } or { app: true }
  const [wipe, setWipe] = useState(true);
  const [acting, setActing] = useState(null);       // { kind: 'restart' | 'quit', profile }: asked before it happens
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
  const confirmAct = () => {
    const a = acting;
    setActing(null);
    try { if (a.kind === 'quit') quitWebApp(entry.id, a.profile.id); else restartWebApp(entry.id, a.profile.id); setMsg(null); } catch (e) { setMsg({ ok: false, text: errorText(e) }); }
  };
  const actName = acting ? (one ? entry.name : tr('webapps.windowName', { app: entry.name, profile: acting.profile.name })) : '';
  const perms = M.permissionRows(entry);
  const options = [['allow', tr('webapps.perm.allow')], ['ask', tr('webapps.perm.ask')], ['deny', tr('webapps.perm.deny')]];
  return (
    <>
      <Section title={tr('webapps.settings.profiles')} footer={tr('webapps.settings.profilesFooter')}>
        {entry.profiles.map((p) => <ProfileRow key={p.id} entry={entry} p={p} size={sizes[p.id]} live={live.has(p.id)} badge={M.badgeLabel(M.badgeTotal(badges, entry.id, p.id))}
          onMsg={setMsg} onRemove={(profile) => { setWipe(true); setRemoving({ profile }); }} onAct={setActing} />)}
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
          <Toggle pending on={!!entry.background} onChange={(on) => call('setBackground', entry.id, on)} />
        </SettingRow>
        <SettingRow label={tr('webapps.settings.tray')} hint={tr('webapps.settings.trayHint')}>
          <Toggle pending on={!!entry.tray} onChange={(on) => call('setTray', entry.id, on)} />
        </SettingRow>
      </Section>
      <Section footer={tr('webapps.appearance.footer')}>
        <SettingRow label={tr('webapps.appearance.title')} hint={tr(`webapps.appearance.${scheme}Hint`)}>
          <Segmented small value={scheme} options={schemes} onChange={(v) => call('setColorScheme', entry.id, v)} />
        </SettingRow>
      </Section>
      <Section title={tr('webapps.identity.title')} footer={tr('webapps.identity.footer')}>
        {entry.profiles.map((p) => {
          const v = M.userAgentOf(entry, p.id);
          return (
            <SettingRow key={p.id} label={one ? tr('webapps.identity.label') : p.name} hint={tr(`webapps.identity.${v}Hint`)}>
              <PopUp value={v} options={identities} width={210} onChange={(x) => call('setUserAgent', entry.id, p.id, x)} />
            </SettingRow>
          );
        })}
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
          {iconBusy ? <Spinner size={14} /> : null}
          <Button small label={tr(iconBusy ? 'webapps.settings.iconRefreshing' : 'webapps.settings.iconRefresh')} disabled={iconBusy || !webAppsAvailable()} onClick={refreshIcon} />
        </SettingRow>
        {iconAsked && iconState && !iconBusy ? (
          <Message msg={iconState.state === 'ok' ? { ok: true, text: tr('webapps.settings.iconUpdated') } : { ok: false, text: errorText({ code: iconState.error, message: iconState.error }) }} />
        ) : null}
        <Actions style={{ paddingVertical: spacing.s }}>
          <Button small destructive label={tr('webapps.settings.uninstall')} onClick={() => { setWipe(true); setRemoving({ app: true }); }} />
        </Actions>
      </Section>
      {msg ? <Message msg={msg} /> : null}
      {acting ? (
        <Over>{(area) => (
          <ConfirmDialog area={area} destructive
            title={tr(acting.kind === 'quit' ? 'webapps.quit.title' : 'webapps.restart.title', { app: actName })}
            detail={tr(acting.kind === 'quit' ? 'webapps.quit.detail' : 'webapps.restart.detail')}
            okLabel={tr(acting.kind === 'quit' ? 'webapps.quit.ok' : 'webapps.restart.ok')} cancelLabel={tr('webapps.settings.cancel')}
            onOk={confirmAct} onCancel={() => setActing(null)} />
        )}</Over>
      ) : null}
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
  const developer = useStore(webDeveloper);
  const [devErr, setDevErr] = useState(null);
  const running = useRunning();
  const [engineAsk, setEngineAsk] = useState(false);
  const [engineMsg, setEngineMsg] = useState(null);
  const restartEngine = () => {
    setEngineAsk(false);
    try { restartWebEngine(); setEngineMsg({ ok: true, text: tr('webapps.settings.restartEngineDone') }); } catch (e) { setEngineMsg({ ok: false, text: errorText(e) }); }
  };
  const setDeveloper = (on) => { try { setWebDeveloperMode(on); setDevErr(null); } catch (e) { setDevErr({ ok: false, text: errorText(e) }); } };
  const engine = useStore(webSearchEngine);
  const engines = M.SEARCH_ENGINES.map((v) => [v, tr(`webapps.search.${v}`)]);
  const setEngine = (v) => { try { setWebSearchEngine(v); setDevErr(null); } catch (e) { setDevErr({ ok: false, text: errorText(e) }); } };
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
      <Section title={tr('webapps.search.title')} footer={tr('webapps.search.footer')}>
        <SettingRow label={tr('webapps.search.label')}>
          <PopUp value={engine} options={engines} width={210} onChange={setEngine} />
        </SettingRow>
      </Section>
      <Section title={tr('webapps.settings.engineTitle')} footer={tr('webapps.settings.engineFooter')}>
        <SettingRow label={tr('webapps.settings.restartEngine')} hint={tr('webapps.settings.restartEngineHint')}>
          <Button small label={tr('webapps.restart.ok')} disabled={!available || !running.length} onClick={() => setEngineAsk(true)} />
        </SettingRow>
      </Section>
      {engineMsg ? <Message msg={engineMsg} /> : null}
      {engineAsk ? (
        <Over>{(area) => (
          <ConfirmDialog area={area} destructive title={tr('webapps.settings.restartEngineTitle')} detail={tr('webapps.settings.restartEngineDetail')}
            okLabel={tr('webapps.restart.ok')} cancelLabel={tr('webapps.settings.cancel')} onOk={restartEngine} onCancel={() => setEngineAsk(false)} />
        )}</Over>
      ) : null}
      <Section title={tr('webapps.settings.developerTitle')} footer={tr('webapps.settings.developerFooter')}>
        <SettingRow label={tr('webapps.settings.developer')} hint={tr('webapps.settings.developerHint')}>
          <Toggle on={developer} onChange={setDeveloper} />
        </SettingRow>
      </Section>
      {devErr ? <Message msg={devErr} /> : null}
    </Stack>
  );
}
