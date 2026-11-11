// webapps.js — the Store's Web apps section (0.8.8.8, docs/WEBAPPS.md): LordOS's list of web apps
// (assets/webapps/catalog.json, bundled at build time; lord-server installs an entry from its own copy, never from
// what JS says), "Add from an address" (addweb.js) and the installed ones; each app's page is webdetail.js. Web apps
// also show in Discover (a banner and a shelf), in its search and in their category, always as cards with a "Web app"
// tag. Without the lordos-web package (lord-web missing) the list is still there: the first Get installs the engine
// first, in the same sheet (getweb.js).
import { createElement as h, useState } from 'react';
import {
  View, Text, Image, Button, GroupBox, useTheme, useWebApps, webAppsAvailable, webAppIcon, webAppsModel as M,
  wm, typeScale, spacing, t,
} from '@lordos/sdk';
import catalog from '../../assets/webapps/catalog.json';
import { debian } from './debian.js';
import { Card, CardGrid, Featured, Pill, Shelf } from './ui.js';
import { AddFromAddress, Origin, box, open } from './addweb.js';
import { GetSteps, useGetWebApp } from './getweb.js';

export const webCatalog = () => M.catalogApps(catalog, []);
export const webCatalogData = catalog;

export function CatalogIcon({ a, size }) {
  return h(Image, { src: `asset:webapps/icons/${a.icon}`, style: { width: size, height: size, borderRadius: size * 0.225, objectFit: 'contain' } });
}

// A web app's card, wherever the Store lists apps: its category after the "Web app" tag. The card opens its page;
// the pill opens the app once it is installed, else Get (onGet, or the page).
export function WebCard({ a, onOpen, onGet }) {
  return h(Card, {
    icon: h(CatalogIcon, { a, size: 56 }), name: a.name, tag: t('store.kind.web'), kind: t(`store.cat.${a.category}`), summary: t(a.description),
    note: a.tested ? '' : t('webapps.store.untestedShort'),
    action: a.installed ? h(Pill, { label: t('store.open'), onClick: () => open(a.id) }) : h(Pill, { label: t('store.get'), onClick: () => (onGet ? onGet(a) : onOpen(a.id)) }),
    onClick: () => onOpen(a.id),
  });
}

// Installing a catalog app: who it is and where it goes, then Install (and the engine first, the first time).
function CatalogSheet({ a, onDone, onMore }) {
  const th = useTheme();
  const g = useGetWebApp(a, debian);
  const idle = g.s.step === 'idle';
  return h(View, { style: box(th) },
    h(View, { style: { flexDirection: 'row', gap: spacing.m, alignItems: 'center' } },
      h(CatalogIcon, { a, size: 48 }),
      h(View, { style: { flex: 1, minWidth: 0, gap: 2 } },
        h(Text, { style: { fontSize: typeScale.headline, fontWeight: 600, color: th.text, textAlign: 'start' } }, a.name),
        h(Text, { style: { fontSize: typeScale.callout, color: th.textDim, textAlign: 'start' } }, t(a.description)))),
    h(Origin, { origin: a.origin }),
    h(Text, { style: { fontSize: typeScale.callout, color: th.textDim, lineHeight: 1.4, textAlign: 'start' } }, t('webapps.store.whatHappens')),
    !a.tested ? h(Text, { style: { fontSize: typeScale.caption, color: '#ff9f0a', textAlign: 'start' } }, t('webapps.store.untested')) : null,
    h(GetSteps, { a, g, pkg: debian, onOpen: (app) => { open(app); onDone(); } }),
    idle || g.s.step === 'done' ? h(View, { style: { flexDirection: 'row', gap: spacing.s, justifyContent: 'flex-end' } },
      h(Button, { label: t('store.close'), onClick: onDone }),
      h(Button, { label: t('webapps.store.seeMore'), onClick: () => onMore(a.id) }),
      idle ? h(Button, { primary: true, label: t('store.install'), onClick: g.start }) : null) : null);
}

// Discover: LordOS's web apps in a banner (the first apps' icons, "See all" opens the section), then a shelf of cards.
function Mosaic({ apps }) {
  return h(View, { style: { width: 96, height: 96, flexDirection: 'row', flexWrap: 'wrap', gap: 8, alignItems: 'center', justifyContent: 'center' } },
    ...apps.slice(0, 4).map((a) => h(CatalogIcon, { key: a.id, a, size: 44 })));
}

