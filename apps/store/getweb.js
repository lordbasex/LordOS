// getweb.js — Get / Install of a catalog web app (the Web apps section's sheet and a web app's page, 0.8.8.8,
// docs/WEBAPPS.md): lord.webapps.install, and before the first one, when the optional lordos-web package isn't
// installed, LordOS's web engine in the same place: what it takes (the package plan's download and disk size), then the
// Store's own package job (the administrator's password, the progress) and, when it ends well, the app with no other
// click. The steps are webappsmodel.getReducer's. The package side (runJob, Progress, state) is debian.js's, passed in
// as `pkg`, so tests/webapps_get_scene.mjs can render this without the rest of the Store.
import { createElement as h, useEffect, useReducer } from 'react';
import {
  View, Text, Button, useTheme, admin, webAppsAvailable, installWebApp, webAppsModel as M, errorText, formatBytes, typeScale, spacing, t,
} from '@lordos/sdk';

// The package plan for the engine; { ok: false } when the package engine can't make one.
function enginePlan() {
  try { return typeof lord !== 'undefined' && lord.pkg ? lord.pkg.plan({ install: [M.RUNTIME_PACKAGE] }) : { ok: false }; } catch (e) { return { ok: false, error: e.code || null }; }
}

// The steps' state and its actions for app a (a catalog entry).
export function useGetWebApp(a, pkg) {
  const [s, send] = useReducer(M.getReducer, M.GET_IDLE);
  useEffect(() => {
    if (s.step !== 'app') return;
    try { send({ type: 'installed', app: installWebApp(M.catalogEntry(a.id, t('webapps.profile.default'))) }); }
    catch (e) { send({ type: 'error', error: e.code || 'webapps.badEntry' }); }
  }, [s.step]);
  const start = () => {
    const available = webAppsAvailable();
    send({ type: 'start', available, plan: available ? null : enginePlan() });
  };
  const confirm = () => {
    if (s.step !== 'engine' || !s.plan || !s.plan.ok || !admin.available()) return;
    const extra = (s.plan.items || []).some((i) => i.action === 'remove' && !i.requested);
    send({ type: 'confirm' });
    pkg.runJob('pkg-install', extra ? `${M.RUNTIME_PACKAGE} --allow-remove` : M.RUNTIME_PACKAGE, t('webapps.get.engineJob'))
      .then((ok) => send({ type: 'jobDone', ok, available: ok && webAppsAvailable() }));
  };
  return { s, start, confirm, cancel: () => send({ type: 'cancel' }) };
}

// What the steps show, under the app's name: nothing while idle.
export function GetSteps({ a, g, pkg, onOpen }) {
  const th = useTheme();
  const { s } = g;
  if (s.step === 'idle') return null;
  const line = (text, color, extra) => h(Text, { style: { fontSize: typeScale.callout, color: color || th.textDim, lineHeight: 1.4, textAlign: 'start', ...extra } }, text);
  if (s.step === 'engine') {
    const sz = M.engineSizes(s.plan);
    const isAdmin = admin.available();
    const count = s.plan && s.plan.ok ? (s.plan.items || []).filter((i) => i.action === 'install').length : 0;
    return h(View, { style: { gap: spacing.s, paddingTop: spacing.s, borderTop: `1px solid ${th.separator}` } },
      h(Text, { style: { fontSize: typeScale.body, fontWeight: 600, color: th.text, textAlign: 'start' } }, t('webapps.get.engineTitle')),
      line(t('webapps.get.engineBody', { name: a.name })),
      sz ? line(t('webapps.get.engineSizes', { download: formatBytes(sz.download), disk: formatBytes(sz.disk) }), th.text, { fontWeight: 600 }) : null,
      sz && count > 1 ? line(t('webapps.get.enginePackages', { count: count - 1 })) : null,
      !sz ? line((s.plan && s.plan.error && errorText(s.plan.error)) || t('webapps.get.enginePlanFailed'), '#ff453a') : null,
      sz && !isAdmin ? line(t('store.debian.needAdmin'), '#ff9f0a') : null,
      h(View, { style: { flexDirection: 'row', gap: spacing.s, justifyContent: 'flex-end' } },
        h(Button, { label: t('store.cancel'), onClick: g.cancel }),
        sz ? h(Button, { primary: true, label: t('webapps.get.installBoth', { name: a.name }), disabled: !isAdmin, onClick: () => isAdmin && g.confirm() }) : null));
  }
  if (s.step === 'engineJob') {
    return h(View, { style: { gap: spacing.s } }, h(pkg.Progress), line(t('webapps.get.thenApp', { name: a.name })));
  }
  if (s.step === 'app') return line(t('store.working'));
  if (s.step === 'done') {
    return h(View, { style: { flexDirection: 'row', alignItems: 'center', gap: spacing.s } },
      h(Text, { style: { flex: 1, minWidth: 0, fontSize: typeScale.callout, color: '#30d158', lineHeight: 1.4, textAlign: 'start' } }, t('webapps.store.installed', { name: a.name })),
      onOpen ? h(Button, { primary: true, label: t('store.open'), onClick: () => onOpen(s.app) }) : null);
  }
  // failed: the package job wrote its own message (pkg.Progress, with the rescue for a non-administrator)
  return h(View, { style: { gap: spacing.s } },
    s.error ? line(errorText(s.error), '#ff453a') : null,
    !s.error ? h(pkg.Progress) : null,
    h(View, { style: { flexDirection: 'row', gap: spacing.s, justifyContent: 'flex-end' } },
      h(Button, { label: t('store.close'), onClick: g.cancel }),
      h(Button, { label: t('webapps.get.retry'), onClick: g.start })));
}
