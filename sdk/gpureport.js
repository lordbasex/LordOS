// gpureport.js — Settings → Graphics cards' compatibility report and NVIDIA's experimental status (docs/NVIDIA-PLAN.md).
// Pure logic, tested by server/tests/gpu_report_test.mjs.
//
//   readGpuReport()                        lord.sysinfo.gpuReport() → { text, info } (made and scrubbed in C,
//                                          src/sysreport/gpureport.c), or null where this lord-server lacks it
//   scrubReport(text, { user, host, home }) the text with the person's traces replaced once more (defence in depth: the
//                                          C side already did it): the home → ~, /home/<name> → /home/<user>, MAC
//                                          addresses, UUIDs, e-mail addresses, serial numbers, the user's and host's names
//   gpuIssueUrl({ report, version, gpu, driver }) the GitHub form (.github/ISSUE_TEMPLATE/gpu-report.yml) filled in,
//                                          under ISSUE_URL_MAX characters (whole lines cut from the end, marked)
//   cardSummary(card)                      "NVIDIA Corporation GA106 [GeForce RTX 3060] [10de:2504]"
//   driverSummary(card, info)              "nvidia 560.35.03 (open)", "nouveau (Mesa 25.0.7)", "amdgpu (Mesa …)" or ""
//   isNvidia(card)                         an NVIDIA card (PCI vendor 0x10de), from lord.vm.gpus() or the report's info
//   NVIDIA_ROLES                           the roles an NVIDIA card lists: [['compute', key], ['desktop', key]]
//   nvidiaRoleState(id, card, info)        what Settings says about one: 'planned', 'experimental', 'gpu' or 'cpu'
//
// The report is written in English (GitHub issues are) and holds technical data only. Nothing here sends anything:
// the browser opens the form, and the person reads and submits it there.
import { encodeQuery, ISSUE_BASE, ISSUE_URL_MAX } from './cloudcheck.js';

export const GPU_TEMPLATE = 'gpu-report.yml';
export const NVIDIA = 0x10de;
// AI and compute (CUDA) is planned. The desktop is experimental: the free driver (nouveau) stays the default, NVIDIA's own
// driver is an opt-in from the Store, and with it the community has seen the desktop composed on the GPU (#276, a GTX
// 1070 on 580) and on the CPU (#171, an RTX 3060 on 0.8.8.9, no GL context) (docs/NVIDIA-PLAN.md)
export const NVIDIA_ROLES = [['compute', 'settings.gpus.roleCompute'], ['desktop', 'settings.gpus.roleDesktop']];

// One role of an NVIDIA card as Settings shows it. card: the report's entry for it (reportCard), info: the report's facts.
// The desktop of the card that draws this session shows the session's real composition: 'gpu' composed on the card,
// 'cpu' composed by the processor; any other card's desktop role is 'experimental'.
export function nvidiaRoleState(id, card, info) {
  if (id !== 'desktop') return 'planned';
  const comp = info?.session?.composition;
  if (card?.desktop === 1 && typeof comp === 'string' && comp) return comp === 'gpu' ? 'gpu' : 'cpu';
  return 'experimental';
}

export function readGpuReport() {
  try { return typeof lord.sysinfo?.gpuReport === 'function' ? lord.sysinfo.gpuReport() : null; } catch (e) { return null; }
}

const vendorNum = (c) => (typeof c?.vendor === 'number' ? c.vendor : parseInt(c?.vendorId || '', 16));
export const isNvidia = (c) => vendorNum(c) === NVIDIA;

const esc = (s) => String(s).replace(/[.*+?^${}()|[\]\\]/g, '\\$&');
const usable = (s) => typeof s === 'string' && s.length >= 2 && s.length <= 255 && !['root', 'localhost', 'none', 'unknown', 'yes', 'no', 'on', 'off'].includes(s);

