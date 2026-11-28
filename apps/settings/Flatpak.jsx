// Settings → Flatpak (docs/FLATPAK.md, "Settings → Flatpak"): whether Flatpak is installed, how much space its apps and
// their data take, Flathub shown in the Store or not, and "Remove Flatpak completely": every Flathub app, the runtimes
// they leave unused and the Flathub remote (sdk/flathub.js, the closed list of flatpak commands), optionally each app's
// own data to the Trash, then the flatpak package through the package engine with its plan shown and the admin
// password asked (as Cleaner removes a package). Nothing of Flatpak runs in the background either way; this page only
// lets a person who tried it leave nothing behind.
import {
  View, Text, useLocale, useTheme, useStore, Button, Toggle, ConfirmDialog, WindowSheet, formatBytes, errorText, wm,
  spacing, typeScale, flathub as F, admin, useSettings, fs,
} from '@lordos/sdk';
import { useEffect, useState } from 'react';
import { Stack, Section, SettingRow, ValueRow, Note, Actions, Message } from './ui.jsx';

// the libraries installing flatpak brought (docs/FLATPAK.md, test plan step 1): removed with it when nothing else needs them
const WITH = ['libostree-1-1', 'libappstream5', 'libmalcontent-0-0'];

// a dialog over this Settings window's content, never above other windows (sdk/layers.js WindowSheet)
const Over = WindowSheet;

// The folders' sizes, measured on one of lord-server's workers (fs.duAsync): ~/.local/share/flatpak holds hundreds of
// thousands of files, and walking it here froze the pointer for up to 2 s on every visit (2026-11-23). Each answer is
// kept for SIZE_TTL, so leaving the page and coming back shows it at once; a request still running is shared.
const SIZE_TTL = 30000;
const sizes = new Map();          // path → { at, p: Promise<{ size, complete } | null>, r (once answered) }
function measure(path) {
  const c = sizes.get(path);
  if (c && (!('r' in c) || Date.now() - c.at < SIZE_TTL)) return c;
  const e = { at: Date.now() };
  e.p = fs.duAsync(path, 10000).then((r) => (r && typeof r.size === 'number' ? r : null), () => null)
    .then((r) => { e.r = r; e.at = Date.now(); return r; });
  sizes.set(path, e);
  return e;
}
const known = (path) => { const c = path && sizes.get(path); return c && 'r' in c && Date.now() - c.at < SIZE_TTL ? c.r : undefined; };
const homeDir = () => { try { return lord.fs.home(); } catch (e) { return ''; } };
const installed = (name) => { try { return !!lord.pkg.info(name, {})?.installed; } catch (e) { return false; } };

// The plan that removes flatpak (and the libraries it brought, when nothing else needs them), or null when the package
// engine would have to remove anything else than those and the packages they pulled in that nothing needs any more
// (pkg.why.auto): never a package something still uses, never lordos.
function removalPlan() {
  if (typeof lord === 'undefined' || !lord.pkg) return null;
  const tryPlan = (names) => {
    try {
      const p = lord.pkg.plan({ remove: names });
      const ok = p && p.ok && p.items.every((i) => i.action === 'remove' && (names.includes(i.name) || i.reason === 'pkg.why.auto')) &&
        !p.items.some((i) => i.name === 'lordos');
      return ok ? { names, items: p.items } : null;
    } catch (e) { return null; }
  };
  return tryPlan(['flatpak', ...WITH.filter(installed)]) || tryPlan(['flatpak']);
}

