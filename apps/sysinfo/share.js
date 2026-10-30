// share.js — Share on GitHub (0.8.7, docs/BENCHMARK.md "0.8.7 — Community hardware reports"): the hardware
// report that is shared, its short summary, the link to GitHub's new-issue form and the small page that
// explains it. Pure functions with no UI and no SDK, tested by server/tests/hwreport_test.mjs; the workflow's
// validator and the catalogue (tools/hwreport) use the same summary, kind and vendor rules.
//
// LordOS sends nothing by itself and holds no token: the app writes the report and the page to the user's
// Documents, opens the page with the default browser (lord.apps.launchWith('', path)), and the user attaches
// the file to the issue and submits it with their own account.
//
// The report is built from the schema (hwreport.schema.json) as an allow-list: pick() copies only the fields
// the schema lists, in the forms it allows, so a field added to the collector is never shared until it is
// added to the schema.

export const FORMAT = 'lordos-hwreport';
export const REPO_URL = 'https://github.com/lordbasex/LordOS';
export const ISSUE_TEMPLATE = 'hardware-report.yml';
export const TITLE_MAX = 100;
export const SUMMARY_MAX = 600;

// ---- picking fields by the schema ----

const reCache = new Map();
const rx = (p) => { if (!reCache.has(p)) reCache.set(p, new RegExp(p, 'u')); return reCache.get(p); };

function deref(root, s) {
  for (let n = 0; s && typeof s.$ref === 'string' && n < 32; n++) {
    const m = /^#\/\$defs\/([A-Za-z0-9_]+)$/.exec(s.$ref);
    s = m && root.$defs ? root.$defs[m[1]] : undefined;
  }
  return s;
}

const kindOf = (v) => (v === null ? 'null' : Array.isArray(v) ? 'array' : typeof v);
function typeOk(s, v) {
  if (s.type === undefined) return true;
  const ts = Array.isArray(s.type) ? s.type : [s.type];
  return ts.some((t) => (t === 'integer' ? typeof v === 'number' && Number.isInteger(v) : t === 'number' ? typeof v === 'number' && Number.isFinite(v) : kindOf(v) === t));
}

// A copy of v with only what schema s allows, or undefined when v can't be shared in any allowed form.
// Strings longer than allowed are cut; values outside an enum, a pattern or a range are dropped.
export function pick(v, s, root = s) {
  s = deref(root, s);
  if (!s || v === undefined) return undefined;
  if (s.anyOf) {
    for (const sub of s.anyOf) { const r = pick(v, sub, root); if (r !== undefined) return r; }
    return undefined;
  }
  if (!typeOk(s, v)) return undefined;
  if ('const' in s) return JSON.stringify(s.const) === JSON.stringify(v) ? v : undefined;
  if (s.enum) return s.enum.some((e) => JSON.stringify(e) === JSON.stringify(v)) ? v : undefined;
  if (typeof v === 'number') return Number.isFinite(v) && (s.minimum == null || v >= s.minimum) && (s.maximum == null || v <= s.maximum) ? v : undefined;
  if (typeof v === 'string') {
    let t = v;
    if (s.maxLength != null && [...t].length > s.maxLength) t = [...t].slice(0, s.maxLength).join('').trimEnd();
    return s.pattern == null || rx(s.pattern).test(t) ? t : undefined;
  }
  if (Array.isArray(v)) {
    const out = [];
    for (const x of v) {
      if (s.maxItems != null && out.length >= s.maxItems) break;
      const r = s.items === undefined ? undefined : pick(x, s.items, root);
      if (r !== undefined) out.push(r);
    }
    return out;
  }
  if (v && typeof v === 'object') {
    const out = {};
    let n = 0;
    for (const key of Object.keys(v)) {
      if (s.maxProperties != null && n >= s.maxProperties) break;
      let sub;
      if (s.properties && Object.prototype.hasOwnProperty.call(s.properties, key)) sub = s.properties[key];
      else if (s.additionalProperties && typeof s.additionalProperties === 'object'
        && (!s.propertyNames || !s.propertyNames.pattern || rx(s.propertyNames.pattern).test(key))) sub = s.additionalProperties;
      if (!sub) continue;
      const r = pick(v[key], sub, root);
      if (r !== undefined) { out[key] = r; n++; }
    }
    for (const r of s.required || []) if (!(r in out)) return undefined;
    return out;
  }
  return v;
}

