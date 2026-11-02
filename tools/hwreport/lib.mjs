// lib.mjs — checking a shared hardware report (0.8.7, docs/BENCHMARK.md "0.8.7 — Community hardware reports"):
// finding the attached file in an issue, downloading it safely, parsing it with limits, validating it against
// the schema, the privacy scan, the plausibility checks against the reference machines, the labels and the
// comment. Pure functions plus one download function that takes its fetch as a parameter (the tests pass a
// fake one). Used by tools/hwreport/validate.mjs (the workflow and the command line).
//
// Everything in an issue is untrusted: its text is only searched for attachment links of a fixed form, the
// file is fetched only from GitHub's attachment hosts, without credentials, at most MAX_BYTES, parsed as JSON
// (never evaluated) with depth and size limits, and only values that passed the schema reach the comment,
// inside code spans. Labels come from fixed lists, never from the report's text.
import { validate } from './jsonschema.mjs';
import { REFERENCES } from '../../apps/sysinfo/references.js';
import { reportKind, gpuVendor, cpuVendor, machineLabel, sameVersion, mainGpu, bootInfo, BOOT_MEDIA, FORMAT } from '../../apps/sysinfo/share.js';

export const MAX_BYTES = 512 * 1024;
export const MAX_DEPTH = 12;
export const MAX_NODES = 40000;
export const MAX_ATTACHMENTS = 3;
export const MARKER = '<!-- lordos-hwreport-check -->';

// ---- the attachment ----

// GitHub's attachment links in an issue's text: https://github.com/user-attachments/files/<n>/<name>.json, or
// the older https://github.com/<owner>/<repo>/files/<n>/<name>.json of this repository only.
export function findAttachments(body, repo) {
  const text = String(body || '').slice(0, 65536);
  const out = [];
  const esc = (s) => s.replace(/[.*+?^${}()|[\]\\]/g, '\\$&');
  const repoRe = /^[A-Za-z0-9_.-]{1,100}\/[A-Za-z0-9_.-]{1,100}$/.test(repo || '') ? esc(repo) : null;
  const re = new RegExp(`https://github\\.com/(?:user-attachments/files|${repoRe || '(?!)'}/files)/(\\d{1,12})/([A-Za-z0-9._-]{1,128}\\.json)(?=[\\s)\\]>"']|$)`, 'g');
  for (const m of text.matchAll(re)) {
    if (!out.includes(m[0])) out.push(m[0]);
    if (out.length >= MAX_ATTACHMENTS) break;
  }
  return out;
}

const HOSTS = [/^github\.com$/, /^objects\.githubusercontent\.com$/, /^private-user-images\.githubusercontent\.com$/,
  /^github-production-user-asset-[a-z0-9]{1,16}\.s3\.amazonaws\.com$/, /^github-cloud\.s3\.amazonaws\.com$/];
export function allowedUrl(u) {
  let url;
  try { url = new URL(u); } catch (e) { return false; }
  return url.protocol === 'https:' && !url.username && !url.password && (url.port === '' || url.port === '443') && HOSTS.some((h) => h.test(url.hostname));
}

// Downloads one attachment: https only, GitHub's hosts only (each redirect checked), no credentials or cookies,
// at most maxBytes (checked against Content-Length and while reading), a timeout, strict UTF-8.
export async function fetchAttachment(url, { fetchImpl = globalThis.fetch, maxBytes = MAX_BYTES, timeoutMs = 20000, maxRedirects = 4 } = {}) {
  let u = url;
  for (let hop = 0; hop <= maxRedirects; hop++) {
    if (!allowedUrl(u)) throw new Error('the attachment is not on GitHub\'s attachment hosts');
    const ctl = new AbortController();
    const timer = setTimeout(() => ctl.abort(), timeoutMs);
    try {
      const r = await fetchImpl(u, { redirect: 'manual', credentials: 'omit', signal: ctl.signal, headers: { accept: 'application/json, application/octet-stream, text/plain' } });
      if (r.status >= 300 && r.status < 400) {
        const loc = r.headers.get('location');
        if (!loc) throw new Error('a redirect without a location');
        u = new URL(loc, u).toString();
        continue;
      }
      if (r.status !== 200) throw new Error(`the attachment answered ${r.status}`);
      const cl = Number(r.headers.get('content-length'));
      if (Number.isFinite(cl) && cl > maxBytes) throw new Error(`the attachment is larger than ${maxBytes} bytes`);
      const chunks = [];
      let n = 0;
      if (r.body && typeof r.body.getReader === 'function') {
        const rd = r.body.getReader();
        for (;;) {
          const { done, value } = await rd.read();
          if (done) break;
          n += value.length;
          if (n > maxBytes) { try { await rd.cancel(); } catch (e) { /* closing */ } throw new Error(`the attachment is larger than ${maxBytes} bytes`); }
          chunks.push(value);
        }
      } else {
        const b = new Uint8Array(await r.arrayBuffer());
        if (b.length > maxBytes) throw new Error(`the attachment is larger than ${maxBytes} bytes`);
        chunks.push(b);
        n = b.length;
      }
      const all = new Uint8Array(n);
      let o = 0;
      for (const c of chunks) { all.set(c, o); o += c.length; }
      return new TextDecoder('utf-8', { fatal: true }).decode(all);
    } finally { clearTimeout(timer); }
  }
  throw new Error('too many redirects');
}