export function FlatpakSettings() {
  const { t: tr } = useLocale();
  const th = useTheme();
  useSettings((s) => s.flathub);
  const s = useStore(F.state);
  const [gen, setGen] = useState(0);
  const [ask, setAsk] = useState(null);           // the package plan, while the confirmation shows
  const [data, setData] = useState(true);
  const [busy, setBusy] = useState(false);
  const [msg, setMsg] = useState(null);
  const has = F.available();
  const prefs = F.prefs();
  // undefined: being measured; null: not known
  const [home] = useState(homeDir);
  const appsPath = has && home ? `${home}/.local/share/flatpak` : '', dataPath = home ? `${home}/.var/app` : '';
  const [appsSize, setAppsSize] = useState(() => (appsPath ? known(appsPath) : null));
  const [dataSize, setDataSize] = useState(() => (dataPath ? known(dataPath) : null));
  useEffect(() => {
    if (has) F.refreshInstalled();
    // measured once per visit (or kept from the last one) and after a removal, never on this thread
    let live = true;
    const want = (path, set) => {
      if (!path) { set(null); return; }
      if (gen) sizes.delete(path);
      const c = measure(path);
      if ('r' in c) set(c.r); else { set(undefined); c.p.then((r) => { if (live) set(r); }); }
    };
    want(appsPath, setAppsSize);
    want(dataPath, setDataSize);
    return () => { live = false; };
  }, [has, gen]);
  // an app installed or removed elsewhere (a terminal, a .flatpakref) changes the count while this page shows
  useEffect(() => (has ? F.watchInstalled() : undefined), [has]);
  const size = (r) => (r === undefined ? tr('files.info.calculating') : r ? `${formatBytes(r.size)}${r.complete === false ? '+' : ''}` : '—');

  const start = () => {
    setMsg(null);
    const plan = removalPlan();
    if (!plan) { setMsg({ ok: false, text: tr('flathub.settings.planRefused') }); return; }
    setData(true);
    setAsk(plan);
  };
  const run = async () => {
    const plan = ask;
    setAsk(null);
    setBusy(true);
    try {
      if (!(await F.removeAll({ data }))) { setMsg(F.state.get().msg || { ok: false, text: tr('flathub.settings.failed') }); return; }
      await admin.run('pkg-remove', `${plan.names.join(' ')} --purge`);   // its /etc files too (profile.d, Xsession.d)
      try { lord.flatpak.forget(); } catch (e) { /* its folders stay; nothing else depends on it */ }
      setMsg({ ok: true, text: tr('flathub.settings.removed') });
    } catch (e) {
      setMsg({ ok: false, text: errorText(e) });
    } finally {
      setBusy(false);
      setGen((g) => g + 1);
    }
  };

  return (
    <Stack>
      <Section title={tr('flathub.settings.title')} footer={tr('flathub.settings.footer')}>
        <ValueRow label={tr('flathub.settings.state')} value={tr(has ? 'flathub.settings.installed' : 'flathub.settings.notInstalled')} />
        {has ? (
          <SettingRow label={tr('flathub.settings.showInStore')} hint={tr('flathub.settings.showInStoreHint')}>
            <Toggle small pending on={prefs.on} onChange={(on) => F.setPrefs({ on })} />
          </SettingRow>
        ) : null}
        {has ? <ValueRow label={tr('flathub.settings.apps')} value={s.listed ? String(s.installed.length) : '—'} /> : null}
        {has ? <ValueRow label={tr('flathub.settings.appsSize')} value={size(appsSize)} /> : null}
        {dataSize && dataSize.size > 0 ? <ValueRow label={tr('flathub.settings.dataSize')} value={size(dataSize)} /> : null}
        <Actions style={{ paddingVertical: spacing.s }}>
          <Button small label={tr('flathub.settings.openStore')} onClick={() => wm.open('store', { args: { page: 'flathub', nonce: Date.now() } })} />
          {has ? <Button small destructive label={tr('flathub.settings.removeAll')} disabled={busy || !!s.job || !admin.available()} onClick={start} /> : null}
        </Actions>
      </Section>
      {busy || s.job ? <Note>{s.job ? tr(`flathub.job.${s.job.kind}`, { name: s.job.name }) : tr('flathub.settings.removing')}</Note> : null}
      {msg ? <Message msg={msg} /> : null}
      {ask ? (
        <Over>{(area) => (
          <ConfirmDialog area={area} destructive title={tr('flathub.settings.confirmTitle')}
            detail={tr('flathub.settings.confirmDetail', { count: s.installed.length, packages: ask.items.map((i) => i.name).join(', ') })}
            okLabel={tr('flathub.settings.removeAll')} cancelLabel={tr('flathub.settings.cancel')} onOk={run} onCancel={() => setAsk(null)}>
            <View style={{ flexDirection: 'row', alignItems: 'center', gap: spacing.s, marginTop: spacing.xs }}>
              <Toggle small on={data} onChange={setData} />
              <Text style={{ flex: 1, fontSize: typeScale.body, color: th.text, textAlign: 'start' }}>{tr('flathub.settings.alsoData')}</Text>
            </View>
          </ConfirmDialog>
        )}</Over>
      ) : null}
    </Stack>
  );
}
