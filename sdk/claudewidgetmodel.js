// claudewidgetmodel.js — the Claude Code widget's plain logic (0.8.8.10; server/tests/claude_widget_model_test.mjs).
// Sessions come from each Claude Code configuration folder in the home: ~/.claude and any ~/.claude-* (another account
// through CLAUDE_CONFIG_DIR, e.g. ~/.claude-cuenta2), each with its sessions/<pid>.json; the context and the plan's
// limits from the status line JSON lord-claude-status keeps (~/.cache/lordos/claude/<session>.json, apps/terminal/
// claude.js parseStatus); the account from that folder's .claude.json (oauthAccount: name, e-mail, plan) — only the
// name, the e-mail and the plan are kept, nothing else of that file.
//
//   configDirs(names)            the folders to look in, from the home's entries
//   accountOf(text)              { name, email, plan } from a .claude.json, or null
//   leftText(ms, t)              "2 d 4 h", "3 h 20 min", "12 min" through t('shell.widgets.claude.d|h|m', …)
//   alertOf(s)                   'high' (≥ 85 % of the context, the 5 hours or the week), 'warn' (≥ 70 %), or ''
//   summarize(sessions)          the busiest first, and per account its newest limits
const SESSION_ID = /^[A-Za-z0-9-]{1,64}$/;

export function configDirs(names) {
  const out = [];
  for (const n of names || []) if (n === '.claude' || /^\.claude-[A-Za-z0-9_.-]{1,40}$/.test(n)) out.push(n);
  return out.sort((a, b) => (a === '.claude' ? -1 : b === '.claude' ? 1 : a.localeCompare(b)));
}

export function accountOf(text) {
  let d = null;
  try { d = JSON.parse(text); } catch (e) { return null; }
  const a = d && typeof d === 'object' ? d.oauthAccount : null;
  if (!a || typeof a !== 'object') return null;
  const s = (v, n) => (typeof v === 'string' ? v.replace(/[\x00-\x1f\x7f]/g, '').slice(0, n) : '');
  const email = s(a.emailAddress, 120);
  const name = s(a.displayName, 60) || s(a.fullName, 60) || (email ? email.split('@')[0] : '');
  const plan = s(a.planDisplayName, 30) || (typeof a.organizationRateLimitTier === 'string' && /max/i.test(a.organizationRateLimitTier) ? 'Max' : '');
  return name || email ? { name, email, plan } : null;
}

export function leftText(ms, t) {
  if (!(ms > 0)) return '';
  const m = Math.ceil(ms / 60000), d = Math.floor(m / 1440), h = Math.floor((m % 1440) / 60), mm = m % 60;
  if (d) return t('shell.widgets.claude.d', { d, h });
  if (h) return t('shell.widgets.claude.h', { h, m: mm });
  return t('shell.widgets.claude.m', { m: Math.max(1, mm) });
}

export function alertOf(s) {
  const top = Math.max(s?.ctx ?? 0, s?.five?.pct ?? 0, s?.week?.pct ?? 0);
  return top >= 85 ? 'high' : top >= 70 ? 'warn' : '';
}

// sessions: [{ id, dir (config folder), state, ctx, model, five, week, cwd, at (status time ms) }]
export function summarize(sessions, accounts = {}) {
  const valid = (sessions || []).filter((s) => s && SESSION_ID.test(String(s.id || '')));
  const order = { working: 0, waiting: 1, '': 2 };
  valid.sort((a, b) => (order[a.state] ?? 2) - (order[b.state] ?? 2) || (b.ctx ?? -1) - (a.ctx ?? -1));
  const per = {};
  for (const s of valid) {
    const k = s.dir || '.claude';
    const cur = per[k] || (per[k] = { dir: k, account: accounts[k] || null, five: null, week: null, at: 0, sessions: 0 });
    cur.sessions++;
    if ((s.five || s.week) && (s.at || 0) >= cur.at) { cur.five = s.five || cur.five; cur.week = s.week || cur.week; cur.at = s.at || 0; }
  }
  return { sessions: valid, accounts: Object.values(per) };
}
