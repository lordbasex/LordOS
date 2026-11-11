// webdetail.js — a web app's page in the Store ("See more", 0.8.8.8, docs/WEBAPPS.md), laid out like a package's page
// (debian.Details): the header (icon, name, description, Install / Open / Remove and the "Web app" tag), the facts row
// (version: always the site's own; when LordOS tested it; its data on disk; its memory), the description, and the
// information: publisher, the verified https origin, where it came from, the test, the engine, the permissions it may
// ask for and their state, its profiles, background mode and what doesn't work in LordOS's engine. A catalog app or one
// added from an address (webappsmodel.detailOf); the catalog's data is webappsmodel.catalogFacts.
import { createElement as h, useEffect, useState } from 'react';
import {
  View, Text, Button, GroupBox, Toggle, useTheme, useWebApps, webAppCall, webAppIcon, webAppMemory, webEngine, webAppsModel as M, launchWebApp,
  fs, formatBytes, formatDate, errorText, wm, typeScale, spacing, t,
} from '@lordos/sdk';
import { debian } from './debian.js';
import { Facts, Tag } from './ui.js';
import { open } from './addweb.js';
import { GetSteps, useGetWebApp } from './getweb.js';
import { CatalogIcon, webCatalogData } from './webapps.js';

const InfoRow = ({ th, label, value, children }) => h(View, { style: { minHeight: 34, flexDirection: 'row', alignItems: 'center', gap: spacing.m, paddingVertical: 4 } },
  h(Text, { style: { fontSize: typeScale.body, color: th.text, whiteSpace: 'nowrap', textAlign: 'start' } }, label),
  children || h(Text, { style: { flex: 1, minWidth: 0, fontSize: typeScale.body, color: th.textDim, textAlign: 'end', whiteSpace: 'nowrap', overflow: 'hidden' } }, value));
const Header = ({ th, children }) => h(Text, { style: { fontSize: typeScale.body, fontWeight: 600, color: th.text, paddingHorizontal: spacing.xs, marginBottom: 6, textAlign: 'start' } }, children);

// A closed padlock, drawn (the origin is https: only).
function Lock({ color }) {
  return h(View, { style: { width: 10, height: 13 } },
    h(View, { style: { position: 'absolute', left: 1.5, top: 0, width: 7, height: 9, borderRadius: 3.5, border: `1.5px solid ${color}` } }),
    h(View, { style: { position: 'absolute', left: 0, bottom: 0, width: 10, height: 7, borderRadius: 1.5, backgroundColor: color } }));
}

// Each profile's data, measured on each visit and after a change of profiles (bounded: lord.fs.du stops after 300 ms).
function useSizes(entry) {
  const [sizes, setSizes] = useState({});
  const key = entry ? entry.profiles.map((p) => p.id).join(',') : '';
  useEffect(() => {
    if (!entry) { setSizes({}); return; }
    const out = {};
    for (const p of entry.profiles) {
      try { out[p.id] = fs.du(`${fs.home()}/${M.dataDir(entry.id, p.id)}`, 300); } catch (e) { out[p.id] = null; }
    }
    setSizes(out);
  }, [entry && entry.id, key]);
  return sizes;
}

// The memory it takes now, read every 5 s while the page is open (cheap: lord-server sums smaps_rollup of its processes).
function useMemory(id, installed) {
  const [mem, setMem] = useState(() => (installed ? webAppMemory(id) : null));
  useEffect(() => {
    if (!installed) { setMem(null); return undefined; }
    setMem(webAppMemory(id));
    const timer = setInterval(() => setMem(webAppMemory(id)), 5000);
    return () => clearInterval(timer);
  }, [id, installed]);
  return mem;
}

const dateText = (iso) => formatDate(new Date(`${iso}T12:00:00`), 'short');

