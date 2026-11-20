// Settings → Flatpak (docs/FLATPAK.md, "Settings → Flatpak"): whether Flatpak is installed, how much space its apps and
// their data take, Flathub shown in the Store or not, and "Remove Flatpak completely": every Flathub app, the runtimes
// they leave unused and the Flathub remote (sdk/flathub.js, the closed list of flatpak commands), optionally each app's
// own data to the Trash, then the flatpak package through the package engine with its plan shown and the admin
// password asked (as Cleaner removes a package). Nothing of Flatpak runs in the background either way; this page only
// lets a person who tried it leave nothing behind.
import {
  View, Text, useLocale, useTheme, useAppWindow, useStore, Button, Toggle, ConfirmDialog, Layer, formatBytes, errorText, wm,
  spacing, typeScale, flathub as F, admin, useSettings,
} from '@lordos/sdk';
import { useEffect, useState } from 'react';
import { Stack, Section, SettingRow, ValueRow, Note, Actions, Message } from './ui.jsx';

// the libraries installing flatpak brought (docs/FLATPAK.md, test plan step 1): removed with it when nothing else needs them
const WITH = ['libostree-1-1', 'libappstream5', 'libmalcontent-0-0'];

function Over({ children }) {
  const win = useAppWindow();
  const g = wm.frame(wm.get(win.id) || {}) || {};
  return <Layer name="modal">{children({ x: g.x || 0, y: g.y || 0, w: g.w || 640, h: g.h || 480 })}</Layer>;
}

const du = (path) => { try { const r = lord.fs.du(path, 2000); return r && typeof r.size === 'number' ? r : null; } catch (e) { return null; } };
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
  const [sizes, setSizes] = useState({ apps: null, data: null });
  useEffect(() => {
    if (has) F.refreshInstalled();
    // measured once per visit (and after a removal), not on every render: the folders can hold gigabytes
    const home = (() => { try { return lord.fs.home(); } catch (e) { return ''; } })();
    const id = setTimeout(() => setSizes({ apps: has && home ? du(`${home}/.local/share/flatpak`) : null, data: home ? du(`${home}/.var/app`) : null }), 0);
    return () => clearTimeout(id);
  }, [has, gen]);
  const appsSize = sizes.apps, dataSize = sizes.data;
  const size = (r) => (r ? `${formatBytes(r.size)}${r.complete === false ? '+' : ''}` : '—');

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
        {has ? <ValueRow label={tr('flathub.settings.apps')} value={String(s.installed.length)} /> : null}
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