// ---- the report ----

const baseVersion = (v) => { const m = /^(\d+\.\d+\.\d+(?:\.\d+)?)/.exec(String(v || '')); return m ? m[1] : ''; };
export const sameVersion = (a, b) => !!baseVersion(a) && baseVersion(a) === baseVersion(b);

// report: lord.sysinfo.report(); bench / desktop: the last LordBench peak and desktop results (or null);
// compat: model.js compatibility(report). A bench result is kept only when it was measured with this LordOS
// version, so the catalogue never files an old version's numbers under the new one.
export function buildShareReport({ report, bench = null, desktop = null, compat = [], schema, date, source = 'community' }) {
  const rep = report || {};
  const sw = rep.software || {};
  const lordos = String(sw.lordos || '');
  const keepBench = (b) => (b && sameVersion(b.lordos, lordos) ? pick(b, { $ref: '#/$defs/bench' }, schema) || null : null);
  const b = keepBench(bench), d = keepBench(desktop);
  const out = {
    format: FORMAT, version: 1,
    createdAt: /^\d{4}-\d{2}-\d{2}$/.test(date || '') ? date : String(rep.date || ''),
    source, lordos,
    live: sw.live === true || !!(b && b.live === true) || !!(d && d.live === true),
    system: pick(rep, { $ref: '#/$defs/system' }, schema) || {},
    compatibility: (compat || []).map((c) => ({ id: c.id, state: c.state, note: c.note || null })),
    bench: b,
    desktop: d,
  };
  out.compatibility = pick(out.compatibility, schema.properties.compatibility, schema) || [];
  return out;
}

// ---- what a report is about (shared with the workflow's labels and the catalogue) ----

export function reportKind(rep) {
  const s = (rep && rep.system) || {};
  const m = s.machine || {};
  if (m.virtualization) return 'vm';
  if (s.raspberryPi || m.firmware === 'dt') return 'board';
  if (['laptop', 'handheld', 'tablet'].includes(m.chassis)) return 'laptop';
  if (m.chassis === 'server') return 'server';
  return 'desktop';
}

// PCI vendor ids of graphics makers (lower case); everything else is "other"
const GPU_VENDORS = { '8086': 'intel', '1002': 'amd', '1022': 'amd', '10de': 'nvidia', '1af4': 'virtual', '15ad': 'virtual', '80ee': 'virtual', '1234': 'virtual', '1414': 'virtual', '1b36': 'virtual' };
const PLATFORM_GPUS = { v3d: 'broadcom', vc4: 'broadcom', panfrost: 'arm', lima: 'arm', etnaviv: 'vivante', msm: 'qualcomm', 'virtio-pci': 'virtual', virtio_gpu: 'virtual' };
// The GPU a report is about: the boot display card when it renders, else the first that renders (the Raspberry
// Pi 5's V3D: its vc4 card only drives the displays), else the first.
export function mainGpu(system) {
  const gs = (system && system.gpus) || [];
  return gs.find((x) => x.bootVga && x.renderNode) || gs.find((x) => x.renderNode) || gs[0] || null;
}

export function gpuVendor(rep) {
  const g = mainGpu((rep && rep.system) || {});
  if (!g) return 'none';
  if (g.vendorId) return GPU_VENDORS[String(g.vendorId).toLowerCase()] || 'other';
  return PLATFORM_GPUS[g.driver] || 'other';
}
export function cpuVendor(rep) {
  const c = ((rep && rep.system) || {}).cpu || {};
  if (c.vendor === 'GenuineIntel') return 'intel';
  if (c.vendor === 'AuthenticAMD') return 'amd';
  if (/^(aarch64|arm)/.test(c.arch || '')) return 'arm';
  if (/^riscv/.test(c.arch || '')) return 'riscv';
  return 'other';
}

// The one-line name of the machine: vendor and product, the device tree model, or the hypervisor.
export function machineLabel(rep) {
  const m = ((rep && rep.system) || {}).machine || {};
  return [m.vendor, m.product].filter(Boolean).join(' ') || m.model || m.virtualization || '';
}

const cpuShort = (s) => String(s || '').replace(/\(R\)|\(TM\)|\bCPU\b|\bProcessor\b/g, '').replace(/\s+/g, ' ').trim();
const oneLine = (s, max) => {
  const t = String(s || '').replace(/[\u0000-\u001f\u007f]+/g, ' ').replace(/\s+/g, ' ').trim();
  return [...t].length > max ? `${[...t].slice(0, max - 1).join('')}…` : t;
};

