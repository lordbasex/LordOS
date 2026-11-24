// diag.js — unified diagnostics (6.19): one place that says what a machine did, from what lord-vm wrote (run.json
// through lord.vm.list, the machine log) and what the library knows (snapshots, saved session, shared base). Pure: the
// Logs window feeds it, server/tests/vm_model_test.mjs checks it.

const TIME = /^(\d{4}-\d\d-\d\dT\d\d:\d\d:\d\d)[.\d]*Z\s+(.*)$/;
const PROBLEM = /(fail|error|refus|crash|killed|panic|cannot|can't|couldn't|lost|stopped unexpectedly)/i;

// the last run's lines of lord-vm.log → the facts the Overview shows
export function readLog(text) {
  const out = { started: '', version: '', sandbox: '', cgroup: '', network: [], recovery: [], problems: [], notes: [] };
  for (const raw of String(text || '').split('\n')) {
    const m = raw.match(TIME);
    const at = m ? m[1].replace('T', ' ') : '';
    const l = m ? m[2] : raw;
    if (!l.trim()) continue;
    if (/^lord-vm \d+: machine /.test(l)) { out.started = at; out.recovery = []; out.problems = []; out.network = []; out.notes = []; out.cgroup = ''; }
    else if (l.startsWith('qemu: version ')) out.version = l.slice(14);
    else if (/^qemu: pid \d+; sandbox: /.test(l)) out.sandbox = l.replace(/^qemu: pid \d+; sandbox: /, '');
    else if (l.startsWith('cgroup: ')) { if (!out.cgroup || /^in |nested leaves/.test(l.slice(8))) out.cgroup = l.slice(8); }
    else if (/while the machine stops/.test(l)) continue;      // passt going with its machine: not news
    else if (/passt|network/i.test(l) && /note|stopped|ended|is back/i.test(l)) out.network.push({ at, text: l });
    else if (l.startsWith('recovery: ')) out.recovery.push({ at, text: l.slice(10) });
    else if (l.startsWith('note: ')) out.notes.push({ at, text: l.slice(6) });
    else if (PROBLEM.test(l) && !/^qemu: event /.test(l)) out.problems.push({ at, text: l });
  }
  out.problems = out.problems.slice(-12);
  return out;
}

// the report Copy puts on the clipboard: plain lines, no secrets (the log never holds any)
export function reportText(m, log, extra) {
  const L = [];
  L.push(`Machine: ${m.name} (${m.id || '?'})`);
  L.push(`System: ${m.arch}, ${m.accel === 'tcg' ? 'emulated' : 'KVM'}, ${m.vcpus} vCPU, ${m.memoryMb} MB, ${m.diskGb} GB, ${m.firmware}, network ${m.network}`);
  L.push(`State: ${m.state} (${m.reason})${m.detail ? `: ${m.detail}` : ''}${m.since ? `, since ${m.since}` : ''}`);
  if (log.version) L.push(`Engine: QEMU ${log.version}`);
  if (log.sandbox) L.push(`Sandbox: ${log.sandbox}`);
  if (log.cgroup) L.push(`Limits: ${log.cgroup}`);
  if (m.base) L.push(`Linked clone of: ${m.base}`);
  if (extra?.snapshots != null) L.push(`Snapshots: ${extra.snapshots}`);
  if (m.suspended) L.push('Saved session: yes');
  if (m.operation) L.push(`Interrupted operation waiting: ${m.operation}`);
  for (const r of log.recovery) L.push(`Recovery ${r.at}: ${r.text}`);
  for (const n of log.network) L.push(`Network ${n.at}: ${n.text}`);
  for (const p of log.problems) L.push(`Problem ${p.at}: ${p.text}`);
  return `${L.join('\n')}\n`;
}

// ---- the guest's journal and event logs, as Logs shows them (6.19) ----
// A line of journalctl's short-iso ("2026-11-24T05:00:00-0300 host unit[pid]: words") or of a Windows guest's event logs
// (lord-vm writes them the same way): its time in this computer's local time (HH:MM:SS), the guest's offset from UTC in
// minutes (null when the line has none), the unit or source, and the rest. A line that isn't one comes back whole.
const JLINE = /^(\d{4})-(\d\d)-(\d\d)T(\d\d):(\d\d):(\d\d)(?:[.,]\d+)?(Z|[+-]\d\d:?\d\d)\s+(.*)$/;
const pad2 = (n) => String(n).padStart(2, '0');
export function journalLine(l, tzOffsetMin = -new Date().getTimezoneOffset()) {
  const m = String(l || '').match(JLINE);
  if (!m) return { at: '', offset: null, unit: '', body: String(l || '') };
  const off = m[7] === 'Z' ? 0 : (m[7][0] === '-' ? -1 : 1) * (Number(m[7].slice(1, 3)) * 60 + Number(m[7].slice(-2)));
  const utc = Date.UTC(+m[1], +m[2] - 1, +m[3], +m[4], +m[5], +m[6]) - off * 60000;
  const local = new Date(utc + tzOffsetMin * 60000);
  const at = `${pad2(local.getUTCHours())}:${pad2(local.getUTCMinutes())}:${pad2(local.getUTCSeconds())}`;
  const rest = m[8];
  const u = rest.match(/^\S+\s+([\w@.\-]+)(\[\d+\])?:/);
  return { at, offset: off, unit: u ? u[1] : '', body: rest };
}
// "UTC+05:30", "UTC−03:00", "UTC" for an offset in minutes
export function tzLabel(min) {
  if (!Number.isFinite(min) || !min) return 'UTC';
  const a = Math.abs(min);
  return `UTC${min < 0 ? '−' : '+'}${pad2(Math.floor(a / 60))}:${pad2(a % 60)}`;
}
// the guest's offset when its last lines say one that isn't this computer's (the footer names it), else null
export function guestZone(lines, tzOffsetMin = -new Date().getTimezoneOffset()) {
  for (let i = lines.length - 1; i >= 0 && i >= lines.length - 50; i--) {
    const j = journalLine(lines[i], tzOffsetMin);
    if (j.offset != null) return j.offset === tzOffsetMin ? null : j.offset;
  }
  return null;
}
