// claude.js — Claude Code in the Terminal's footer, the pure part (StatusBar in Chrome.jsx, ClaudeBar.jsx;
// server/tests/terminal_claude_test.mjs). Two sources, both files in the home folder:
//  - ~/.claude/sessions/<pid>.json, which Claude Code keeps for each running session (its pid is the pane's
//    foreground process group): the session's id and whether it is working or waiting for the user;
//  - ~/.cache/lordos/claude/<session>.json, the status line JSON lord-claude-status keeps once the Terminal is
//    connected (server/src/term/claudebridge.c): the context window and the plan's 5-hour and weekly limits.
// When Claude Code runs inside screen or tmux, or on another computer over ssh, the pane's process isn't claude: then
// its status line as lord-claude-status prints it is read from the screen (parseScreenStatus), and inside a local
// screen or tmux the session that matches it gives the rest (matchSession).

export const BRIDGE = '/usr/lib/lordos/bin/lord-claude-status';
export const sessionFile = (home, pid) => `${home}/.claude/sessions/${pid}.json`;
export const snapshotFile = (home, id) => `${home}/.cache/lordos/claude/${id}.json`;
export const settingsFile = (home) => `${home}/.claude/settings.json`;

const SESSION_ID = /^[A-Za-z0-9-]{1,64}$/;
const parse = (text) => { try { const v = JSON.parse(text); return v && typeof v === 'object' && !Array.isArray(v) ? v : null; } catch (e) { return null; } };

// The session file, only when it is that process's: { id, state: 'working' | 'waiting' | '' }
export function parseSession(text, pid) {
  const d = parse(text);
  if (!d || d.pid !== pid || !SESSION_ID.test(String(d.sessionId || ''))) return null;
  return { id: d.sessionId, state: d.status === 'busy' ? 'working' : d.status === 'idle' ? 'waiting' : '' };
}

const pct = (v) => (v == null || v === '' || !Number.isFinite(+v) ? null : Math.max(0, Math.min(100, Math.round(+v))));

// A limit's window, while it hasn't reset (resets_at in epoch seconds; now in ms)
function limit(w, now) {
  const p = pct(w?.used_percentage);
  if (p == null) return null;
  const resetsAt = Number.isFinite(+w.resets_at) && +w.resets_at > 0 ? +w.resets_at * 1000 : null;
  return resetsAt != null && resetsAt <= now ? null : { pct: p, resetsAt };
}

// The status line JSON → what the footer shows; null when it isn't that session's. A subscriber's limits
// (rate_limits present) keep the 5-hour slot even when Claude Code leaves that window out (it drops a window
// once it resets and may omit one on its own): { pct: null } shows as "5h –" instead of vanishing.
export function parseStatus(text, id, now = Date.now()) {
  const d = parse(text);
  if (!d || d.session_id !== id) return null;
  const cw = d.context_window || {};
  const name = typeof d.model?.display_name === 'string' ? d.model.display_name.slice(0, 32) : '';
  return {
    model: name,
    ctx: pct(cw.used_percentage),
    ctxTokens: Number.isFinite(+cw.total_input_tokens) ? +cw.total_input_tokens : null,
    ctxSize: Number.isFinite(+cw.context_window_size) ? +cw.context_window_size : null,
    five: limit(d.rate_limits?.five_hour, now) || (d.rate_limits && typeof d.rate_limits === 'object' ? { pct: null, resetsAt: null } : null),
    week: limit(d.rate_limits?.seven_day, now),
    cost: Number.isFinite(+d.cost?.total_cost_usd) ? +d.cost.total_cost_usd : null,
    dir: typeof d.workspace?.current_dir === 'string' ? d.workspace.current_dir : typeof d.cwd === 'string' ? d.cwd : '',
  };
}

// ---- read from the screen: screen, tmux, ssh ----

// The line lord-claude-status prints: "Opus 5.5 | LordOS | main | ctx 44% | 5h – | sem 26%" (folder, branch and the
// limits may be missing; the week's label in the user's language). The last such line on the screen; whether Claude
// Code is working comes from its own "esc to interrupt" while it works.
const VIEW = /^\s*(\S(?:.{0,62}?\S)?) \| (?:(.{1,128}?) \| )?(?:(.{1,128}?) \| )?ctx (\d{1,3})%(?: \| 5h (?:(\d{1,3})%|–))?(?: \| (?:week|sem|sett) (\d{1,3})%)?\s*$/;
export function parseScreenStatus(text) {
  const lines = String(text || '').split('\n');
  for (let i = lines.length - 1; i >= 0; i--) {
    const m = VIEW.exec(lines[i]);
    if (!m) continue;
    const n = (v) => (v == null ? null : Math.min(100, +v));
    return {
      model: m[1], folder: m[2] || '', branch: m[3] || '', ctx: n(m[4]),
      five: /\| 5h /.test(lines[i]) ? { pct: n(m[5]), resetsAt: null } : null,
      week: m[6] != null ? { pct: n(m[6]), resetsAt: null } : null,
      state: /esc to interrupt/i.test(text) ? 'working' : 'waiting',
    };
  }
  return null;
}