// What the system started from (0.8.7): the medium's class, a USB link speed, and whether a live session was
// copied to memory; '' when unknown. Classes only: the collector never gives a device name.
export const BOOT_MEDIA = ['internal', 'usb', 'sd', 'emmc', 'optical', 'network'];
export function bootInfo(rep) {
  const sw = ((rep && rep.system) || {}).software || {};
  return {
    medium: BOOT_MEDIA.includes(sw.bootMedium) ? sw.bootMedium : '',
    mbps: Number.isInteger(sw.bootLinkMbps) && sw.bootLinkMbps > 0 ? sw.bootLinkMbps : null,
    toRam: sw.bootToRam === true,
  };
}
export function bootText(rep) {
  const b = bootInfo(rep);
  return [b.medium, b.mbps ? `${b.mbps} Mbps` : '', b.toRam ? 'toram' : ''].filter(Boolean).join(', ');
}

// The short, non-identifying summary that goes into the issue's link: the machine, CPU, GPU, memory,
// LordOS version, live or installed, the boot medium's class, and the compatibility states. Nothing else of the report travels in
// the URL; the full JSON is attached by the user.
export function summaryRows(rep) {
  const s = (rep && rep.system) || {};
  const g = mainGpu(s);
  const memGB = s.memory && s.memory.totalKB ? Math.round(s.memory.totalKB / 1048576) : null;
  const rows = [
    ['machine', machineLabel(rep)],
    ['kind', reportKind(rep)],
    ['cpu', [cpuShort(s.cpu && s.cpu.model), s.cpu && s.cpu.threads ? `${s.cpu.threads}T` : ''].filter(Boolean).join(', ')],
    ['gpu', g ? [[g.vendor, g.device].filter(Boolean).join(' '), g.driver ? `(${g.driver})` : ''].filter(Boolean).join(' ') : ''],
    ['memory', memGB != null ? `${memGB} GB` : ''],
    ['arch', (s.cpu && s.cpu.arch) || ''],
    ['lordos', rep && rep.lordos],
    ['kernel', (s.software && s.software.kernel) || ''],
    ['live', rep && rep.live ? 'yes' : 'no'],
    ['boot', bootText(rep)],
    ['bench', rep && rep.bench ? rep.bench.mode : ''],
    ['compat', ((rep && rep.compatibility) || []).filter((c) => c.state !== 'absent').map((c) => `${c.id}=${c.state}`).join(' ')],
  ];
  return rows.map(([k, v]) => [k, oneLine(v, 120)]).filter(([, v]) => v);
}

export function issueTitle(rep) {
  const name = machineLabel(rep) || 'unknown machine';
  return oneLine(`Hardware report: ${name} (LordOS ${rep && rep.lordos ? rep.lordos : '?'}${rep && rep.live ? ', live' : ''})`, TITLE_MAX);
}

export function summaryText(rep) {
  const t = summaryRows(rep).map(([k, v]) => `${k}: ${v}`).join('\n');
  return [...t].length > SUMMARY_MAX ? [...t].slice(0, SUMMARY_MAX).join('') : t;
}

// GitHub's issue forms take a field's value from the query string by its id (the template's "summary" and
// "lordos" fields); the title too.
export function issueUrl(rep) {
  const q = [
    ['template', ISSUE_TEMPLATE],
    ['title', issueTitle(rep)],
    ['lordos', String((rep && rep.lordos) || '')],
    ['summary', summaryText(rep)],
  ].map(([k, v]) => `${k}=${encodeURIComponent(v)}`).join('&');
  return `${REPO_URL}/issues/new?${q}`;
}

// ---- the files ----

// lordos-hwreport-<date>.json and .html in the given folder, or -2, -3… when either name is taken.
export function shareFileNames(date, exists) {
  const base = `lordos-hwreport-${/^\d{4}-\d{2}-\d{2}$/.test(date || '') ? date : 'today'}`;
  for (let i = 1; i < 100; i++) {
    const b = `${base}${i > 1 ? `-${i}` : ''}`;
    if (!exists(`${b}.json`) && !exists(`${b}.html`)) return { json: `${b}.json`, html: `${b}.html` };
  }
  return null;
}

