// suggested.js — Store → Discover, "Suggested for your computer": packages for the detected
// hardware (sdk/suggestions.js, assets/store/suggestions.json, docs/PACKAGES.md). Each card says
// why it appears, installs through the usual plan + pkg-install, and can be dismissed. Software
// that isn't free says so before anything is installed, and is never preselected.
import { createElement as h, useEffect, useState } from 'react';
import {
  View, Text, Button, EmptyState, useTheme, useStore, admin, typeScale, spacing, t, direction,
  suggestions, refreshSuggestions, reevaluateSuggestions, dismissSuggestion, markSuggestionsSeen,
  fanCase, session,
} from '@lordos/sdk';

// ---- drawn glyphs (monochrome on a tinted tile) ----

const TINT = {
  vm: ['#5e8cff', '#3757d8'], gpu: ['#6fcf5b', '#2f9a3b'], board: ['#e2557a', '#b12c55'], printer: ['#8e8e93', '#5f5f66'],
  scanner: ['#8e8e93', '#5f5f66'], wifi: ['#36a3ff', '#1463d6'], bluetooth: ['#4f7dff', '#2448c9'], network: ['#2fb5a5', '#16867a'],
  graphics: ['#6fcf5b', '#2f9a3b'], camera: ['#ff9f43', '#e0662a'], other: ['#a07cff', '#6a45d6'], package: ['#a07cff', '#6a45d6'],
  disk: ['#8e9bb0', '#55627a'], web: ['#5e5ce6', '#3a38b8'], voice: ['#bf5af2', '#8a2fc0'],
};

// (also Apps LordOS's cards, apps/store/lordospages.js: camera, disk, web, voice)
export function Glyph({ kind, size: s = 40 }) {
  const [a, b] = TINT[kind] || TINT.package;
  const w = '#fff';
  const box = (style) => h(View, { style: { position: 'absolute', ...style } });
  let parts;
  switch (kind) {
    case 'vm':                          // a display with a stand
      parts = [box({ left: 9, top: 10, width: 22, height: 15, borderRadius: 2, border: `2px solid ${w}` }),
        box({ left: 18, top: 26, width: 4, height: 3, backgroundColor: w }), box({ left: 13, top: 29, width: 14, height: 2, borderRadius: 1, backgroundColor: w })];
      break;
    case 'gpu': case 'graphics': case 'board':   // a chip with pins
      parts = [box({ left: 12, top: 12, width: 16, height: 16, borderRadius: 3, border: `2px solid ${w}` }), box({ left: 17, top: 17, width: 6, height: 6, borderRadius: 1, backgroundColor: w }),
        ...[14, 19, 24].flatMap((p) => [box({ left: p, top: 8, width: 2, height: 3, backgroundColor: w }), box({ left: p, top: 29, width: 2, height: 3, backgroundColor: w }),
          box({ left: 8, top: p, width: 3, height: 2, backgroundColor: w }), box({ left: 29, top: p, width: 3, height: 2, backgroundColor: w })])];
      break;
    case 'printer':
      parts = [box({ left: 14, top: 8, width: 12, height: 8, border: `2px solid ${w}`, borderRadius: 1 }), box({ left: 8, top: 15, width: 24, height: 11, borderRadius: 3, backgroundColor: w }),
        box({ left: 13, top: 23, width: 14, height: 9, borderRadius: 1, border: `2px solid ${w}`, backgroundColor: b })];
      break;
    case 'scanner':
      parts = [box({ left: 8, top: 17, width: 24, height: 11, borderRadius: 3, backgroundColor: w }), box({ left: 9, top: 13, width: 22, height: 3, borderRadius: 1.5, backgroundColor: w }),
        box({ left: 11, top: 21, width: 18, height: 2, backgroundColor: b })];
      break;
    case 'wifi':                        // a dot and two rings (top halves)
      parts = [h(View, { key: 'r', style: { position: 'absolute', left: 6, top: 9, width: 28, height: 14, overflow: 'hidden' } },
        box({ left: 0, top: 0, width: 28, height: 28, borderRadius: 14, border: `2.5px solid ${w}` }),
        box({ left: 6, top: 6, width: 16, height: 16, borderRadius: 8, border: `2.5px solid ${w}` })),
      box({ left: 17, top: 23, width: 6, height: 6, borderRadius: 3, backgroundColor: w })];
      break;
    case 'bluetooth':
      parts = [h(Text, { key: 'b', style: { position: 'absolute', left: 0, top: 6, width: s, textAlign: 'center', fontSize: 22, fontWeight: 700, color: w, lineHeight: 28 } }, 'B')];
      break;
    case 'camera':                      // a body and a lens hood (the capture indicator's camera)
      parts = [box({ left: 7, top: 13, width: 18, height: 14, borderRadius: 3, backgroundColor: w }),
        box({ left: 26, top: 16, width: 7, height: 8, borderTopRightRadius: 1, borderBottomRightRadius: 1, backgroundColor: w })];
      break;
    case 'disk':                        // a drive: its case, a slot and the light
      parts = [box({ left: 8, top: 12, width: 24, height: 16, borderRadius: 3, border: `2px solid ${w}` }),
        box({ left: 12, top: 19, width: 10, height: 2, backgroundColor: w }), box({ left: 25, top: 18, width: 4, height: 4, borderRadius: 2, backgroundColor: w })];
      break;
    case 'web':                         // a globe: the circle, its equator and a meridian
      parts = [box({ left: 9, top: 9, width: 22, height: 22, borderRadius: 11, border: `2px solid ${w}` }),
        box({ left: 10, top: 19, width: 20, height: 2, backgroundColor: w }), box({ left: 15, top: 10, width: 10, height: 20, borderRadius: 5, border: `2px solid ${w}` })];
      break;
    case 'voice':                       // a sound wave: five bars
      parts = [[9, 6], [14, 12], [19, 18], [24, 12], [29, 6]].map(([x, hh]) => box({ left: x, top: 20 - hh / 2, width: 3, height: hh, borderRadius: 1.5, backgroundColor: w }));
      break;
    case 'network':
      parts = [box({ left: 8, top: 14, width: 24, height: 12, borderRadius: 2, border: `2px solid ${w}` }),
        ...[12, 17, 22, 27].map((x) => box({ left: x - 1, top: 18, width: 3, height: 4, backgroundColor: w }))];
      break;
    default:                            // a box
      parts = [box({ left: 10, top: 12, width: 20, height: 18, borderRadius: 3, border: `2px solid ${w}` }), box({ left: 10, top: 18, width: 20, height: 2, backgroundColor: w })];
  }
  const keyed = parts.map((p, i) => (p.key ? p : { ...p, key: i }));
  // the parts are drawn on a 40 px tile; another size scales them
  return h(View, { style: { width: s, height: s, borderRadius: s * 0.24, background: `linear-gradient(180deg, ${a}, ${b})`, flexShrink: 0, direction: 'ltr' } },
    ...(s === 40 ? keyed : [h(View, { key: 'k', style: { width: 40, height: 40, transform: `scale(${s / 40})`, transformOrigin: 'left top' } }, ...keyed)]));
}