const baseName = (d) => String(d || '').replace(/\/+$/, '').split('/').pop() || '/';
// Of the local sessions [{ id, state, st }], the one whose status line is the one on screen: same model, context and
// folder. null when none is, or when more than one could be (then the screen's numbers are shown as they are).
export function matchSession(view, sessions) {
  if (!view) return null;
  const hits = (sessions || []).filter((x) => x && x.st && x.st.model === view.model && x.st.ctx === view.ctx
    && (!view.folder || baseName(x.st.dir) === view.folder));
  return hits.length === 1 ? hits[0] : null;
}

// green up to half, amber up to 80 %, red past it
export const level = (p) => (p == null ? 'none' : p >= 80 ? 'high' : p >= 50 ? 'warn' : 'ok');
export const LEVEL_COLORS = { ok: '#32d74b', warn: '#ffd60a', high: '#ff453a' };

// ▰▰▱▱▱: n cells, at least one lit when anything is used
export function bar(p, n = 5) {
  const k = p > 0 ? Math.max(1, Math.min(n, Math.round((p / 100) * n))) : 0;
  return '▰'.repeat(k) + '▱'.repeat(n - k);
}

// 237000 → "237k", 1000000 → "1M"
export const tokens = (n) => (n >= 1e6 ? `${+(n / 1e6).toFixed(n % 1e6 ? 1 : 0)}M` : n >= 1000 ? `${Math.round(n / 1000)}k` : String(n));

// What fits in the footer, the least needed first to go: a narrow window keeps the state and the context.
const ORDER = ['state', 'ctx', 'five', 'week', 'model'];
const MIN_COLS = { state: 0, ctx: 0, five: 64, week: 76, model: 92 };
export const fitSegments = (cols, have) => ORDER.filter((k) => have[k] && cols >= MIN_COLS[k]);

// ---- connecting Claude Code's status line (~/.claude/settings.json) ----

const quote = (s) => `'${s.replace(/'/g, `'\\''`)}'`;
const unquote = (s) => s.slice(1, -1).replace(/'\\''/g, "'");

// The status line command with lord-claude-status in front: a simple command gets it as its first word,
// anything with shell syntax goes in whole through sh -c, so it runs as before.
export function wrapCommand(cmd) {
  const c = String(cmd || '').trim();
  if (!c) return BRIDGE;
  return /[|&;<>()$`\n\\"'*?{}[\]#~=]/.test(c.replace(/^~\//, '').replace(/ ~\//g, ' ')) ? `${BRIDGE} sh -c ${quote(c)}` : `${BRIDGE} ${c}`;
}

export function unwrapCommand(cmd) {
  const c = String(cmd || '');
  if (c === BRIDGE) return '';
  if (!c.startsWith(`${BRIDGE} `)) return c;
  const rest = c.slice(BRIDGE.length + 1);
  const m = /^sh -c ('(?:[^']|'\\'')*')$/.exec(rest);
  return m ? unquote(m[1]) : rest;
}

export const isConnected = (settings) => {
  const c = settings?.statusLine?.command;
  return typeof c === 'string' && (c === BRIDGE || c.startsWith(`${BRIDGE} `));
};

// The settings with the bridge in (or out of) the status line; everything else as it was
export function connectSettings(settings) {
  const s = { ...(settings || {}) };
  if (isConnected(s)) return s;
  const old = s.statusLine && s.statusLine.type === 'command' ? s.statusLine.command : '';
  s.statusLine = { ...(s.statusLine || {}), type: 'command', command: wrapCommand(old) };
  return s;
}

export function disconnectSettings(settings) {
  const s = { ...(settings || {}) };
  if (!isConnected(s)) return s;
  const old = unwrapCommand(s.statusLine.command);
  if (old) s.statusLine = { ...s.statusLine, command: old };
  else delete s.statusLine;
  return s;
}

export const parseSettings = (text) => (text == null || !String(text).trim() ? {} : parse(text));
