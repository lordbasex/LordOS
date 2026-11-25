// nvreport.js — Monitor → GPU's "Couldn't read NVIDIA data" and its report (docs/MONITOR.md, "NVIDIA"). Pure logic,
// tested by server/tests/nv_report_test.mjs.
//
// lord-server adds raw.nvDiag to lord.monitor.gpu() when sysfs shows a PCI display device of vendor 0x10de
// (src/monitor/nvgpu.c, nvgpu_diag_json): a fixed state name, its number, what lord-nvgpu's sandbox refused (syscall
// numbers, ioctl requests), /sys/module/nvidia/version and the PCI ids and names. Only numbers and fixed names.
//
//   nvFailure(diag, hasData)   null (nothing to say: working, starting, no NVIDIA card), or { state, key, params,
//                              partial } — key the t() key of the reason in plain words; partial: data came but the
//                              sandbox refused some calls
//   nvReportText({ diag, version, kernel, arch }, who)  the plain-text report the "Copy report" button copies, in
//                              English (GitHub issues are), with every field checked against its expected shape and
//                              scrubbed once more (sdk/gpureport.js's scrubReport: home, host and user names). It holds
//                              no user data, no path in the home, no host name and no pid.
import { scrubReport, GPU_TEMPLATE } from '../../sdk/gpureport.js';
import { ISSUE_BASE } from '../../sdk/cloudcheck.js';

export const NV_STATES = ['ok', 'starting', 'noDriver', 'libMissing', 'dlopen', 'symbol', 'init', 'count', 'noDevice', 'sandbox',
  'killed', 'crash', 'exit', 'timeout', 'badMessage', 'noHelper', 'spawn'];

const int = (v) => (Number.isInteger(v) ? v : null);
const stateOf = (d) => (d && NV_STATES.includes(d.state) ? d.state : null);
const DRIVER_RE = /^[a-z0-9_-]{1,23}$/;

// the reason's t() key and parameters
function reason(d) {
  const st = stateOf(d);
  const n = int(d?.detail);
  switch (st) {
    case 'noDriver': {
      const drv = (Array.isArray(d.pci) ? d.pci : []).map((p) => p?.driver).find((x) => typeof x === 'string' && DRIVER_RE.test(x) && x !== 'nvidia');
      return drv ? { key: 'monitor.gpu.nv.r.otherDriver', params: { driver: drv } } : { key: 'monitor.gpu.nv.r.noDriver', params: {} };
    }
    case 'libMissing': return { key: 'monitor.gpu.nv.r.libMissing', params: {} };
    case 'dlopen': case 'symbol': return { key: 'monitor.gpu.nv.r.libBroken', params: {} };
    case 'init': case 'count':
      if (n === 9) return { key: 'monitor.gpu.nv.r.driverNotLoaded', params: {} };
      if (n === 18) return { key: 'monitor.gpu.nv.r.mismatch', params: {} };
      if (n === 4) return { key: 'monitor.gpu.nv.r.noPermission', params: {} };
      return { key: 'monitor.gpu.nv.r.init', params: { code: n == null ? '?' : String(n) } };
    case 'noDevice': return { key: 'monitor.gpu.nv.r.noDevice', params: {} };
    case 'sandbox': return { key: 'monitor.gpu.nv.r.sandbox', params: {} };
    case 'killed': return { key: 'monitor.gpu.nv.r.killed', params: {} };
    case 'crash': case 'exit': case 'badMessage': return { key: 'monitor.gpu.nv.r.crash', params: {} };
    case 'timeout': return { key: 'monitor.gpu.nv.r.timeout', params: {} };
    case 'noHelper': case 'spawn': return { key: 'monitor.gpu.nv.r.noStart', params: {} };
    default: return { key: 'monitor.gpu.nv.r.unknown', params: {} };
  }
}

export function nvFailure(diag, hasData = false) {
  if (!diag || typeof diag !== 'object') return null;
  const st = stateOf(diag);
  const denied = int(diag.denied) || 0;
  if (hasData || st === 'ok') return hasData && denied > 0 ? { state: 'ok', key: 'monitor.gpu.nv.blocked', params: {}, partial: true } : null;
  if (st === 'starting') return null;
  return { state: st || 'unknown', ...reason(diag), partial: false };
}

// ---- the report ----

// NVIDIA's public nvmlReturn_t names (nvml.h), so a number reads without the header at hand
const NVML_ERRORS = {
  1: 'UNINITIALIZED', 2: 'INVALID_ARGUMENT', 3: 'NOT_SUPPORTED', 4: 'NO_PERMISSION', 5: 'ALREADY_INITIALIZED', 6: 'NOT_FOUND',
  7: 'INSUFFICIENT_SIZE', 8: 'INSUFFICIENT_POWER', 9: 'DRIVER_NOT_LOADED', 10: 'TIMEOUT', 11: 'IRQ_ISSUE', 12: 'LIBRARY_NOT_FOUND',
  13: 'FUNCTION_NOT_FOUND', 14: 'CORRUPTED_INFOROM', 15: 'GPU_IS_LOST', 16: 'RESET_REQUIRED', 17: 'OPERATING_SYSTEM',
  18: 'LIB_RM_VERSION_MISMATCH', 19: 'IN_USE', 20: 'MEMORY', 999: 'UNKNOWN',
};
const SIGNALS = { 4: 'SIGILL', 6: 'SIGABRT', 7: 'SIGBUS', 8: 'SIGFPE', 9: 'SIGKILL', 11: 'SIGSEGV', 31: 'SIGSYS' };
const SYMBOLS = { 1: 'nvmlInit_v2', 2: 'nvmlDeviceGetCount_v2', 3: 'nvmlDeviceGetHandleByIndex_v2' };
const WHAT = {
  noDriver: 'no NVIDIA card is bound to the nvidia driver', libMissing: 'libnvidia-ml.so.1 not found in the system folders (or not root-owned)',
  dlopen: 'dlopen of libnvidia-ml.so.1 failed', symbol: 'a required NVML function is missing', init: 'nvmlInit failed',
  count: 'nvmlDeviceGetCount failed', noDevice: 'NVML found no device', sandbox: 'the sandbox could not be applied (exit 70)',
  killed: 'killed by the sandbox (SIGSYS)', crash: 'the helper died of a signal', exit: 'the helper exited', timeout: 'no answer in time',
  badMessage: 'the helper sent a malformed message', noHelper: 'lord-nvgpu is missing or not root-owned',
  spawn: 'lord-nvgpu could not be started', ok: 'working', starting: 'starting',
};