// ---- Secure Boot: enrolling DKMS's signing key (dkms-mok-enroll, server/src/hw/mok_admin.h) ----
// For an installed card whose package DKMS builds (the `secureBoot` note): asks the helper whether the key still has to
// be enrolled; if so, "Prepare the Key" makes a one-time password root-side and gives it once. It lives only in this
// component's state (no store, no log, no clipboard) and is gone when the card closes; a lost one is replaced by a new
// request ("New Password"), which withdraws the old one.

function askMok(set) {
  admin.run('dkms-mok-enroll', 'status', null, { quiet: true })
    .then((line) => { const m = /state=([a-z]+)/.exec(String(line)); set(m ? m[1] : 'unsupported'); }, () => set('unsupported'));
}

function MokStep({ th }) {
  const [state, setState] = useState(null);
  const [password, setPassword] = useState('');
  const [busy, setBusy] = useState(false);
  const [error, setError] = useState('');
  useEffect(() => { askMok(setState); }, []);
  if (state !== 'needed' && state !== 'pending') return null;
  const run = (arg) => {
    setBusy(true);
    setError('');
    admin.run('dkms-mok-enroll', arg, null, { quiet: true }).then((line) => {
      const m = /password=([0-9]{8})$/.exec(String(line));
      setPassword(m ? m[1] : '');
      askMok(setState);
    }, (e) => { setPassword(''); setError(e.message); askMok(setState); }).finally(() => setBusy(false));
  };
  const canRestart = typeof session.canPower === 'function' && session.canPower();
  const text = (key, extra = {}) => h(Text, { style: { fontSize: typeScale.callout, color: th.textDim, lineHeight: 1.4, textAlign: 'start', ...extra } }, t(key));
  const shown = state === 'pending' && password;
  return h(View, { style: { gap: spacing.s, marginTop: spacing.xs, paddingTop: spacing.s, borderTop: `1px solid ${th.separator}` } },
    h(Text, { style: { fontSize: typeScale.body, fontWeight: 600, color: th.text, textAlign: 'start' } }, t('store.suggest.mok.title')),
    shown ? null : text(state === 'pending' ? 'store.suggest.mok.pending' : 'store.suggest.mok.body'),
    shown ? h(View, { style: { gap: spacing.xs } },
      text('store.suggest.mok.passwordTitle', { color: th.text, fontWeight: 500 }),
      h(Text, { style: { fontSize: 34, fontWeight: 300, color: th.text, fontFamily: 'monospace', direction: 'ltr', textAlign: 'start', letterSpacing: 4 } }, password),
      text('store.suggest.mok.passwordHint'),
      text('store.suggest.mok.steps', { color: th.text })) : null,
    error ? h(Text, { style: { fontSize: typeScale.caption, color: '#ff9f0a', textAlign: 'start' } }, error) : null,
    shown && !canRestart ? text('store.restart.how') : null,
    h(View, { style: { flexDirection: 'row', gap: spacing.s, justifyContent: 'flex-end', flexWrap: 'wrap' } },
      state === 'pending' ? h(Button, { label: t('store.suggest.mok.cancel'), disabled: busy, onClick: () => run('cancel') }) : null,
      state === 'pending' ? h(Button, { label: t('store.suggest.mok.newPassword'), disabled: busy, onClick: () => run('import') }) : null,
      state === 'needed' ? h(Button, { primary: true, label: t('store.suggest.mok.prepare'), disabled: busy, onClick: () => run('import') }) : null,
      shown && canRestart ? h(Button, { primary: true, label: t('store.restart.now'), onClick: () => session.restart() }) : null));
}

