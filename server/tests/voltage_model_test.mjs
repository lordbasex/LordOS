// voltage_model_test.mjs — Monitor → Voltage's model (apps/monitor/voltmodel.js): the rpi-pmic line, the power state,
// the sources, the trace and the power estimate. Run by tests/run_suites.sh (needs node).
import { LOW_5V, parsePmic, boardWatts, powerState, voltSources, primaryRail, traceRange, afterBoot } from '../../apps/monitor/voltmodel.js';

let n = 0, bad = 0;
const eq = (what, got, want) => {
  n++;
  const g = JSON.stringify(got), w = JSON.stringify(want);
  if (g !== w) { bad++; console.log(`✗ ${what}\n    got  ${g}\n    want ${w}`); }
};

// ---- the action's line ----
const line = 'uv 0 -1 EXT5V=5187140,-1 VDD_CORE=828424,803700 3V3_SYS=3302609,121015 BATT=0,-1';
const p = parsePmic(line);
eq('parsed', [p.count, p.lastS, p.input, p.rails.length], [0, -1, 5.18714, 4]);
eq('a rail', p.rails[1], { id: 'VDD_CORE', v: 0.828424, a: 0.8037 });
eq('no current', p.rails[0].a, null);
eq('alarms logged', parsePmic('uv 3 7265 EXT5V=4700000,-1').lastS, 7265);
for (const badLine of ['', null, 'uv 0 -1', 'uv x -1 EXT5V=1,-1', 'uv 0 -1 EXT5V=1', 'uv 0 -1 EVIL=1,-1', 'uv 0 -1 VDD_CORE=1,1', 'uv 0 -1 EXT5V=1,1 EXT5V=1,1',
  'uv 0 -1 EXT5V=31000000,-1', 'uv 0 -1 EXT5V=5000000,31000000', 'uv 0 -1 EXT5V=1.5,-1', ' uv 0 -1 EXT5V=1,-1', 'uv 0 -1 EXT5V=1,-1 ', 'uv 0 -1 EXT5V=1,-1\nx'])
  eq(`refused: ${badLine}`, parsePmic(badLine), null);

// ---- states ----
eq('threshold', LOW_5V, 4.8);
eq('normal', powerState({ alarm: 0, input: 5.18, count: 0 }), 'normal');
eq('normal, alarm only', powerState({ alarm: 0 }), 'normal');
eq('the threshold itself is not low', powerState({ alarm: 0, input: 4.8, count: 0 }), 'normal');
eq('low', powerState({ alarm: 0, input: 4.79, count: 0 }), 'low');
eq('under-voltage is the alarm', powerState({ alarm: 1, input: 5.1, count: 0 }), 'under');
eq('the alarm wins over low', powerState({ alarm: 1, input: 4.5, count: 1 }), 'under');
eq('low wins over the past', powerState({ alarm: 0, input: 4.7, count: 2 }), 'low');
eq('happened: kernel log', powerState({ alarm: 0, input: 5.1, count: 2 }), 'happened');
eq('happened: firmware bit', powerState({ alarm: 0, sinceBoot: 1 }), 'happened');
eq('happened: seen here', powerState({ alarm: 0, input: 5.1, count: -1, seen: true }), 'happened');
eq('unknown log is not an event', powerState({ alarm: 0, input: 5.1, count: -1 }), 'normal');
eq('nothing to judge', powerState({}), null);
eq('a laptop has no state', powerState({ alarm: -1, input: null, count: null }), null);

// ---- power ----
const near = (what, got, want) => { n++; if (Math.abs(got - want) > 1e-6) { bad++; console.log(`✗ ${what}: ${got}`); } };
near('watts', boardWatts(p.rails), 0.828424 * 0.8037 + 3.302609 * 0.121015);
eq('one rail is no estimate', boardWatts([{ v: 1, a: 1 }, { v: 5, a: null }]), null);
eq('no rails', boardWatts(undefined), null);

// ---- sources ----
eq('a virtual machine', voltSources({ board: false, alarm: -1, chips: [], supplies: [] }, null), []);
const pi = voltSources({ board: true, alarm: 0, chips: [{ dev: 'hwmon1', name: 'rp1_adc', in: [{ i: 1, v: 2.017, label: '' }] }], supplies: [] }, p);
eq('pi: board first, no coin cell', [pi.map((s) => s.kind), pi[0].rails.map((r) => r.rail)], [['board', 'chip'], ['EXT5V', 'VDD_CORE', '3V3_SYS']]);
eq('pi: chip inputs are raw', [pi[1].raw, pi[1].rails[0].name, pi[1].rails[0].raw], [true, 'in1', true]);
eq('pi: trace', primaryRail(pi).rail, 'EXT5V');
eq('coin cell shown when it reads', voltSources({}, parsePmic('uv 0 -1 EXT5V=5000000,-1 BATT=3010000,-1'))[0].rails.length, 2);
const laptop = voltSources({ chips: [], supplies: [{ name: 'AC', battery: false, v: 20, min: null }, { name: 'BAT0', battery: true, v: 11.8, min: 11.4 }] }, null);
eq('laptop: battery traced', primaryRail(laptop).key, 'BAT0');
eq('laptop: min', laptop[0].rails[1].min, 11.4);
eq('adapter traced with no battery', primaryRail(voltSources({ supplies: [{ name: 'AC', battery: false, v: 19.5 }] }, null)).key, 'AC');
const desk = voltSources({ chips: [{ dev: 'hwmon2', name: 'nct6798', in: [{ i: 1, v: 1.016, label: '' }, { i: 0, v: 1.03, label: 'Vcore' }] }] }, null);
eq('desktop: labelled traced', primaryRail(desk).label, 'Vcore');
eq('raw alone: no trace', primaryRail(voltSources({ chips: [{ dev: 'h', name: 'x', in: [{ i: 1, v: 1, label: '' }] }] }, null)), null);

// ---- trace ----
const r = traceRange([5.18, 5.19], 4.8);
eq('threshold in sight', r.floor < 4.8 && r.floor + r.span > 5.19, true);
eq('flat values still have a window', traceRange([1, 1]).span > 0, true);
eq('empty', traceRange([]), { floor: 0, span: 1 });
eq('after boot', afterBoot(7265), { h: 2, m: 1 });

console.log(`voltage-model-test: ${n} checks, ${bad} failed`);
if (bad) process.exit(1);