// Remove: the app and, unless unticked, its data (Settings → Web apps does the same).
function RemoveBox({ th, d, onMsg, onClose }) {
  const [wipe, setWipe] = useState(true);
  const go = () => {
    try { webAppCall('uninstall', d.id, wipe); onMsg({ ok: true, text: t('webapps.settings.uninstalled', { name: d.name }) }); } catch (e) { onMsg({ ok: false, text: errorText(e) }); }
    onClose();
  };
  return h(View, { style: { padding: spacing.xl, borderRadius: 10, border: `1px solid ${th.separator}`, gap: spacing.s } },
    h(Text, { style: { fontSize: typeScale.headline, fontWeight: 600, color: th.text, textAlign: 'start' } }, t('webapps.settings.uninstallTitle', { name: d.name })),
    h(View, { style: { flexDirection: 'row', alignItems: 'center', gap: spacing.s } },
      h(Toggle, { on: wipe, onChange: setWipe }),
      h(Text, { style: { flex: 1, fontSize: typeScale.callout, color: th.text, textAlign: 'start' } }, t('webapps.settings.deleteData'))),
    h(Text, { style: { fontSize: typeScale.caption, color: th.textDim, lineHeight: 1.4, textAlign: 'start' } }, t(wipe ? 'webapps.settings.wipeOn' : 'webapps.settings.wipeOff')),
    h(View, { style: { flexDirection: 'row', gap: spacing.s, justifyContent: 'flex-end' } },
      h(Button, { label: t('store.cancel'), onClick: onClose }),
      h(Button, { destructive: true, primary: true, label: t('store.remove'), onClick: go })));
}