// ---- one card ----

const loc = (s, part, extra = {}) => t(`store.suggest.${s.text}.${part}`, { ...s.params, ...extra });

function Notes({ s, th }) {
  const lines = [
    !s.free && t('store.suggest.note.nonfree', { component: s.component || 'non-free' }),
    ...s.notes.filter((n) => n !== 'nonfree' && n !== 'firmware').map((n) => t(`store.suggest.${s.text}.note.${n}`)),
    s.notes.includes('firmware') && t('store.suggest.note.firmware'),
  ].filter(Boolean);
  if (!lines.length) return null;
  return h(View, { style: { gap: 4, marginTop: 2 } },
    ...lines.map((l, i) => h(Text, { key: i, style: { fontSize: typeScale.caption, color: th.textDim, lineHeight: 1.4, textAlign: 'start' } }, `• ${l}`)));
}

function Card({ s, deb }) {
  const th = useTheme();
  const { job } = useStore(deb.state);
  const [plan, setPlan] = useState(null);
  const [more, setMore] = useState(false);
  const [error, setError] = useState('');
  const isAdmin = admin.available();
  const nonfree = !s.free;
  const ask = () => setPlan(lord.pkg.plan({ install: s.install }));
  const confirm = () => {
    const extra = plan.items.some((i) => i.action === 'remove' && !i.requested);
    deb.runJob('pkg-install', `${s.install.join(' ')}${extra ? ' --allow-remove' : ''}`, t('store.suggest.job', { name: loc(s, 'title') }));
    setPlan(null);
  };
  const detailKey = s.state === 'unavailable' ? 'unavailable' : 'detail';
  const badge = s.state === 'installed'
    ? h(Text, { style: { fontSize: typeScale.caption, fontWeight: 500, color: '#30d158' } }, t('store.installedBadge'))
    : null;
  return h(View, { style: deb.boxStyle(th, { gap: spacing.s, padding: spacing.l }) },
    h(View, { style: { flexDirection: 'row', alignItems: 'flex-start', gap: spacing.m } },
      h(Glyph, { kind: s.icon }),
      h(View, { style: { flex: 1, minWidth: 0, gap: 3 } },
        h(View, { style: { flexDirection: 'row', alignItems: 'center', gap: spacing.s, flexWrap: 'wrap' } },
          h(Text, { style: { fontSize: typeScale.body, fontWeight: 600, color: th.text, textAlign: 'start' } }, loc(s, 'title')),
          nonfree ? h(View, { style: { paddingHorizontal: 6, height: 18, borderRadius: 9, justifyContent: 'center', backgroundColor: 'rgba(255,159,10,0.16)' } },
            h(Text, { style: { fontSize: 10.5, fontWeight: 600, color: '#ff9f0a' } }, t('store.suggest.nonfreeBadge'))) : null,
          badge),
        h(Text, { style: { fontSize: typeScale.callout, color: th.text, lineHeight: 1.4, textAlign: 'start' } }, loc(s, 'why')),
        h(Text, { style: { fontSize: typeScale.callout, color: th.textDim, lineHeight: 1.4, textAlign: 'start' } },
          loc(s, detailKey, { packages: (s.state === 'unavailable' ? s.missing : s.packages).join(', '), component: s.component || 'non-free' })),
        (nonfree || s.notes.length) && s.state !== 'installed'
          ? (more ? h(Notes, { s, th }) : h(Button, { plain: true, small: true, label: t('store.suggest.whatChanges'), onClick: () => setMore(true), style: { alignSelf: 'flex-start', [direction() === 'rtl' ? 'marginRight' : 'marginLeft']: -6 } }))
          : null),
      h(View, { style: { alignItems: 'flex-end', gap: 2 } },
        s.state === 'install' ? h(Button, { primary: !nonfree, label: nonfree ? t('store.suggest.installNonfree') : t('store.install'), disabled: !isAdmin || !!job,
          onClick: () => isAdmin && !job && ask() }) : null,
        s.state === 'setup' ? h(Button, { primary: true, label: t('store.suggest.keepOn'), disabled: !isAdmin, onClick: () => admin.run('rpi-case', 'argon-one-v5').then(() => { fanCase.set(() => 'argon-one-v5'); refreshSuggestions(); }, (e) => setError(e.message)) }) : null,
        s.state !== 'installed' ? h(Button, { plain: true, small: true, label: t('store.suggest.dismiss'), onClick: () => dismissSuggestion(s.id) }) : null)),
    error ? h(Text, { style: { fontSize: typeScale.caption, color: '#ff9f0a', textAlign: 'start' } }, error) : null,
    !isAdmin && s.state === 'install' ? h(Text, { style: { fontSize: typeScale.caption, color: '#ff9f0a', textAlign: 'start' } }, t('store.debian.needAdmin')) : null,
    isAdmin && s.state === 'installed' && s.notes.includes('secureBoot') ? h(MokStep, { th }) : null,
    plan && h(View, { style: { gap: spacing.s, marginTop: spacing.xs, paddingTop: spacing.s, borderTop: `1px solid ${th.separator}` } },
      nonfree ? h(Text, { style: { fontSize: typeScale.caption, color: '#ff9f0a', lineHeight: 1.4, textAlign: 'start' } }, t('store.suggest.confirmNonfree')) : null,
      h(deb.PlanView, { plan }),
      h(View, { style: { flexDirection: 'row', gap: spacing.s, justifyContent: 'flex-end' } },
        h(Button, { label: t('store.cancel'), onClick: () => setPlan(null) }),
        plan.ok && h(Button, { primary: true, label: t('store.plan.confirm'), onClick: confirm }))));
}