export function scrubReport(text, { user = '', host = '', home = '' } = {}) {
  let s = String(text ?? '');
  const h = typeof home === 'string' && home.length > 1 && home[0] === '/' ? home.replace(/\/+$/, '') : '';
  if (h.length > 1) s = s.replace(new RegExp(`${esc(h)}(?![\\w-])`, 'g'), '~');
  s = s.replace(/\/home\/[^/\s:"']+/g, '/home/<user>');
  s = s.replace(/(?<![\w-])[0-9A-Fa-f]{2}([:-])(?:[0-9A-Fa-f]{2}\1){4}[0-9A-Fa-f]{2}(?![0-9A-Fa-f])/g, '<mac>');
  s = s.replace(/(?<![\w-])[0-9A-Fa-f]{8}-[0-9A-Fa-f]{4}-[0-9A-Fa-f]{4}-[0-9A-Fa-f]{4}-[0-9A-Fa-f]{12}(?![0-9A-Fa-f])/g, '<uuid>');
  s = s.replace(/(?<![\w-])[A-Za-z0-9._%+-]+@[A-Za-z0-9-]+(?:\.[A-Za-z0-9-]+)+/g, '<email>');
  s = s.replace(/(?<![\w-])([Ss][Ee][Rr][Ii][Aa][Ll][A-Za-z_]*[ \t:=]+)[^\s,;]+/g, '$1<hidden>');
  // names as whole words only (a word here: letters, digits, _ and -)
  if (usable(user)) s = s.replace(new RegExp(`(?<![\\w-])${esc(user)}(?![\\w-])`, 'g'), '<user>');
  if (usable(host)) s = s.replace(new RegExp(`(?<![\\w-])${esc(host)}(?![\\w-])`, 'g'), '<host>');
  return s;
}

export function cardSummary(c) {
  if (!c) return '';
  const str = (v) => (typeof v === 'string' ? v : '');
  const name = [str(c.vendor) || str(c.vendorName), str(c.device)].filter(Boolean).join(' ') || 'Unknown graphics card';
  const hex = (v) => (typeof v === 'number' ? ((v >>> 0) & 0xffff).toString(16).padStart(4, '0') : String(v || '?'));
  const vid = c.vendorId ?? (typeof c.vendor === 'number' ? hex(c.vendor) : '');
  const did = c.deviceId ?? (typeof c.device === 'number' ? hex(c.device) : '');
  return vid ? `${name} [${vid}:${did || '?'}]` : name;
}

const MESA_DRIVERS = ['nouveau', 'amdgpu', 'radeon', 'i915', 'xe', 'v3d', 'vc4', 'virtio-pci', 'virtio_gpu'];
export function driverSummary(card, info) {
  const d = String(card?.driver || '');
  if (!d) return '';
  const nv = info?.nvidia;
  if (d === 'nvidia' && nv?.version) return `nvidia ${nv.version}${nv.open === 1 ? ' (open)' : nv.open === 0 ? ' (proprietary)' : ''}`;
  if (MESA_DRIVERS.includes(d) && info?.mesa) return `${d} (Mesa ${info.mesa})`;
  return d;
}

// the report's card in lord.vm.gpus()'s address ("0000:01:00.0"), or null
export function reportCard(info, address) {
  return (info?.cards || []).find((c) => c.slot && c.slot === address) || null;
}

const TRUNC = '\n[truncated: the whole report is in the clipboard]';
export function gpuIssueUrl({ report = '', version = '', gpu = '', driver = '' } = {}) {
  const q = (k, v) => `&${k}=${encodeQuery(v)}`;
  const g = String(gpu || '').slice(0, 120);
  const head = `${ISSUE_BASE}?template=${GPU_TEMPLATE}${q('title', `GPU report: ${g || 'graphics card'}`)}`
    + (version ? q('version', String(version).slice(0, 40)) : '')
    + (g ? q('gpu', g) : '')
    + (driver ? q('driver', String(driver).slice(0, 80)) : '');
  let r = String(report || '');
  if (!r) return head;
  let url = head + q('report', r);
  if (url.length <= ISSUE_URL_MAX) return url;
  const lines = r.split('\n');
  while (lines.length > 1) {
    lines.pop();
    r = lines.join('\n') + TRUNC;
    url = head + q('report', r);
    if (url.length <= ISSUE_URL_MAX) return url;
  }
  return head;      // the head alone is short: a name of 120 characters, a version of 40, a driver of 80
}
