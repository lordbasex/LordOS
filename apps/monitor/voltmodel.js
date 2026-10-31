// voltmodel.js — Monitor → Voltage's model (docs/MONITOR.md, "Voltage"): one list of sources (the board's power chip,
// hwmon chips, batteries and adapters) however the machine reports them, the power state and the trace's range. Pure
// functions with no UI and no SDK, tested by server/tests/voltage_model_test.mjs. Nothing here makes up a number: what
// a machine doesn't report stays out of the list.

export const LOW_5V = 4.8;              // a 5 V input under this is "low"; "under-voltage" is only the board's own alarm
const RAILS = ['EXT5V', 'VDD_CORE', '3V3_SYS', '1V8_SYS', '1V1_SYS', '0V8_SW', '0V8_AON', 'DDR_VDD2', 'DDR_VDDQ', '3V7_WL_SW', '3V3_DAC', '3V3_ADC', 'HDMI', 'BATT'];

// The rpi-pmic action's OK line, checked again here (it comes from the root side, already validated there):
// "uv <count|-1> <last_s|-1> <RAIL>=<uV>,<uA|-1> ...". null if it isn't exactly that.
export function parsePmic(line) {
  const m = /^uv (-1|\d{1,6}) (-1|\d{1,10})((?: [A-Z0-9_]{1,12}=\d{1,8},(?:-1|\d{1,8}))+)$/.exec(String(line || ''));
  if (!m) return null;
  const rails = [];
  for (const part of m[3].trim().split(' ')) {
    const [id, vals] = part.split('=');
    const [uv, ua] = vals.split(',').map(Number);
    if (!RAILS.includes(id) || rails.some((r) => r.id === id) || uv > 30e6 || ua > 30e6) return null;
    rails.push({ id, v: uv / 1e6, a: ua < 0 ? null : ua / 1e6 });
  }
  const input = rails.find((r) => r.id === 'EXT5V');
  if (!input) return null;
  return { count: Number(m[1]), lastS: Number(m[2]), input: input.v, rails };
}

// Sum of volts × amps of the rails that report both: only those, so the board's real draw is higher (USB devices, the
// fan, the regulators' losses). null with fewer than two such rails.
export function boardWatts(rails) {
  const both = (rails || []).filter((r) => r.a != null);
  return both.length < 2 ? null : both.reduce((w, r) => w + r.v * r.a, 0);
}

// 'under' (the board's alarm is on), 'low', 'happened' (an alarm since boot, not now), 'normal', or null with nothing
// to judge by. alarm: 1, 0, -1 (none); sinceBoot: 1, 0, -1; count: alarms in the kernel log, -1 or null unknown; seen:
// the alarm was on while this view was open.
export function powerState({ alarm = -1, sinceBoot = -1, input = null, count = null, seen = false }) {
  if (alarm === 1) return 'under';
  if (input != null && input < LOW_5V) return 'low';
  if (seen || sinceBoot === 1 || count > 0) return 'happened';
  return alarm === 0 || input != null ? 'normal' : null;
}

// Every source this machine has, the board's first; empty ones never appear.
//   { id, kind: 'board' | 'chip' | 'supply', title, raw, rails: [{ key, rail | label | name, v, a, battery, min, raw }] }
export function voltSources(d, pmic) {
  const out = [];
  const board = (pmic?.rails || []).filter((r) => r.id !== 'BATT' || r.v > 0);      // no coin cell: it reads 0
  if (board.length) out.push({ id: 'board', kind: 'board', rails: board.map((r) => ({ key: r.id, rail: r.id, v: r.v, a: r.a })) });
  for (const c of d?.chips || []) {
    const rails = c.in.map((x) => ({ key: `${c.dev}/${x.i}`, label: x.label || null, name: `in${x.i}`, v: x.v, raw: !x.label }));
    if (rails.length) out.push({ id: c.dev, kind: 'chip', title: c.name, raw: rails.some((r) => r.raw), rails });
  }
  const sup = (d?.supplies || []).map((s) => ({ key: s.name, label: s.name, battery: !!s.battery, v: s.v, min: s.min ?? null }));
  if (sup.length) out.push({ id: 'supplies', kind: 'supply', rails: sup });
  return out;
}

// The rail the trace follows: the board's input, a battery, an adapter, else the first labelled sensor. Raw values
// alone get no trace: without the board's scale its shape means little.
export function primaryRail(sources) {
  const all = sources.flatMap((s) => s.rails);
  return all.find((r) => r.rail === 'EXT5V') || all.find((r) => r.battery) || all.find((r) => r.battery === false)
    || all.find((r) => r.label && !r.raw && r.v > 0) || null;
}

// The trace's window: a voltage barely moves, so it is drawn around its values (and `mark`, a threshold to keep in
// sight), not from zero. { floor, span }
export function traceRange(values, mark = null) {
  const all = mark == null ? values : [...values, mark];
  if (!all.length) return { floor: 0, span: 1 };
  const lo = Math.min(...all), hi = Math.max(...all);
  const pad = Math.max((hi - lo) * 0.25, Math.abs(hi) * 0.01, 0.005);
  return { floor: lo - pad, span: hi - lo + 2 * pad };
}

// Seconds since boot → { h, m }
export function afterBoot(s) {
  const n = Math.max(0, Math.floor(Number(s) || 0));
  return { h: Math.floor(n / 3600), m: Math.floor(n / 60) % 60 };
}
