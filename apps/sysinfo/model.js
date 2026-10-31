// model.js — System Information's model (docs/BENCHMARK.md, "0.8.5 — System Information"): the GPU
// as separate layers and the Compatibility panel, worked out from lord.sysinfo.report(). Pure
// functions with no UI and no SDK, tested by server/tests/sysinfo_model_test.mjs. States and notes
// are codes; the app turns them into text with t('sysinfo.…').
//
// Layer states: 'yes' (available), 'no' (not available), 'unchecked' (not checked in this
// version). Compatibility states: 'works', 'limits' (works, with a limit the note names), 'no'
// (doesn't work), 'absent' (no such device), 'unchecked'.

// Why this session composes on the CPU, from lord_composition() and gpu_info()'s outcome.
export function compositionReason(session) {
  const s = session || {};
  if (s.composition === 'gpu') return null;
  if (s.composition && s.composition !== 'gpu-off') return s.composition;      // headless, no-display, rotated, capture
  return s.gpuInit || 'unknown';            // env-off, no-3d, no-gbm, no-egl, no-config, no-context, software, shaders, not-built
}

// The layers of one card: what the kernel shows of it.
export function cardLayers(gpu) {
  const g = gpu || {};
  return [
    { id: 'device', state: 'yes', vars: { name: [g.vendor, g.device].filter(Boolean).join(' ') || `${g.vendorId || '?'}:${g.deviceId || '?'}` } },
    { id: 'driver', state: g.driver ? 'yes' : 'no', vars: { driver: g.driver || '' } },
    { id: 'kms', state: g.connectors > 0 ? 'yes' : 'no', note: g.connectors > 0 ? null : 'renderOnly',
      vars: { connected: g.connected || 0, connectors: g.connectors || 0 } },
    { id: 'render', state: g.renderNode ? 'yes' : 'no' },
  ];
}

// The layers of this session: the EGL context, composition, scanout, and what 0.8.5 doesn't check.
export function sessionLayers(session) {
  const s = session || {};
  const on = s.composition === 'gpu';
  const reason = compositionReason(s);
  const soft = /llvmpipe|softpipe|swrast/i.test(s.renderer || '');
  return [
    { id: 'egl', state: on ? 'yes' : 'no', note: on ? (soft ? 'software' : null) : reason === 'software' ? 'softwareRefused' : null,
      vars: { renderer: s.renderer || '' } },
    { id: 'composition', state: on ? 'yes' : 'no', note: on ? null : reason },
    { id: 'scanout', state: on ? 'yes' : 'no', vars: { count: s.scanouts || 0 }, note: on ? (s.scanouts ? 'scanoutSeen' : 'scanoutNone') : null },
    { id: 'dmabuf', state: on && s.dmabuf ? 'yes' : 'no' },
    { id: 'decode', state: 'unchecked' },
    { id: 'encode', state: 'unchecked' },
    { id: 'vulkan', state: 'unchecked' },
  ];
}

// The Compatibility panel: one row per component, from this machine's detection only (0.8.5).
export function compatibility(r) {
  const rep = r || {};
  const out = [];
  const add = (id, state, note = null, vars = {}) => out.push({ id, state, note, vars });
  const s = rep.session || {};
  const gpus = rep.gpus || [];

  if (s.composition === 'gpu') add('gpu', 'works', 'gpuComposes', { renderer: s.renderer || '' });
  else if (gpus.length) add('gpu', 'limits', `cpu.${compositionReason(s)}`);
  else add('gpu', 'limits', 'noGpu');

  const displays = rep.displays || [];
  if (displays.length) {
    const noEdid = displays.filter((d) => !d.nativeWidth).length;
    add('display', noEdid ? 'limits' : 'works', noEdid ? 'noEdid' : null, { count: displays.length });
  } else add('display', s.composition === 'headless' ? 'unchecked' : 'absent', s.composition === 'headless' ? 'headless' : null);

  const disks = rep.storage || [];
  add('storage', disks.length ? 'works' : 'absent', null, { count: disks.length });

  const nets = (rep.network || []).filter((n) => n.kind === 'ethernet' || n.kind === 'wifi');
  if (!nets.length) add('network', 'absent');
  else add('network', nets.some((n) => n.up) ? 'works' : 'limits', nets.some((n) => n.up) ? null : 'notConnected');

  const wifi = (rep.network || []).filter((n) => n.kind === 'wifi');
  if (!wifi.length) add('wifi', 'absent');
  else add('wifi', wifi.every((n) => n.driver) ? 'works' : 'limits', wifi.every((n) => n.driver) ? null : 'noDriver', { driver: wifi[0].driver || '' });

  const bt = rep.bluetooth || [];
  if (!bt.length) add('bluetooth', 'absent');
  else add('bluetooth', bt[0].driver ? 'works' : 'limits', bt[0].driver ? null : 'noDriver', { driver: bt[0].driver || '' });

  add('audio', (rep.audio || []).length ? 'works' : 'absent', null, { count: (rep.audio || []).length });

  const bat = (rep.power || []).find((p) => p.kind === 'battery');
  if (!bat) add('power', 'absent');
  else if (bat.health != null && bat.health < 70) add('power', 'limits', 'batteryWorn', { health: bat.health });
  else add('power', 'works', bat.health != null ? 'batteryHealth' : null, { health: bat.health });

  // isolation: Landlock (null = not known, e.g. a fixture), the BPF LSM and the unified cgroup tree
  const sw = rep.software || {};
  const ll = sw.landlockAbi;
  const missing = [ll === 0 ? 'landlock' : null, sw.bpfLsm ? null : 'bpfLsm', sw.cgroup2 ? null : 'cgroup2'].filter(Boolean);
  if (ll == null && !missing.length) add('isolation', 'works');
  else add('isolation', missing.length ? 'limits' : 'works', missing.length ? `missing.${missing[0]}` : null, { abi: ll || 0 });

  add('suspend', 'unchecked');
  return out;
}