// ---- the section ----

// `page`: the Store's own page for it (0.8.5): no heading (the toolbar names it) and an empty state.
export function Suggested({ deb, page }) {
  const th = useTheme();
  const { list, fixture, ready } = useStore(suggestions);
  const { gen } = useStore(deb.state);
  useEffect(() => { if (!suggestions.get().ready) refreshSuggestions(); }, []);
  useEffect(() => { if (ready) markSuggestionsSeen(list); }, [ready, list]);
  useEffect(() => { if (gen) reevaluateSuggestions(); }, [gen]);     // installed or removed something
  if (page && ready && !list.length) return h(EmptyState, { mascot: 'celebrate', title: t('store.suggest.none'), hint: t('store.suggest.subtitle'), style: { minHeight: 320 } });
  if (!ready || !list.length) return null;
  return h(View, { style: { gap: spacing.s, marginBottom: spacing.s } },
    h(View, { style: { flexDirection: 'row', alignItems: 'baseline', gap: spacing.s, flexWrap: 'wrap' } },
      page ? null : h(Text, { style: { fontSize: typeScale.title, fontWeight: 600, color: th.text, textAlign: 'start' } }, t('store.suggest.title')),
      fixture ? h(View, { style: { paddingHorizontal: 8, height: 20, borderRadius: 10, justifyContent: 'center', backgroundColor: '#ff453a' } },
        h(Text, { style: { fontSize: 11, fontWeight: 700, color: '#fff' } }, t('store.suggest.fixture'))) : null),
    h(Text, { style: { fontSize: typeScale.callout, color: th.textDim, lineHeight: 1.4, textAlign: 'start', marginBottom: 2 } }, t('store.suggest.subtitle')),
    ...list.map((s) => h(Card, { key: s.id, s, deb })));
}
