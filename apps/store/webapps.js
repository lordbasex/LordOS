// webapps.js — the Store's Web apps section (0.8.8.8, docs/WEBAPPS.md): LordOS's list of web apps
// (assets/webapps/catalog.json, bundled at build time; lord-server installs an entry from its own copy, never from
// what JS says), "Add from an address" (addweb.js) and the installed ones. Without the lordos-web package (lord-web missing) the section says so and
// offers the package through the package engine, like any Debian package (debian.Details).
import { createElement as h, useState } from 'react';
import {
  View, Text, Image, Button, GroupBox, useTheme, useStore, useWebApps, webAppsAvailable, installWebApp, webAppIcon, webAppsModel as M,
  errorText, wm, typeScale, spacing, t,
} from '@lordos/sdk';
import catalog from '../../assets/webapps/catalog.json';
import { debian } from './debian.js';
import { Card, CardGrid, Pill } from './ui.js';
import { AddFromAddress, Origin, box, open } from './addweb.js';

export const webCatalog = () => M.catalogApps(catalog, []);

function CatalogIcon({ a, size }) {
  return h(Image, { src: `asset:webapps/icons/${a.icon}`, style: { width: size, height: size, borderRadius: size * 0.225, objectFit: 'contain' } });
}

// Installing a catalog app: who it is and where it goes, then Install.
function CatalogSheet({ a, onDone }) {
  const th = useTheme();
  const [msg, setMsg] = useState(null);
  const [busy, setBusy] = useState(false);
  const install = () => {
    setBusy(true);
    try { const app = installWebApp(M.catalogEntry(a.id, t('webapps.profile.default'))); setMsg({ ok: true, app }); }
    catch (e) { setMsg({ ok: false, text: errorText(e) }); }
    setBusy(false);
  };
  return h(View, { style: box(th) },
    h(View, { style: { flexDirection: 'row', gap: spacing.m, alignItems: 'center' } },
      h(CatalogIcon, { a, size: 48 }),
      h(View, { style: { flex: 1, minWidth: 0, gap: 2 } },
        h(Text, { style: { fontSize: typeScale.headline, fontWeight: 600, color: th.text, textAlign: 'start' } }, a.name),
        h(Text, { style: { fontSize: typeScale.callout, color: th.textDim, textAlign: 'start' } }, t(a.description)))),
    h(Origin, { origin: a.origin }),
    h(Text, { style: { fontSize: typeScale.callout, color: th.textDim, lineHeight: 1.4, textAlign: 'start' } }, t('webapps.store.whatHappens')),
    !a.tested ? h(Text, { style: { fontSize: typeScale.caption, color: '#ff9f0a', textAlign: 'start' } }, t('webapps.store.untested')) : null,
    msg ? h(Text, { style: { fontSize: typeScale.callout, color: msg.ok ? '#30d158' : '#ff453a', textAlign: 'start' } }, msg.ok ? t('webapps.store.installed', { name: a.name }) : msg.text) : null,
    h(View, { style: { flexDirection: 'row', gap: spacing.s, justifyContent: 'flex-end' } },
      h(Button, { label: t('store.close'), onClick: onDone }),
      msg?.ok ? h(Button, { primary: true, label: t('store.open'), onClick: () => { open(msg.app); onDone(); } })
        : h(Button, { primary: true, label: busy ? t('store.working') : t('store.install'), disabled: busy, onClick: () => !busy && install() })));
}

// Without lord-web: what it is, and the package that brings it.
function Runtime() {
  const th = useTheme();
  useStore(debian.state);
  return h(View, { style: { gap: spacing.l } },
    h(View, { style: box(th) },
      h(Text, { style: { fontSize: typeScale.headline, fontWeight: 600, color: th.text, textAlign: 'start' } }, t('webapps.store.runtimeTitle')),
      h(Text, { style: { fontSize: typeScale.body, color: th.text, lineHeight: 1.45, textAlign: 'start' } }, t('webapps.store.runtimeBody'))),
    h(debian.Details, { name: M.RUNTIME_PACKAGE }));
}

export function WebAppsPage({ query }) {
  const th = useTheme();
  const installed = useWebApps();
  const available = webAppsAvailable();
  const [sheet, setSheet] = useState(null);
  const sec = M.storeSection({ available, installed });
  if (sec.view === 'runtime') return h(Runtime);
  const list = M.catalogApps(catalog, installed, query);
  const mine = installed.filter((e) => !query || e.name.toLowerCase().includes(query.trim().toLowerCase()));
  return h(View, null,
    h(Text, { style: { fontSize: typeScale.body, color: th.textDim, lineHeight: 1.45, maxWidth: 680, marginBottom: spacing.xl, textAlign: 'start' } }, t('webapps.store.subtitle')),
    sheet ? h(CatalogSheet, { a: sheet, onDone: () => setSheet(null) }) : null,
    h(AddFromAddress),
    mine.length ? h(View, { style: { marginBottom: spacing.xl, gap: 6 } },
      h(Text, { style: { fontSize: typeScale.body, fontWeight: 600, color: th.text, textAlign: 'start', paddingHorizontal: spacing.xs } }, t('webapps.store.installedTitle')),
      h(GroupBox, null, ...mine.map((e) => h(View, { key: e.id, style: { flexDirection: 'row', alignItems: 'center', gap: spacing.m, paddingVertical: spacing.s + 2 } },
        h(webAppIcon(e), { size: 32 }),
        h(View, { style: { flex: 1, minWidth: 0, gap: 2 } },
          h(Text, { style: { fontSize: typeScale.body, fontWeight: 600, color: th.text, textAlign: 'start' } }, e.name),
          h(Text, { style: { fontSize: typeScale.caption, color: th.textDim, textAlign: 'start', direction: 'ltr', whiteSpace: 'nowrap', overflow: 'hidden' } }, e.origin)),
        h(Button, { small: true, label: t('webapps.store.manage'), onClick: () => wm.open('settings', { args: { section: 'webapps', app: e.id, nonce: Date.now() } }) }),
        h(Button, { small: true, label: t('store.open'), onClick: () => open(e.id) }))))) : null,
    h(Text, { style: { fontSize: typeScale.body, fontWeight: 600, color: th.text, textAlign: 'start', paddingHorizontal: spacing.xs, marginBottom: 6 } }, t('webapps.store.catalogTitle')),
    h(CardGrid, null, ...list.map((a) => h(Card, {
      key: a.id, icon: h(CatalogIcon, { a, size: 56 }), name: a.name, kind: t(`store.cat.${a.category}`), summary: t(a.description),
      note: a.tested ? '' : t('webapps.store.untestedShort'),
      action: a.installed ? h(Pill, { label: t('store.open'), onClick: () => open(a.id) }) : h(Pill, { label: t('store.get'), onClick: () => setSheet(a) }),
      onClick: () => (a.installed ? open(a.id) : setSheet(a)),
    }))));
}