// The Hardware tab's sections that this machine has something for.
export function hardwareSections(r) {
  const rep = r || {};
  const has = {
    computer: true, cpu: true, memory: true,
    graphics: (rep.gpus || []).length > 0 || !!rep.session,
    displays: (rep.displays || []).length > 0,
    storage: (rep.storage || []).length > 0,
    network: (rep.network || []).length > 0,
    audio: (rep.audio || []).length > 0,
    bluetooth: (rep.bluetooth || []).length > 0,
    peripherals: true,                  // read on the page itself, from this machine, never from the report
    power: (rep.power || []).length > 0,
    thermal: (rep.thermal || []).length > 0,
    pci: (rep.pci || []).length > 0,
    usb: (rep.usb || []).length > 0,
    raspberryPi: !!rep.raspberryPi,
    software: true,
  };
  return Object.keys(has).filter((k) => has[k]);
}

// Raspberry Pi get_throttled bits: what happened now and since boot.
export function throttleFlags(v) {
  if (v == null || v < 0) return null;
  const bit = (n) => (v >> n) & 1;
  return {
    underVoltageNow: !!bit(0), cappedNow: !!bit(1), throttledNow: !!bit(2), softLimitNow: !!bit(3),
    underVoltage: !!bit(16), capped: !!bit(17), throttled: !!bit(18), softLimit: !!bit(19),
  };
}

// The Raspberry Pi section's power and throttling rows, as locale keys under sysinfo.pi (null: the kernel
// doesn't tell, so the row is left out). `throttled` is get_throttled (older kernels only); `underVoltageAlarm`
// rpi_volt's alarm, the only one the LordOS kernel has. rpi_volt clears the sticky bits every 2 s, so with it
// they mean "in the last few seconds", never "since boot".
export function piStatus(pi) {
  if (!pi) return { power: null, throttling: null };
  const f = throttleFlags(pi.throttled);
  const alarm = typeof pi.underVoltageAlarm === 'boolean' ? pi.underVoltageAlarm : null;
  const recent = alarm !== null;
  let power = null;
  let throttling = null;
  if (f) {
    power = f.underVoltageNow ? 'underVoltageNow' : f.underVoltage ? (recent ? 'underVoltageRecent' : 'underVoltageSeen')
      : recent ? 'powerOkRecent' : 'powerOk';
    throttling = f.throttledNow || f.cappedNow || f.softLimitNow ? 'throttledNow'
      : f.throttled || f.capped || f.softLimit ? (recent ? 'throttledRecent' : 'throttledSeen')
        : recent ? 'throttleOkRecent' : 'throttleOk';
  } else if (recent) {
    power = alarm ? 'underVoltageRecent' : 'powerOkRecent';
  }
  return { power, throttling };
}

// Bluetooth's kind of device, from BlueZ's icon name.
const BT_KINDS = [[/keyboard/, 'keyboard'], [/mouse/, 'mouse'], [/tablet/, 'tablet'], [/gaming|joystick/, 'gamepad'], [/audio|headset|headphone/, 'audio'], [/phone/, 'phone']];
const BT_ADDRESS = /^[0-9a-f]{2}([:-][0-9a-f]{2}){5}$/i;
const KIND_ORDER = ['keyboard', 'mouse', 'touchpad', 'receiver', 'gamepad', 'tablet', 'input', 'audio'];
const kindRank = (k) => { const i = KIND_ORDER.indexOf(k); return i < 0 ? KIND_ORDER.length : i; };