export const shareJson = (rep) => `${JSON.stringify(rep, null, 2)}\n`;

const esc = (s) => String(s == null ? '' : s).replace(/&/g, '&amp;').replace(/</g, '&lt;').replace(/>/g, '&gt;').replace(/"/g, '&quot;').replace(/'/g, '&#39;');

// The page the browser opens: plain words about what is shared and what never is, the summary, the steps and
// one link to the form. Self-contained (no scripts, no outside files), light and dark, right to left when the
// language is. `text`: the page's strings, already translated (sysinfo.share.page.*); `rows`: [label, value].
export function sharePage({ text, rows, url, jsonName, lang = 'en', rtl = false }) {
  const T = (k) => esc(text[k] || '');
  const li = (k) => `<li>${T(k)}</li>`;
  const tr = rows.map(([k, v]) => `<tr><th scope="row">${esc(k)}</th><td>${esc(v)}</td></tr>`).join('\n');
  const steps = ['step1', 'step2', 'step3', 'step4'].map((k) => `<li>${esc(String(text[k] || '').replace('{file}', jsonName))}</li>`).join('\n');
  return `<!doctype html>
<html lang="${esc(lang)}" dir="${rtl ? 'rtl' : 'ltr'}">
<head>
<meta charset="utf-8">
<meta name="viewport" content="width=device-width, initial-scale=1">
<meta name="referrer" content="no-referrer">
<meta http-equiv="Content-Security-Policy" content="default-src 'none'; style-src 'unsafe-inline'">
<title>${T('title')}</title>
<style>
:root { --bg: #f6f5fb; --card: #fff; --text: #1d1b26; --dim: #5b5869; --line: #e2dfec; --accent: #5b3fd6; --accent-text: #fff; }
@media (prefers-color-scheme: dark) { :root { --bg: #15131c; --card: #1f1c29; --text: #f1eff8; --dim: #aaa6bb; --line: #34304a; --accent: #9b85ff; --accent-text: #15131c; } }
* { box-sizing: border-box; }
body { margin: 0; background: var(--bg); color: var(--text); font: 16px/1.55 system-ui, sans-serif; }
main { max-width: 760px; margin: 0 auto; padding: 32px 16px 48px; }
h1 { font-size: 1.6rem; line-height: 1.25; margin: 0 0 8px; }
h2 { font-size: 1.1rem; margin: 28px 0 8px; }
p, li { color: var(--text); }
.lead { color: var(--dim); margin: 0 0 20px; }
.card { background: var(--card); border: 1px solid var(--line); border-radius: 14px; padding: 16px 20px; }
table { width: 100%; border-collapse: collapse; }
th, td { text-align: start; vertical-align: top; padding: 6px 0; border-bottom: 1px solid var(--line); overflow-wrap: anywhere; }
th { color: var(--dim); font-weight: 600; width: 34%; padding-inline-end: 12px; }
tr:last-child th, tr:last-child td { border-bottom: 0; }
.go { display: inline-block; margin: 8px 0 4px; padding: 12px 20px; border-radius: 12px; background: var(--accent); color: var(--accent-text); font-weight: 700; text-decoration: none; }
.go:focus-visible { outline: 3px solid var(--text); outline-offset: 3px; }
code { font-family: ui-monospace, monospace; font-size: 0.92em; overflow-wrap: anywhere; }
.small { color: var(--dim); font-size: 0.9rem; }
</style>
</head>
<body>
<main>
<h1>${T('title')}</h1>
<p class="lead">${T('lead')}</p>
<div class="card">
<h2 style="margin-top:0">${T('summaryHead')}</h2>
<table>
${tr}
</table>
</div>
<h2>${T('sharedHead')}</h2>
<ul>${['shared1', 'shared2', 'shared3', 'shared4'].map(li).join('')}</ul>
<h2>${T('neverHead')}</h2>
<ul>${['never1', 'never2', 'never3'].map(li).join('')}</ul>
<h2>${T('stepsHead')}</h2>
<ol>
${steps}
</ol>
<p><a class="go" href="${esc(url)}" rel="noopener noreferrer">${T('open')}</a></p>
<p class="small">${T('fileLabel')} <code>${esc(jsonName)}</code></p>
<p class="small">${T('footer')}</p>
</main>
</body>
</html>
`;
}
