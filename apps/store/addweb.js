// addweb.js — the Store's "Add from an address" sheet (0.8.8.8, docs/WEBAPPS.md): the address the person types is
// probed by lord-web --probe; the sheet shows the real origin, the name, the icon and whether the page has a web app
// manifest, and nothing is installed before Install. What it sends names the probe, never a URL: lord-server installs
// what lord-web read. Its own file, so tests/webapps_sheet_scene.mjs can render it without the rest of the Store.
import { createElement as h, useReducer } from 'react';
import {
  View, Text, Image, Button, TextInput, LetterIcon, useTheme, probeWebApp, installWebApp, launchWebApp, webAppsModel as M,
  errorText, notify, typeScale, spacing, t,
} from '@lordos/sdk';

export const box = (th) => ({ padding: spacing.xl, borderRadius: 12, backgroundColor: th.mode === 'dark' ? 'rgba(255,255,255,0.05)' : '#ffffff',
  border: `1px solid ${th.separator}`, boxShadow: '0 6px 20px rgba(0,0,0,0.18)', gap: spacing.m, marginBottom: spacing.xl });
export const errText = (code) => (code && code.startsWith('address.') ? t(`webapps.store.${code}`) : errorText(code || 'webapps.probeFailed'));

// The origin, large and left to right: what the person checks before installing.
export function Origin({ origin }) {
  const th = useTheme();
  return h(View, { style: { gap: 2 } },
    h(Text, { style: { fontSize: typeScale.caption, color: th.textDim, textAlign: 'start' } }, t('webapps.store.origin')),
    h(Text, { style: { fontSize: typeScale.body, fontWeight: 600, color: th.text, direction: 'ltr', textAlign: 'start', whiteSpace: 'nowrap', overflow: 'hidden' } }, origin));
}

export const open = (app) => { try { launchWebApp(app); } catch (e) { notify({ app: 'store', title: errorText(e) }); } };

// "Add from an address": type, check (the probe), see what it really is, install.
export function AddFromAddress() {
  const th = useTheme();
  const [s, send] = useReducer(M.addReducer, M.ADD_IDLE);
  const probe = () => {
    const next = M.addReducer(s, { type: 'probe' });
    send({ type: 'probe' });
    if (next.step !== 'probing') return;
    probeWebApp(next.url, (id) => send({ type: 'started', id })).then((r) => send({ type: 'result', id: r.id, result: r }));
  };
  const install = () => {
    if (!M.nameOk(s.name)) return;
    send({ type: 'install' });
    try { send({ type: 'installed', app: installWebApp(M.addressEntry(s, t('webapps.profile.default'))) }); }
    catch (e) { send({ type: 'error', error: e.code || 'webapps.badEntry' }); }
  };
  const r = s.result;
  const busy = s.step === 'probing' || s.step === 'installing';
  return h(View, { style: { gap: spacing.s, marginBottom: spacing.xl } },
    h(Text, { style: { fontSize: typeScale.body, fontWeight: 600, color: th.text, textAlign: 'start', paddingHorizontal: spacing.xs } }, t('webapps.store.addTitle')),
    h(View, { style: { flexDirection: 'row', gap: spacing.s, alignItems: 'center' } },
      h(View, { style: { flex: 1, minWidth: 0, direction: 'ltr' } },
        h(TextInput, { value: s.address, onChange: (text) => send({ type: 'type', text }), onSubmit: probe, placeholder: t('webapps.store.addPlaceholder') })),
      h(Button, { label: s.step === 'probing' ? t('webapps.store.checking') : t('webapps.store.check'), disabled: busy || !s.address.trim(), onClick: () => !busy && probe() })),
    h(Text, { style: { fontSize: typeScale.caption, color: th.textDim, lineHeight: 1.4, textAlign: 'start', paddingHorizontal: spacing.xs } }, t('webapps.store.addHint')),
    s.step === 'failed' ? h(Text, { style: { fontSize: typeScale.callout, color: '#ff453a', textAlign: 'start', paddingHorizontal: spacing.xs } }, errText(s.error)) : null,
    (s.step === 'found' || s.step === 'installing' || s.step === 'installed') && r ? h(View, { style: box(th) },
      h(View, { style: { flexDirection: 'row', gap: spacing.m, alignItems: 'center' } },
        r.iconKind === 'png' && r.iconPath ? h(Image, { src: r.iconPath, style: { width: 48, height: 48, objectFit: 'contain' } }) : h(LetterIcon, { size: 48, name: s.name || r.name }),
        h(View, { style: { flex: 1, minWidth: 0, gap: spacing.xs } },
          h(Text, { style: { fontSize: typeScale.caption, color: th.textDim, textAlign: 'start' } }, t('webapps.store.name')),
          s.step === 'found' ? h(TextInput, { value: s.name || '', onChange: (name) => send({ type: 'rename', name }), onSubmit: install })
            : h(Text, { style: { fontSize: typeScale.body, fontWeight: 600, color: th.text, textAlign: 'start' } }, s.name))),
      h(Origin, { origin: r.origin }),
      h(Text, { style: { fontSize: typeScale.callout, color: th.textDim, textAlign: 'start' } }, t(r.manifest ? 'webapps.store.manifestYes' : 'webapps.store.manifestNo')),
      h(Text, { style: { fontSize: typeScale.callout, color: th.textDim, lineHeight: 1.4, textAlign: 'start' } }, t('webapps.store.whatHappens')),
      s.step === 'found' && !M.nameOk(s.name) ? h(Text, { style: { fontSize: typeScale.caption, color: '#ff453a', textAlign: 'start' } }, t('webapps.store.badName')) : null,
      s.step === 'installed' ? h(Text, { style: { fontSize: typeScale.callout, color: '#30d158', textAlign: 'start' } }, t('webapps.store.installed', { name: s.name })) : null,
      h(View, { style: { flexDirection: 'row', gap: spacing.s, justifyContent: 'flex-end' } },
        h(Button, { label: s.step === 'installed' ? t('store.close') : t('store.cancel'), onClick: () => send({ type: 'reset' }) }),
        s.step === 'installed' ? h(Button, { primary: true, label: t('store.open'), onClick: () => { open(s.app); send({ type: 'reset' }); } })
          : h(Button, { primary: true, label: s.step === 'installing' ? t('store.working') : t('store.install'), disabled: busy || !M.nameOk(s.name), onClick: install }))) : null);
}

