// codesig.js — verified system code (docs/SIGNING.md, "Verified system code").
//
//   codeStatus() → { state: 'verified' | 'development' | 'invalid' | 'unchecked', signed, release,
//                    version, files, ms, reason, refused: [{ path, reason }] }
//   reportRefusedCode()   one notification for each LordOS component that axia-server refused to load
//                         because it no longer matches the signed manifest (called by startShell)
//
// axia-server does the checking (server/src/sec/codesig.c); this only shows the result.
import { notify } from './notify.js';
import { t } from './i18n.js';
import { wm } from './windows.js';

export function codeStatus() {
  try { if (typeof lord.code?.status === 'function') return lord.code.status(); } catch (e) { /* app mode */ }
  return { state: 'unchecked', signed: false, release: false, version: '', files: 0, ms: 0, reason: '', refused: [] };
}

export function reportRefusedCode() {
  if (typeof lord.code?.takeRefused !== 'function') return;
  const list = lord.code.takeRefused();
  if (!list.length) return;
  console.warn(`code: not loaded (modified): ${list.map((r) => `${r.path} (${r.reason})`).join(', ')}`);
  notify({
    app: 'about',
    title: t('shell.code.modifiedTitle'),
    body: t('shell.code.modifiedBody', { list: list.map((r) => r.path).join(', ') }),
    sticky: true,
    actions: [{ id: 'about', label: t('shell.code.details'), run: () => wm.open('about') }],
  });
}
