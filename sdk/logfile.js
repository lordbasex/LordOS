// logfile.js — reading LordOS's own log files for the Logs app (server/src/lordlog.h; docs/SYSTEM.md, "Log files").
// Pure logic, no lord.*: node server/tests/logfile_test.mjs
//
//   parseLordLog(text, file) → [{ t, line, p, id, u, pid, uid, k, b, m }]   rows like the journal's (t in µs, p a syslog priority)
//   fileGroup(name)          → 'session' | 'apps' | 'web' | 'old'           where the Logs sidebar lists a file
//   fileLabel(name)          → the name shown: "compositor", "compositor (previous)" is done by the app with t()
//   isPrevious(name)         → true for "<name>.log.prev" (the previous session's, kept at login)
//   isOlder(name)            → true for "<name>.log.old" (the older part the 16 MB cap moved aside)
//   levelMatch(p, level)     → the level filter: 'all', or exactly 'error' (0..3), 'warning' (4), 'info' (5, 6)
//   cloudAccounts(rows)      → cloud.log's rows with the account each one is about (acct), rclone's copies folded
//
// A line written by LordOS is "2026-11-11T12:43:56.123-03:00 INFO  xwayland: message" (local time with its
// offset, ERROR|WARN|INFO|DEBUG, the subsystem). rclone's own (cloud.log) is "2026/11/17 10:12:59 ERROR : message"
// in local time; the lines that follow one without a stamp of their own are the rest of its message (an error
// with a provider's JSON details). Anything else (a Linux app's own output, or a log from an older version) gets
// no time and a level from its words.

const STAMP = /^(\d{4})-(\d\d)-(\d\d)T(\d\d):(\d\d):(\d\d)\.(\d{3})([+-])(\d\d):(\d\d) (ERROR|WARN|INFO|DEBUG) +([^:\s][^:]{0,60}?): ?(.*)$/;
const PRIO = { ERROR: 3, WARN: 4, INFO: 6, DEBUG: 7 };
// rclone's log line (fs/log: date, time, then the level padded to 6 and a colon); #183: before 0.8.8.10 these got no
// time and a level guessed from their words, and each line of a multi-line error was a row of its own
const RCLONE = /^(\d{4})\/(\d\d)\/(\d\d) (\d\d):(\d\d):(\d\d)(?:\.(\d{1,6}))? (EMERGENCY|ALERT|CRITICAL|ERROR|WARNING|NOTICE|INFO|DEBUG) *: ?(.*)$/;
const RCLONE_PRIO = { EMERGENCY: 0, ALERT: 1, CRITICAL: 2, ERROR: 3, WARNING: 4, NOTICE: 5, INFO: 6, DEBUG: 7 };

function stampTime(m) {
  const utc = Date.UTC(+m[1], +m[2] - 1, +m[3], +m[4], +m[5], +m[6], +m[7]);
  const off = (+m[9] * 60 + +m[10]) * (m[8] === '-' ? -1 : 1);
  return (utc - off * 60000) * 1000;
}

// the old guess, for lines without a stamp; the Spanish words match lines written by older LordOS builds
function guessLevel(low) {
  return /\b(error|err|fail|failed|falla|fallo|abort|crash|segfault)\b/.test(low) ? 3
    : /\b(warning|warn|could not|cannot|aviso|sin |no se pudo)\b/.test(low) ? 4 : 6;
}

