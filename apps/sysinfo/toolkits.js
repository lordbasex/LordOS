// toolkits.js — the toolkit audit in the hardware report (docs/TOOLKIT-AUDIT.md, docs/HARDWARE-REPORT.md): the
// saved result of `lordos-testbench --toolkits` (~/.cache/lordos/toolkit-audit.json) turned into the report's
// optional "toolkits" section, how fresh it is, and the labels and summary the workflow and the site use. Pure
// functions with no UI and no SDK, tested by server/tests/hwreport_test.mjs.
//
// The saved file is the person's own and may have been edited: nothing is copied from it as it is. The section is
// built from known ids and numbers here, then share.js pick()s it through the schema (hwreport.schema.json,
// $defs/toolkits), which drops any value outside its lists. The audit's JSON never holds free text, paths or
// titles; the section keeps that.

export const TOOLKIT_COMBOS = ['gtk3-wayland', 'gtk3-x11', 'gtk4-wayland', 'gtk4-x11', 'qt5-wayland', 'qt5-xcb', 'qt6-wayland', 'qt6-xcb', 'x11-xwayland'];
export const SHARED_CHECKS = ['map', 'size', 'backend', 'first_frame', 'decoration', 'menu', 'combo', 'clipboard', 'dnd', 'scale', 'theme', 'tooltip', 'text', 'fullscreen'];
// A saved result older than this is run again before it is shared ("uses the saved result if it's under 1 day old").
export const FRESH_MS = 24 * 3600 * 1000;
// A combination "fails" (the workflow's toolkit:<id>-fail label, the site's mark) when it ran and scored under this,
// or ended at 0 % (crash, timeout, fatal error).
export const FAIL_BELOW = 80;

const STATUS = ['ran', 'fatal', 'not-installed', 'no-display', 'no-harness', 'not-run'];
const FATAL = ['timeout', 'crash', 'protocol-error', 'connection-lost', 'egl', 'vulkan', 'platform-plugin', 'no-result'];
const RECOMMEND = ['GDK_BACKEND=x11', 'QT_QPA_PLATFORM=xcb'];
const CHECK_RE = /^(?:[a-z_]{1,16}|global\.[a-z][a-z0-9_]{1,63})$/;   // the schema then keeps only the known ids

const num = (v, lo, hi) => (typeof v === 'number' && Number.isFinite(v) && v >= lo && v <= hi ? v : undefined);
const int = (v, lo, hi) => (Number.isInteger(v) && v >= lo && v <= hi ? v : undefined);
const pct = (v) => (v === null ? null : num(v, 0, 100));

// The saved audit (already parsed), or null when it isn't one.
export function isAudit(a) {
  return !!a && typeof a === 'object' && !Array.isArray(a) && a.tool === 'lordos-testbench' && a.mode === 'toolkits' && a.v === 1
    && Number.isFinite(a.at) && a.at > 0 && Array.isArray(a.combos);
}

// Fresh enough to share without running it again: made within FRESH_MS before now (and not in the future).
export function isFresh(a, now = Date.now()) {
  return isAudit(a) && a.at <= now + 60000 && now - a.at < FRESH_MS;
}

// The report's section from a saved audit, or null. Unknown combinations are left out; values outside the lists
// become undefined and the schema's pick drops them.
export function toolkitSection(a) {
  if (!isAudit(a)) return null;
  const seen = new Set();
  const combos = [];
  for (const c of a.combos.slice(0, TOOLKIT_COMBOS.length)) {
    if (!c || typeof c !== 'object' || !TOOLKIT_COMBOS.includes(c.id) || seen.has(c.id)) continue;
    seen.add(c.id);
    const checks = Array.isArray(c.checks) ? c.checks.slice(0, 80).filter((k) => k && typeof k.id === 'string' && CHECK_RE.test(k.id)) : [];
    const p = c.protocol && typeof c.protocol === 'object' ? c.protocol : null;
    combos.push({
      id: c.id,
      status: STATUS.includes(c.status) ? c.status : 'not-run',
      fatal: FATAL.includes(c.fatal) ? c.fatal : null,
      version: typeof c.version === 'string' && /^[0-9A-Za-z.+~-]{0,31}$/.test(c.version) ? c.version : '',
      backendSeen: ['', 'wayland', 'x11'].includes(c.backendSeen) ? c.backendSeen : '',
      score: pct(c.score === undefined ? null : c.score),
      points: int(c.points, 0, 1000), total: int(c.total, 0, 1000), skipped: int(c.skipped, 0, 1000), ms: int(c.ms, 0, 600000),
      failed: checks.filter((k) => k.result === 'fail').map((k) => k.id),
      skippedChecks: checks.filter((k) => k.result === 'skip' && SHARED_CHECKS.includes(k.id)).map((k) => k.id),
      protocol: p ? { bound: int(p.bound, 0, 10000), failed: int(p.failed, 0, 10000), extra: int(p.extra, 0, 10000), errors: int(p.errors, 0, 10000), unattributed: int(p.unattributed, 0, 10000) } : undefined,
      recommend: RECOMMEND.includes(c.recommend) ? c.recommend : null,
    });
  }
  return {
    v: 1,
    at: new Date(a.at).toISOString().slice(0, 10),
    score: pct(a.score === undefined ? null : a.score),
    ran: int(a.ran, 0, 9),
    combos,
  };
}

// The combinations that failed (see FAIL_BELOW), in table order.
export function failedCombos(section) {
  if (!section || !Array.isArray(section.combos)) return [];
  return TOOLKIT_COMBOS.filter((id) => section.combos.some((c) => c && c.id === id
    && (c.status === 'fatal' || (c.status === 'ran' && typeof c.score === 'number' && c.score < FAIL_BELOW))));
}

// The workflow's labels for a report's section: toolkit:<combination>-fail, from the fixed list only.
export const TOOLKIT_LABELS = TOOLKIT_COMBOS.map((id) => `toolkit:${id}-fail`);
export function toolkitLabels(section) {
  return failedCombos(section).map((id) => `toolkit:${id}-fail`);
}

// One short line per combination that ran, for the issue comment and the site: "gtk4-wayland 98.0 %".
export function toolkitRows(section) {
  if (!section || !Array.isArray(section.combos)) return [];
  return section.combos.filter((c) => c && TOOLKIT_COMBOS.includes(c.id) && (c.status === 'ran' || c.status === 'fatal')).map((c) => ({
    id: c.id,
    score: c.status === 'fatal' ? 0 : typeof c.score === 'number' ? c.score : null,
    fatal: c.status === 'fatal' ? (c.fatal || 'no-result') : null,
    version: c.version || '',
    failed: failedCombos({ combos: [c] }).length > 0,
  }));
}