// JSON.parse (no reviver, nothing evaluated), then limits on depth and on the number of values; "__proto__",
// "constructor" and "prototype" keys are refused outright.
export function parseLimited(text, { maxBytes = MAX_BYTES, maxDepth = MAX_DEPTH, maxNodes = MAX_NODES } = {}) {
  const s = String(text);
  if (s.length > maxBytes) throw new Error('the file is too big');
  const v = JSON.parse(s.charCodeAt(0) === 0xfeff ? s.slice(1) : s);
  let nodes = 0;
  (function walk(x, d) {
    if (++nodes > maxNodes) throw new Error('the file has too many values');
    if (d > maxDepth) throw new Error('the file is nested too deep');
    if (x && typeof x === 'object') {
      for (const k of Object.keys(x)) {
        if (k === '__proto__' || k === 'constructor' || k === 'prototype') throw new Error('the file has a key that is not allowed');
        walk(x[k], d + 1);
      }
    }
  })(v, 0);
  return v;
}

// ---- privacy ----

// What must never be in a report, even inside an allowed text field: MAC and IP addresses, UUIDs, e-mail
// addresses and home-folder paths. Version fields are skipped for the IPv4 form (0.8.4.1 looks like one).
const PRIVATE = [
  ['mac', /\b[0-9a-f]{2}(?:[:-][0-9a-f]{2}){5}\b/i],
  ['uuid', /\b[0-9a-f]{8}-[0-9a-f]{4}-[0-9a-f]{4}-[0-9a-f]{4}-[0-9a-f]{12}\b/i],
  ['email', /[A-Za-z0-9._%+-]+@[A-Za-z0-9.-]+\.[A-Za-z]{2,}/],
  ['path', /(?:^|[\s"'=:])(?:\/home\/|\/root\/|\/Users\/|\/run\/user\/|~\/)/],
  ['ipv6', /\b(?:[0-9a-f]{1,4}:){4,7}[0-9a-f]{1,4}\b/i],
];
const IPV4 = /\b(?:25[0-5]|2[0-4]\d|1?\d?\d)(?:\.(?:25[0-5]|2[0-4]\d|1?\d?\d)){3}\b/;
const VERSION_KEYS = new Set(['lordos', 'version', 'kernel', 'osVersion', 'biosVersion', 'biosDate', 'microcode']);
export function privacyScan(v) {
  const found = [];
  (function walk(x, key, path) {
    if (found.length >= 10) return;
    if (typeof x === 'string') {
      const hit = PRIVATE.find(([, re]) => re.test(x));         /* one finding per value is enough */
      if (hit) found.push({ path, kind: hit[0] });
      else if (!VERSION_KEYS.has(key) && IPV4.test(x)) found.push({ path, kind: 'ipv4' });
    } else if (x && typeof x === 'object') for (const k of Object.keys(x)) walk(x[k], k, `${path}/${k}`);
  })(v, '', '');
  return found;
}

// ---- plausibility ----

// Absolute ceilings no 2026 machine reaches, per measurement unit (beyond them the number is wrong, whatever
// the machine), and how far past the fastest reference machine a result may go before a person looks at it.
const CEILING = { 'MB/s': 200000, 'GB/s': 2000, 'GFLOP/s': 100000, 'runs/s': 10000000, fps: 100000, 'Mpx/s': 10000000, IOPS: 50000000, ns: 100000, ms: 100000, '%': 100 };
export const REF_FACTOR = 50;

export function plausibility(rep, { today = new Date().toISOString().slice(0, 10) } = {}) {
  const flags = [];
  const flag = (id, detail = '') => flags.push({ id, detail: String(detail) });
  const s = rep.system || {};
  const threads = s.cpu && s.cpu.threads;
  if (threads != null && (threads < 1 || threads > 1024)) flag('cpu-threads', threads);
  const kb = s.memory && s.memory.totalKB;
  if (kb != null && (kb < 256 * 1024 || kb > 64 * 1024 * 1024 * 1024)) flag('memory-size', kb);
  if (rep.createdAt > today) flag('date-future', rep.createdAt);
  if (s.software && s.software.lordos && !sameVersion(s.software.lordos, rep.lordos)) flag('version-mismatch', `${s.software.lordos} / ${rep.lordos}`);
  // the boot medium (0.8.7): a copy to memory only exists in a live session, a link speed only for USB
  const boot = bootInfo(rep);
  if (boot.toRam && rep.live !== true) flag('boot-mismatch', 'toram, installed');
  if (boot.mbps && boot.medium !== 'usb') flag('boot-mismatch', `${boot.mbps} Mbps, ${boot.medium || 'unknown'}`);
  for (const [which, b] of [['bench', rep.bench], ['desktop', rep.desktop]]) {
    if (!b) continue;
    if (b.lordos && !sameVersion(b.lordos, rep.lordos)) flag('bench-version', `${which}: ${b.lordos}`);
    if (b.live === true && rep.live !== true) flag('live-mismatch', which);
    if (b.arch && s.cpu && s.cpu.arch && b.arch !== s.cpu.arch) flag('arch-mismatch', `${which}: ${b.arch} / ${s.cpu.arch}`);
    if (b.cpus && threads && b.cpus !== threads) flag('cpu-mismatch', `${which}: ${b.cpus} / ${threads}`);
    const refs = REFERENCES.filter((r) => r.lordbench === b.lordbench);
    for (const [id, m] of Object.entries(b.results || {})) {
      const val = m.value;
      if (!(val > 0)) { if (!(val === 0 && /missed$/.test(id))) flag('not-positive', `${id}=${val}`); continue; }
      if (m.unit && CEILING[m.unit] != null && val > CEILING[m.unit]) { flag('beyond-ceiling', `${id}=${val} ${m.unit}`); continue; }
      const vals = refs.map((r) => r.results[id] && r.results[id].value).filter((x) => x > 0);
      if (!vals.length) continue;
      const lower = m.lowerBetter === true || id === 'mem.latency';
      const best = lower ? Math.min(...vals) : Math.max(...vals);
      const ratio = lower ? best / val : val / best;
      if (ratio > REF_FACTOR) flag('far-above-references', `${id}: ×${Math.round(ratio)}`);
    }
    const r = b.results || {};
    if (r['sustained.start'] && r['sustained.end'] && r['sustained.end'].value > r['sustained.start'].value * 1.5) flag('sustained-rise', `${r['sustained.start'].value} → ${r['sustained.end'].value}`);
  }
  return flags;
}

// ---- the whole check ----

// { status: 'valid' | 'check' | 'invalid', errors, privacy, flags, labels, summary }
export function checkReport(rep, schema, opts = {}) {
  const errors = validate(schema, rep, { maxErrors: 20 });
  if (!errors.length && rep.source !== 'community') errors.push({ path: '/source', message: 'only "community" reports come from issues' });
  const privacy = errors.length ? [] : privacyScan(rep);
  const flags = errors.length || privacy.length ? [] : plausibility(rep, opts);
  const status = errors.length || privacy.length ? 'invalid' : flags.length ? 'check' : 'valid';
  return { status, errors, privacy, flags, labels: labelsFor(status, errors.length ? null : rep), summary: errors.length ? null : summaryOf(rep) };
}

export const STATUS_LABELS = ['report:valid', 'report:check', 'report:invalid', 'report:missing'];
const KINDS = ['vm', 'board', 'laptop', 'server', 'desktop'];
const GPU_V = ['intel', 'amd', 'nvidia', 'virtual', 'broadcom', 'arm', 'vivante', 'qualcomm', 'other', 'none'];
const CPU_V = ['intel', 'amd', 'arm', 'riscv', 'other'];
// Every label the workflow can set (the repository must have them: docs/RELEASING.md, "The labels of hardware reports").
export const ALL_LABELS = ['hardware-report', ...STATUS_LABELS, ...KINDS.map((k) => `kind:${k}`), ...GPU_V.map((g) => `gpu:${g}`),
  ...CPU_V.map((c) => `cpu:${c}`), 'live', 'installed', ...BOOT_MEDIA.map((b) => `boot:${b}`)];

// Labels from fixed lists only.
export function labelsFor(status, rep) {
  const out = ['hardware-report', `report:${status}`];
  if (rep) {
    const k = reportKind(rep), g = gpuVendor(rep), c = cpuVendor(rep);
    if (KINDS.includes(k)) out.push(`kind:${k}`);
    if (GPU_V.includes(g)) out.push(`gpu:${g}`);
    if (CPU_V.includes(c)) out.push(`cpu:${c}`);
    out.push(rep.live ? 'live' : 'installed');
    const b = bootInfo(rep).medium;
    if (BOOT_MEDIA.includes(b)) out.push(`boot:${b}`);
  }
  return out;
}

function summaryOf(rep) {
  const s = rep.system || {};
  const g = mainGpu(s);
  return {
    machine: machineLabel(rep), kind: reportKind(rep), lordos: rep.lordos, live: rep.live, boot: bootInfo(rep).medium,
    cpu: (s.cpu && s.cpu.model) || '', threads: (s.cpu && s.cpu.threads) || 0,
    gpu: g ? `${[g.vendor, g.device].filter(Boolean).join(' ')}${g.driver ? ` (${g.driver})` : ''}` : '',
    compat: (rep.compatibility || []).filter((c) => c.state !== 'absent').map((c) => `${c.id}: ${c.state}`),
    bench: rep.bench ? `${rep.bench.mode}, ${Object.keys(rep.bench.results || {}).length} measurements` : '',
  };
}

// ---- the comment ----

// A value in a code span: no backticks, no line breaks, at most 120 characters (code spans don't render links
// or mentions, so nothing in a report can ping anyone or inject Markdown).
export const code = (v) => {
  const t = String(v == null ? '' : v).replace(/[`\r\n\u0000-\u001f\u007f\u2028\u2029]/g, ' ').slice(0, 120).trim();
  return t ? `\`${t}\`` : '—';
};

const STATUS_TEXT = {
  valid: '✅ **The report is valid.** Thanks! A maintainer will review it and add it to the tested-hardware catalogue.',
  check: '🔎 **The report is valid, but some numbers look unusual for this hardware.** A maintainer will look at them before it is added to the catalogue.',
  invalid: '❌ **The report could not be accepted as it is.** Please create it again with System Information → Report → Share on GitHub, and attach the `.json` file it saved, unchanged.',
  missing: '📎 **No report file was found.** Please attach the `lordos-hwreport-….json` file that System Information saved (drag it into the “Report file” box when you edit the issue).',
};

export function commentBody(result, { fileName = '' } = {}) {
  const L = [MARKER, '### LordOS hardware report check', '', STATUS_TEXT[result.status] || STATUS_TEXT.invalid, ''];
  if (fileName) L.push(`File: ${code(fileName)}`, '');
  if (result.summary) {
    const s = result.summary;
    L.push('| | |', '|---|---|',
      `| Machine | ${code(s.machine)} (${code(s.kind)}) |`,
      `| LordOS | ${code(s.lordos)}${s.live ? ' — live session' : ''}${s.boot ? ` — started from ${code(s.boot)}` : ''} |`,
      `| CPU | ${code(s.cpu)}${s.threads ? ` — ${s.threads} threads` : ''} |`,
      `| GPU | ${code(s.gpu)} |`,
      `| Compatibility | ${s.compat.length ? s.compat.slice(0, 12).map(code).join(' ') : '—'} |`,
      `| LordBench | ${s.bench ? code(s.bench) : 'not included'} |`, '');
  }
  if (result.errors && result.errors.length) {
    L.push('**What doesn’t match the format:**', '');
    for (const e of result.errors.slice(0, 10)) L.push(`- ${code(e.path)} ${code(e.message)}`);
    L.push('');
  }
  if (result.privacy && result.privacy.length) {
    L.push('**It holds something that looks personal** (a report never carries addresses, identifiers or home-folder paths), so it was not accepted. Please don’t edit the file by hand; create it again:', '');
    for (const p of result.privacy.slice(0, 10)) L.push(`- ${code(p.path)}: ${code(p.kind)}`);
    L.push('');
  }
  if (result.flags && result.flags.length) {
    L.push('**Numbers to look at:**', '');
    for (const f of result.flags.slice(0, 15)) L.push(`- ${code(f.id)} ${code(f.detail)}`);
    L.push('');
  }
  if (result.error) L.push(`Details: ${code(result.error)}`, '');
  L.push('<sub>Checked automatically by the hardware-report workflow (`tools/hwreport/validate.mjs`). Edit the issue to check again.</sub>');
  return L.join('\n');
}

export { FORMAT };