export function fileBase(file) {
  return String(file || '').replace(/^.*\//, '').replace(/\.log(\.prev|\.old)?$/, '');
}

export function parseLordLog(text, file) {
  const out = [];
  const lines = String(text || '').split('\n');
  const dflt = fileBase(file);
  let rc = null;                      // the last row of rclone's: unstamped lines after it are the rest of its message
  for (let i = 0; i < lines.length; i++) {
    const line = lines[i];
    if (!line) continue;
    const s = STAMP.exec(line);
    if (s) {
      const sub = s[12];
      const pid = /\[(\d+)\]$/.exec(sub);
      out.push({ t: stampTime(s), line: i + 1, p: PRIO[s[11]], id: pid ? sub.slice(0, pid.index) : sub, u: '', pid: pid ? +pid[1] : -1,
        uid: -1, k: 0, b: '', m: s[13] });
      rc = null;
      continue;
    }
    const r = RCLONE.exec(line);
    if (r) {
      const ms = r[7] ? Math.floor(+r[7].padEnd(6, '0') / 1000) : 0;
      rc = { t: new Date(+r[1], +r[2] - 1, +r[3], +r[4], +r[5], +r[6], ms).getTime() * 1000, line: i + 1, p: RCLONE_PRIO[r[8]],
        id: 'rclone', u: '', pid: -1, uid: -1, k: 0, b: '', m: r[9] };
      out.push(rc);
      continue;
    }
    if (rc) { rc.m += `\n${line}`; continue; }
    const src = /^([a-z][\w.-]{1,30})(\[\d+\])?:/i.exec(line);
    out.push({ t: 0, line: i + 1, p: guessLevel(line.toLowerCase()), id: src ? src[1] : dflt, u: '', pid: -1, uid: -1, k: 0, b: '', m: line });
  }
  return out;
}

// The Logs app's level filter (#183): each choice shows that level only, as its name says ("Warnings" are the
// warnings, not the errors too); notice and info are both "Info", debug shows under "All" only. For the journal,
// LEVEL_MAX is what to ask lord-server for (it gives a priority and everything more urgent) before levelMatch.
export const LEVEL_MAX = { info: 6, warning: 4, error: 3 };
export function levelMatch(p, level) {
  if (!LEVEL_MAX[level]) return true;
  const v = p == null || p < 0 ? 6 : p;
  return level === 'error' ? v <= 3 : level === 'warning' ? v === 4 : v === 5 || v === 6;
}

// #183: which Cloud account a line of cloud.log is about. rclone serves every account and its lines name none, so
// lord-server adds a line of its own after each call about an account that failed (cloud.c, note_failure):
// "ERROR c1a2b3c4d: operations/list: <rclone's error>". Such a row gets acct; rclone's own copy of it
// ('rc: "operations/list": error: …', a moment before) is folded into it (its message, with its lines, replaces the
// one-line one), and rclone's lines just before it that end with the same error get the account too (rclone says
// "error listing: …" and then the call's error). Rows of the same second only, so another account's lines aren't taken.
const ACCT = /^c[0-9a-f]{8}$/;
const norm = (x) => String(x || '').replace(/\s+/g, ' ').trim();
const tailOf = (m) => { const i = m.indexOf(': '); return i < 0 ? '' : m.slice(i + 2); };
export function cloudAccounts(rows) {
  const out = (rows || []).map((r) => ({ ...r }));
  const drop = new Set();
  for (let i = 0; i < out.length; i++) {
    const a = out[i];
    if (!ACCT.test(a.id || '') || a.acct) continue;
    a.acct = a.id;
    const at = a.m.indexOf(': ');
    if (at < 0) continue;
    const method = a.m.slice(0, at), err = norm(a.m.slice(at + 2));
    const head = `rc: "${method}": error: `;
    let folded = false;
    for (let j = i - 1; j >= 0 && j >= i - 60; j--) {
      const r = out[j];
      if (r.id !== 'rclone' || r.acct || drop.has(j)) continue;
      if (r.t && a.t && (a.t - r.t > 3e6 || r.t - a.t > 1e6)) break;
      const m = norm(r.m);
      if (!folded && r.m.startsWith(head)) {
        const x = norm(r.m.slice(head.length));
        if (x === err || x.startsWith(err) || err.startsWith(x)) { drop.add(j); folded = true; a.m = `${method}: ${r.m.slice(head.length)}`; continue; }
      }
      const tail = norm(tailOf(m));
      if (tail.length >= 20 && err.includes(tail.slice(0, 80))) r.acct = a.acct;
    }
  }
  return drop.size ? out.filter((_, i) => !drop.has(i)) : out;
}

export function fileGroup(name) {
  const n = String(name || '');
  if (n.startsWith('apps/')) return 'apps';
  if (n.startsWith('web/')) return 'web';
  if (n.startsWith('cache/')) return 'old';
  return 'session';
}

export const isPrevious = (name) => /\.log\.prev$/.test(String(name || ''));
export const isOlder = (name) => /\.log\.old$/.test(String(name || ''));
const kind = (name) => (isOlder(name) ? 1 : isPrevious(name) ? 2 : 0);

export function fileLabel(name) { return fileBase(name); }

// The sidebar's order: the session's files first (compositor, shell, then the rest by name; each previous
// one right after its current one, after its older part), then apps, web apps and the old place, each by name.
export function sortFiles(files) {
  const rank = { session: 0, apps: 1, web: 2, old: 3 };
  const first = { compositor: 0, shell: 1 };
  return [...(files || [])].sort((a, b) => {
    const ga = fileGroup(a.name), gb = fileGroup(b.name);
    if (ga !== gb) return rank[ga] - rank[gb];
    const ba = fileBase(a.name), bb = fileBase(b.name);
    const fa = first[ba] ?? 9, fb = first[bb] ?? 9;
    if (ga === 'session' && fa !== fb) return fa - fb;
    if (ba !== bb) return ba < bb ? -1 : 1;
    return kind(a.name) - kind(b.name);
  });
}
