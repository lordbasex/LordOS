// logfile.js — reading LordOS's own log files for the Logs app (server/src/lordlog.h; docs/SYSTEM.md, "Log files").
// Pure logic, no lord.*: node server/tests/logfile_test.mjs
//
//   parseLordLog(text, file) → [{ t, line, p, id, u, pid, uid, k, b, m }]   rows like the journal's (t in µs, p a syslog priority)
//   fileGroup(name)          → 'session' | 'apps' | 'web' | 'old'           where the Logs sidebar lists a file
//   fileLabel(name)          → the name shown: "compositor", "compositor (previous)" is done by the app with t()
//   isPrevious(name)         → true for "<name>.log.prev" (the previous session's)
//
// A line written by LordOS is "2026-11-11T12:43:56.123-03:00 INFO  xwayland: message" (local time with its
// offset, ERROR|WARN|INFO|DEBUG, the subsystem). Anything else (a Linux app's own output, or a log from an
// older version) gets no time and a level from its words.

const STAMP = /^(\d{4})-(\d\d)-(\d\d)T(\d\d):(\d\d):(\d\d)\.(\d{3})([+-])(\d\d):(\d\d) (ERROR|WARN|INFO|DEBUG) +([^:\s][^:]{0,60}?): ?(.*)$/;
const PRIO = { ERROR: 3, WARN: 4, INFO: 6, DEBUG: 7 };

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
  return String(file || '').replace(/^.*\//, '').replace(/\.log(\.prev)?$/, '');
}

export function parseLordLog(text, file) {
  const out = [];
  const lines = String(text || '').split('\n');
  const dflt = fileBase(file);
  for (let i = 0; i < lines.length; i++) {
    const line = lines[i];
    if (!line) continue;
    const s = STAMP.exec(line);
    if (s) {
      const sub = s[12];
      const pid = /\[(\d+)\]$/.exec(sub);
      out.push({ t: stampTime(s), line: i + 1, p: PRIO[s[11]], id: pid ? sub.slice(0, pid.index) : sub, u: '', pid: pid ? +pid[1] : -1,
        uid: -1, k: 0, b: '', m: s[13] });
      continue;
    }
    const src = /^([a-z][\w.-]{1,30})(\[\d+\])?:/i.exec(line);
    out.push({ t: 0, line: i + 1, p: guessLevel(line.toLowerCase()), id: src ? src[1] : dflt, u: '', pid: -1, uid: -1, k: 0, b: '', m: line });
  }
  return out;
}

export function fileGroup(name) {
  const n = String(name || '');
  if (n.startsWith('apps/')) return 'apps';
  if (n.startsWith('web/')) return 'web';
  if (n.startsWith('cache/')) return 'old';
  return 'session';
}

export const isPrevious = (name) => /\.log\.prev$/.test(String(name || ''));

export function fileLabel(name) { return fileBase(name); }

// The sidebar's order: the session's files first (compositor, shell, then the rest by name; each previous
// one right after its current one), then apps, web apps and the old place, each by name.
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
    return (isPrevious(a.name) ? 1 : 0) - (isPrevious(b.name) ? 1 : 0);
  });
}