// The Peripherals section: what lord.sysinfo.peripherals() found on USB and the input buses, plus BlueZ's paired or
// connected devices (lord.bt). Built field by field, so an address or a D-Bus path can't come along; a device that
// only has its address for a name gets no name. On screen only: this is never part of a report.
export function peripherals(local, bt) {
  const out = (Array.isArray(local) ? local : []).map((p) => ({
    name: String(p.name || ''), kind: String(p.kind || 'other'), connection: String(p.connection || 'usb'),
    battery: p.battery >= 0 ? p.battery : null, connected: true,
  }));
  for (const d of (bt && bt.devices) || []) {
    if (!d.paired && !d.connected) continue;
    const kind = (BT_KINDS.find(([re]) => re.test(d.icon || '')) || [null, 'other'])[1];
    out.push({ name: BT_ADDRESS.test(d.name || '') ? '' : String(d.name || ''), kind, connection: 'bluetooth',
      battery: d.battery >= 0 ? d.battery : null, connected: !!d.connected });
  }
  return out.map((p, i) => [p, i]).sort(([a, i], [b, j]) => kindRank(a.kind) - kindRank(b.kind) || i - j).map(([p]) => p);
}

// A firmware date as a Date: DMI's mm/dd/yyyy or the report's yyyy-mm-dd; null for anything else.
export function firmwareDate(s) {
  const us = /^(\d{2})\/(\d{2})\/(\d{4})$/.exec(s || ''), iso = /^(\d{4})-(\d{2})-(\d{2})$/.exec(s || '');
  if (!us && !iso) return null;
  const [y, m, d] = us ? [+us[3], +us[1], +us[2]] : [+iso[1], +iso[2], +iso[3]];
  const dt = new Date(y, m - 1, d, 12);
  return dt.getMonth() === m - 1 && dt.getDate() === d ? dt : null;
}

// A machine's one-line name: vendor and product, the device tree model, or the hypervisor.
export function machineName(r) {
  const m = (r && r.machine) || {};
  return [m.vendor, m.product].filter(Boolean).join(' ') || m.model || '';
}

// The Report tab's file name: lordos-report-<date>.<ext>, or -2, -3… when one with that name exists.
export function reportFileName(date, ext, exists) {
  const base = `lordos-report-${/^\d{4}-\d{2}-\d{2}$/.test(date || '') ? date : 'today'}`;
  for (let i = 1; i < 100; i++) {
    const name = `${base}${i > 1 ? `-${i}` : ''}.${ext}`;
    if (!exists(name)) return name;
  }
  return null;
}

// The JSON a report is saved as: the collector's fields as they are, two-space indent.
export const reportJson = (r) => `${JSON.stringify(r, null, 2)}\n`;

// hw-serial-numbers' lines (hw/hw_admin.c): "serial <system|board|device> <value>". Held by the Hardware tab's
// component while they are shown and nowhere else; anything else is dropped.
export function parseSerialLines(lines) {
  const out = [];
  for (const l of Array.isArray(lines) ? lines : []) {
    const m = /^serial (system|board|device) ([A-Za-z0-9 ._:/-]{1,63})$/.exec(String(l));
    if (m && !out.some((x) => x.kind === m[1])) out.push({ kind: m[1], value: m[2] });
  }
  return out;
}

// hw-memory-modules' lines (hw/hw_admin.c, sysreport/smbios.c): "mem <size_mb>|<type>|<speed>|
// <configured>|<slot>|<bank>|<maker>", one per slot. Anything else is dropped.
export function parseMemoryLines(lines) {
  const out = [];
  for (const l of Array.isArray(lines) ? lines : []) {
    const m = /^mem (\d{1,9})\|([A-Za-z0-9]{0,11})\|(\d{1,6})\|(\d{1,6})\|([\x20-\x7e]{0,31})\|([\x20-\x7e]{0,31})\|([\x20-\x7e]{0,31})$/.exec(String(l));
    if (!m || [m[5], m[6], m[7]].some((x) => x.includes('|'))) continue;
    out.push({ sizeMB: +m[1], type: m[2], speed: +m[3], configured: +m[4], slot: m[5], bank: m[6], maker: m[7] });
    if (out.length >= 64) break;
  }
  return out;
}