export function WebDiscover({ onOpen, onMore }) {
  const installed = useWebApps();
  const all = M.catalogApps(catalog, installed);
  if (!all.length) return null;
  const top = M.featuredApps(catalog, installed, 4);
  const more = t('store.seeAll', { count: all.length });
  return h(View, { style: { gap: 26 } },
    h(Featured, { icon: h(Mosaic, { apps: top }), glow: top[0].themeColor || '#2aabee', colors: ['#10233a', '#1d4f7a'],
      kicker: t('webapps.discover.kicker'), title: t('webapps.discover.title'), text: t('webapps.discover.text', { names: top.map((a) => a.name).join(', ') }),
      action: h(Pill, { big: true, onDark: true, label: more, onClick: onMore }), onClick: onMore }),
    h(Shelf, { title: t('store.page.webapps'), more, onMore },
      h(CardGrid, null, ...top.map((a) => h(WebCard, { key: a.id, a, onOpen })))));
}

// Discover's search: the web apps that match, as their own shelf.
export function WebResults({ query, onOpen }) {
  const installed = useWebApps();
  const list = M.catalogApps(catalog, installed, query);
  if (!list.length) return null;
  return h(Shelf, { title: t('webapps.discover.results') }, h(CardGrid, null, ...list.map((a) => h(WebCard, { key: a.id, a, onOpen }))));
}
export const webHits = (query) => M.catalogApps(catalog, [], query).length;

// A category page: its web apps above its packages.
export function WebCategory({ cat, query, onOpen }) {
  const installed = useWebApps();
  const list = M.categoryApps(catalog, installed, cat).filter((a) => !query || M.catalogApps({ apps: [a] }, [], query).length);
  if (!list.length) return null;
  return h(Shelf, { title: t('store.page.webapps') }, h(CardGrid, null, ...list.map((a) => h(WebCard, { key: a.id, a, onOpen }))));
}

export function WebAppsPage({ query, onOpen }) {
  const th = useTheme();
  const installed = useWebApps();
  const available = webAppsAvailable();
  const [sheet, setSheet] = useState(null);
  const sec = M.storeSection({ available, installed });
  const list = M.catalogApps(catalog, installed, query);
  const mine = installed.filter((e) => !query || e.name.toLowerCase().includes(query.trim().toLowerCase()));
  return h(View, null,
    h(Text, { style: { fontSize: typeScale.body, color: th.textDim, lineHeight: 1.45, maxWidth: 680, marginBottom: spacing.xl, textAlign: 'start' } }, t('webapps.store.subtitle')),
    sec.needsEngine ? h(View, { style: box(th) },
      h(Text, { style: { fontSize: typeScale.headline, fontWeight: 600, color: th.text, textAlign: 'start' } }, t('webapps.store.engineTitle')),
      h(Text, { style: { fontSize: typeScale.callout, color: th.textDim, lineHeight: 1.45, textAlign: 'start' } }, t('webapps.store.engineBody'))) : null,
    sheet ? h(CatalogSheet, { key: sheet.id, a: sheet, onDone: () => setSheet(null), onMore: onOpen }) : null,
    sec.canAdd ? h(AddFromAddress) : null,
    mine.length ? h(View, { style: { marginBottom: spacing.xl, gap: 6 } },
      h(Text, { style: { fontSize: typeScale.body, fontWeight: 600, color: th.text, textAlign: 'start', paddingHorizontal: spacing.xs } }, t('webapps.store.installedTitle')),
      h(GroupBox, null, ...mine.map((e) => h(View, { key: e.id, style: { flexDirection: 'row', alignItems: 'center', gap: spacing.m, paddingVertical: spacing.s + 2 } },
        h(webAppIcon(e), { size: 32 }),
        h(View, { style: { flex: 1, minWidth: 0, gap: 2 } },
          h(Text, { style: { fontSize: typeScale.body, fontWeight: 600, color: th.text, textAlign: 'start' } }, e.name),
          h(Text, { style: { fontSize: typeScale.caption, color: th.textDim, textAlign: 'start', direction: 'ltr', whiteSpace: 'nowrap', overflow: 'hidden' } }, e.origin)),
        h(Button, { small: true, label: t('webapps.store.seeMore'), onClick: () => onOpen(e.id) }),
        h(Button, { small: true, label: t('webapps.store.manage'), onClick: () => wm.open('settings', { args: { section: 'webapps', app: e.id, nonce: Date.now() } }) }),
        h(Button, { small: true, label: t('store.open'), onClick: () => open(e.id) }))))) : null,
    h(Text, { style: { fontSize: typeScale.body, fontWeight: 600, color: th.text, textAlign: 'start', paddingHorizontal: spacing.xs, marginBottom: 6 } }, t('webapps.store.catalogTitle')),
    h(CardGrid, null, ...list.map((a) => h(WebCard, { key: a.id, a, onOpen, onGet: setSheet }))));
}