const clean = (v, re, max = 80) => (typeof v === 'string' && v.length <= max && re.test(v) ? v : '');
const SLOT_RE = /^[0-9a-f]{4,8}:[0-9a-f]{2}:[0-9a-f]{2}\.[0-7]$/;
const ID_RE = /^10de:[0-9a-f]{4}$/;
const CLASS_RE = /^03[0-9a-f]{4}$/;
const NAME_RE = /^[\x20-\x7e]{1,159}$/;
const PATHY_RE = /[~@]|\/[A-Za-z._~]/;             // pci.ids names have none: a path, a home, an address
const VER_RE = /^[0-9][0-9A-Za-z.+~_-]{0,39}$/;
const KERNEL_RE = /^[0-9][0-9A-Za-z.+~_-]{0,63}$/;
const ARCH_RE = /^[a-z0-9_]{1,16}$/;
const DRV_VER_RE = /^[0-9][0-9.]{0,30}$/;
const hex = (n) => `0x${(n >>> 0).toString(16).padStart(8, '0')}`;

function detailText(st, n) {
  if (n == null) return '';
  if (st === 'init' || st === 'count') return ` (nvmlReturn ${n}${NVML_ERRORS[n] ? ` NVML_ERROR_${NVML_ERRORS[n]}` : ''})`;
  if (st === 'symbol') return ` (${SYMBOLS[n] || `#${n}`})`;
  if (st === 'crash' || st === 'killed') return ` (signal ${n}${SIGNALS[n] ? ` ${SIGNALS[n]}` : ''})`;
  if (st === 'exit' || st === 'sandbox') return ` (exit code ${n})`;
  if (st === 'spawn') return ` (errno ${n})`;
  return ` (${n})`;
}

export function nvReportText({ diag, version = '', kernel = '', arch = '' } = {}, who = {}) {
  const d = diag && typeof diag === 'object' ? diag : {};
  const st = stateOf(d) || 'unknown';
  const n = int(d.detail);
  const lines = ['LordOS NVIDIA report (Monitor > GPU, lord-nvgpu)', ''];
  lines.push(`LordOS: ${clean(version, VER_RE) || 'unknown'}`);
  lines.push(`Kernel: ${clean(kernel, KERNEL_RE) || 'unknown'}${clean(arch, ARCH_RE) ? ` (${arch})` : ''}`);
  const pci = (Array.isArray(d.pci) ? d.pci : []).slice(0, 8);
  if (!pci.length) lines.push('GPU: none found in sysfs');
  for (const p of pci) {
    const slot = clean(p?.slot, SLOT_RE) || '?';
    const id = clean(p?.id, ID_RE) || '10de:?';
    const nm = clean(p?.name, NAME_RE, 159);
    const name = nm && !PATHY_RE.test(nm) ? nm : '';
    const cls = clean(p?.class, CLASS_RE);
    const drv = clean(p?.driver, DRIVER_RE, 23);
    lines.push(`GPU: ${slot} [${id}]${name ? ` ${name}` : ''}${cls ? ` (class ${cls})` : ''}, driver: ${drv || 'none'}`);
  }
  lines.push(`NVIDIA driver (/sys/module/nvidia/version): ${clean(d.driverVersion, DRV_VER_RE, 31) || 'not readable'}`);
  lines.push(`Helper state: ${st}${detailText(st, n)}${WHAT[st] ? ` - ${WHAT[st]}` : ''}`);
  const denied = int(d.denied) || 0;
  const sys = (Array.isArray(d.syscalls) ? d.syscalls : []).filter((x) => Number.isInteger(x) && x >= 0 && x < 4096).slice(0, 8);
  const ioc = (Array.isArray(d.ioctls) ? d.ioctls : []).filter((x) => Number.isInteger(x) && x > 0 && x <= 0xffffffff).slice(0, 8);
  lines.push(`Sandbox refusals: ${denied}`);
  if (sys.length) lines.push(`Refused syscalls (numbers for the arch above): ${sys.join(', ')}`);
  if (ioc.length) lines.push(`Refused ioctl requests: ${ioc.map((r) => `${hex(r)} (type 0x${((r >>> 8) & 0xff).toString(16).padStart(2, '0')}, nr 0x${(r & 0xff).toString(16).padStart(2, '0')})`).join(', ')}`);
  lines.push('');
  lines.push(`Please open an issue and paste this report: ${ISSUE_BASE}?template=${GPU_TEMPLATE}`);
  return scrubReport(lines.join('\n'), who);
}