export function WebAppDetail({ id }) {
  const th = useTheme();
  const installed = useWebApps();
  const d = M.detailOf(id, webCatalogData, installed);
  const a = d && d.catalog;
  const g = useGetWebApp(a || { id, name: d ? d.name : id }, debian);
  const sizes = useSizes(d && d.entry);
  const mem = useMemory(id, !!(d && d.installed));
  const [removing, setRemoving] = useState(false);
  const [msg, setMsg] = useState(null);
  if (!d) return h(Text, { style: { color: th.textDim, textAlign: 'start' } }, t('webapps.detail.notFound'));
  const facts = M.catalogFacts(a);
  const tested = a ? M.testedState(a) : null;
  const disk = M.diskUsage(d.entry, sizes);
  const memory = M.memoryView(mem, facts);
  const engine = webEngine();
  const host = M.hostOf(d.origin);
  const busy = M.getBusy(g.s);
  const testedText = !tested ? t('webapps.detail.testedAddress')
    : tested.state === 'verified' ? t('webapps.detail.testedOn', { date: dateText(tested.date) })
      : tested.state === 'tested' ? t('webapps.detail.testedYes') : t('webapps.detail.testedNo');
  const sizeText = !disk.installed ? t('webapps.detail.sizeNew')
    : disk.complete ? formatBytes(disk.total) : t('webapps.settings.sizeAtLeast', { size: formatBytes(disk.total) });
  const memText = memory.kind === 'live' ? formatBytes(memory.bytes)
    : memory.kind === 'typical' ? t('webapps.detail.memTypical', { size: formatBytes(memory.bytes) }) : t('webapps.detail.memUnknown');
  const memOf = (p) => memory.profiles.find((x) => x.profile === p);
  const call = (...args) => { try { webAppCall(...args); setMsg(null); } catch (e) { setMsg({ ok: false, text: errorText(e) }); } };
  const settings = () => wm.open('settings', { args: { section: 'webapps', app: id, nonce: Date.now() } });
  const icon = d.entry ? h(webAppIcon(d.entry), { size: 96 }) : h(CatalogIcon, { a, size: 96 });
  return h(View, { style: { gap: spacing.xl } },
    // header: icon, name, description and the actions under them, as on a package's page
    h(View, { style: { flexDirection: 'row', gap: spacing.l, alignItems: 'flex-start' } },
      h(View, { style: { borderRadius: 22, overflow: 'hidden', boxShadow: '0 6px 14px rgba(0,0,0,0.22)' } }, icon),
      h(View, { style: { flex: 1, minWidth: 0, gap: spacing.xs } },
        h(Text, { style: { fontSize: typeScale.largeTitle, fontWeight: 600, color: th.text, textAlign: 'start' } }, d.name),
        h(Text, { style: { fontSize: typeScale.body, color: th.textDim, lineHeight: 1.35, textAlign: 'start' } }, a ? t(a.description) : host),
        h(View, { style: { flexDirection: 'row', flexWrap: 'wrap', alignItems: 'center', gap: spacing.s, marginTop: spacing.s } },
          !d.installed ? h(Button, { primary: true, label: busy ? t('store.working') : t('store.install'), disabled: busy || !a || g.s.step === 'engine', onClick: () => a && !busy && g.start() }) : null,
          d.installed ? h(Button, { primary: true, label: t('store.open'), onClick: () => open(id) }) : null,
          d.installed ? h(Button, { label: t('store.remove'), onClick: () => setRemoving(!removing) }) : null,
          h(Tag, { label: t('store.kind.web') }),
          d.installed ? h(Text, { style: { fontSize: typeScale.caption, fontWeight: 500, color: '#30d158', marginStart: spacing.xs } }, t('store.installedBadge')) : null))),
    a && !d.installed ? h(GetSteps, { a, g, pkg: debian, onOpen: open }) : null,
    removing && d.installed ? h(RemoveBox, { th, d, onMsg: setMsg, onClose: () => setRemoving(false) }) : null,
    msg ? h(Text, { style: { fontSize: typeScale.callout, color: msg.ok ? '#30d158' : '#ff453a', textAlign: 'start' } }, msg.text) : null,
    h(Facts, { items: [
      [t('store.info.version'), t('webapps.detail.versionValue')],
      [t('webapps.detail.tested'), testedText],
      [t('store.info.size'), sizeText],
      [t('webapps.detail.memory'), memText],
    ] }),
    h(View, null,
      h(Header, { th }, t('store.info.description')),
      h(View, { style: { gap: spacing.s, maxWidth: 620, paddingHorizontal: spacing.xs } },
        h(Text, { style: { fontSize: typeScale.body, color: th.text, lineHeight: 1.45, textAlign: 'start' } }, a ? t(a.description) : t('webapps.detail.addressDesc', { host })),
        h(Text, { style: { fontSize: typeScale.body, color: th.text, lineHeight: 1.45, textAlign: 'start' } }, t('webapps.store.whatHappens')))),
    h(View, null,
      h(Header, { th }, t('store.info.title')),
      h(GroupBox, null,
        h(InfoRow, { th, label: t('webapps.detail.publisher'), value: facts.publisher || host }),
        h(InfoRow, { th, label: t('webapps.detail.origin') },
          h(View, { style: { flex: 1, minWidth: 0, flexDirection: 'row', alignItems: 'center', justifyContent: 'flex-end', gap: 6 } },
            h(Lock, { color: '#30d158' }),
            h(Text, { style: { fontSize: typeScale.body, color: th.textDim, direction: 'ltr', whiteSpace: 'nowrap', overflow: 'hidden' } }, d.origin))),
        h(InfoRow, { th, label: t('webapps.detail.source'), value: t(d.source === 'catalog' ? 'webapps.detail.sourceCatalog' : 'webapps.detail.sourceAddress') }),
        h(InfoRow, { th, label: t('webapps.detail.tested'), value: testedText }),
        h(InfoRow, { th, label: t('webapps.detail.engine'),
          value: !engine.installed ? t('webapps.detail.engineMissing') : engine.webkit ? t('webapps.detail.engineValue', { version: engine.webkit }) : t('webapps.detail.engineName') }))),
    h(View, null,
      h(Header, { th }, t('webapps.detail.permissions')),
      h(GroupBox, null,
        ...M.permissionRows(d.entry).map(({ kind, value }) => h(InfoRow, { key: kind, th, label: t(`webapps.perm.${kind}`), value: t(`webapps.perm.${value}`) })),
        d.installed ? h(View, { style: { flexDirection: 'row', justifyContent: 'flex-end', paddingVertical: spacing.s } },
          h(Button, { small: true, label: t('webapps.detail.changeInSettings'), onClick: settings })) : null),
      !d.installed ? h(Text, { style: { fontSize: typeScale.caption, color: th.textDim, lineHeight: 1.4, paddingHorizontal: spacing.xs, marginTop: 6, textAlign: 'start' } }, t('webapps.detail.permissionsBefore')) : null),
    h(View, null,
      h(Header, { th }, t('webapps.settings.profiles')),
      h(GroupBox, null,
        ...(d.entry ? disk.profiles.map((p) => h(InfoRow, { key: p.id, th, label: p.name },
          h(View, { style: { flex: 1, minWidth: 0, flexDirection: 'row', alignItems: 'center', justifyContent: 'flex-end', gap: spacing.s } },
            h(Text, { style: { flex: 1, minWidth: 0, fontSize: typeScale.callout, color: th.textDim, textAlign: 'end', whiteSpace: 'nowrap', overflow: 'hidden' } },
              memOf(p.id) ? t('webapps.detail.profileUseRunning', { size: formatBytes(p.bytes), memory: formatBytes(memOf(p.id).bytes) }) : t('webapps.detail.profileUse', { size: formatBytes(p.bytes) })),
            h(Button, { small: true, label: t('store.open'), onClick: () => { try { launchWebApp(id, p.id); } catch (e) { setMsg({ ok: false, text: errorText(e) }); } } }))))
          : [h(InfoRow, { key: 'first', th, label: t('webapps.profile.default'), value: t('webapps.detail.profileFirst') })]),
        d.entry && M.canAddProfile(d.entry) ? h(View, { style: { flexDirection: 'row', justifyContent: 'flex-end', paddingVertical: spacing.s } },
          h(Button, { small: true, label: t('webapps.settings.newProfile'), onClick: () => call('addProfile', id, M.nextProfileName(d.entry, t('webapps.profile.work'))) })) : null),
      h(Text, { style: { fontSize: typeScale.caption, color: th.textDim, lineHeight: 1.4, paddingHorizontal: spacing.xs, marginTop: 6, textAlign: 'start' } }, t('webapps.settings.profilesFooter'))),
    h(View, null,
      h(Header, { th }, t('webapps.detail.background')),
      h(GroupBox, null,
        h(InfoRow, { th, label: t('webapps.settings.background') },
          d.installed ? h(View, { style: { flex: 1, alignItems: 'flex-end' } }, h(Toggle, { on: !!d.entry.background, onChange: (on) => call('setBackground', id, on) }))
            : h(Text, { style: { flex: 1, minWidth: 0, fontSize: typeScale.body, color: th.textDim, textAlign: 'end' } }, t(d.background ? 'webapps.detail.backgroundOn' : 'webapps.detail.backgroundOff')))),
      h(Text, { style: { fontSize: typeScale.caption, color: th.textDim, lineHeight: 1.4, paddingHorizontal: spacing.xs, marginTop: 6, textAlign: 'start' } }, t('webapps.settings.backgroundFooter'))),
    h(View, null,
      h(Header, { th }, t('webapps.detail.limitations')),
      h(GroupBox, null,
        ...(facts.limitations.length ? facts.limitations.map((k) => h(Text, { key: k, style: { fontSize: typeScale.body, color: th.text, lineHeight: 1.4, paddingVertical: 8, textAlign: 'start' } }, `• ${t(`webapps.limit.${k}`)}`))
          : [h(Text, { key: 'none', style: { fontSize: typeScale.body, color: th.textDim, lineHeight: 1.4, paddingVertical: 8, textAlign: 'start' } }, t(a ? 'webapps.detail.noLimits' : 'webapps.detail.limitsUnknown'))]))));
}
