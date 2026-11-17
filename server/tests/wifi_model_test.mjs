// wifi_model_test.mjs — Settings → Network → Wi-Fi's lists (apps/settings/wifimodel.js, GitHub #167).
//   node server/tests/wifi_model_test.mjs
import path from 'node:path';
import { fileURLToPath } from 'node:url';
const here = path.dirname(fileURLToPath(import.meta.url));
const { wifiRows, markCurrent, currentOf, savedRows } = await import(path.join(here, '../../apps/settings/wifimodel.js'));
const { wifiBars } = await import(path.join(here, '../../sdk/menubarmodel.js'));

let n = 0, bad = 0;
const eq = (name, got, want) => {
  n++;
  if (JSON.stringify(got) !== JSON.stringify(want)) { bad++; console.log(`✗ ${name}: ${JSON.stringify(got)} ≠ ${JSON.stringify(want)}`); }
};
const net = (ssid, signal, extra) => ({ ssid, bssid: `b-${ssid}`, signal, security: 'wpa2', known: false, current: false, ...extra });

// the connected one first, whatever its signal; the rest strongest first
{
  const rows = wifiRows([net('Far', -80), net('Home', -82, { current: true, known: true }), net('Near', -40), net('Mid', -60)]);
  eq('connected first, then by signal', rows.map((r) => r.ssid), ['Home', 'Near', 'Mid', 'Far']);
  eq('connected one is current', currentOf(rows).ssid, 'Home');
}
eq('nothing connected: only by signal', wifiRows([net('A', -70), net('B', -50)]).map((r) => r.ssid), ['B', 'A']);
eq('two marked: only the first stays connected', wifiRows([net('A', -70, { current: true }), net('B', -50, { current: true })]).map((r) => [r.ssid, r.current]),
  [['A', true], ['B', false]]);
eq('an older lord-server (no "current"): by signal', wifiRows([{ ssid: 'A', signal: -70 }, { ssid: 'B', signal: -50 }]).map((r) => r.ssid), ['B', 'A']);
eq('no signal reading goes last', wifiRows([{ ssid: 'A' }, { ssid: 'B', signal: -90 }]).map((r) => r.ssid), ['B', 'A']);
eq('junk is dropped', wifiRows([null, { signal: 3 }, net('A', -50)]).map((r) => r.ssid), ['A']);
eq('not a list', wifiRows(undefined), []);
eq('no connected network', currentOf(wifiRows([net('A', -50)])), null);

// events: "net wifi <if> connected <ssid>" / "disconnected"
{
  const rows = wifiRows([net('Home', -82, { current: true }), net('Near', -40)]);
  const moved = markCurrent(rows, 'Near');
  eq('connected to another: it moves first', moved.map((r) => [r.ssid, r.current]), [['Near', true], ['Home', false]]);
  const off = markCurrent(moved, null);
  eq('disconnected: none is current', off.map((r) => [r.ssid, r.current]), [['Near', false], ['Home', false]]);
  eq('an SSID the scan has not seen: none is current', markCurrent(rows, 'Elsewhere').some((r) => r.current), false);
  eq('an empty SSID is no network', markCurrent(rows, '').some((r) => r.current), false);
}

// saved networks: connected, then in range, then the rest, each by name
{
  const found = wifiRows([net('Cafe', -60), net('Home', -70, { current: true })]);
  const known = [{ ssid: 'Office', security: 'psk' }, { ssid: 'Cafe', security: 'psk' }, { ssid: 'Home', security: 'psk' }, { ssid: 'Airport', security: 'open' }, { ssid: '' }, null];
  const rows = savedRows(known, found);
  eq('saved: order', rows.map((r) => r.ssid), ['Home', 'Cafe', 'Airport', 'Office']);
  eq('saved: flags', rows.map((r) => [r.current, r.inRange]), [[true, true], [false, true], [false, false], [false, false]]);
  eq('saved: keeps the security', rows[2].security, 'open');
  eq('saved: before any scan', savedRows([{ ssid: 'B' }, { ssid: 'A' }], []).map((r) => [r.ssid, r.inRange, r.current]), [['A', false, false], ['B', false, false]]);
}

// the bars are the Control Center's (sdk/menubarmodel.js wifiBars)
eq('bars', [-40, -60, -70, -85, undefined].map(wifiBars), [4, 3, 2, 1, 4]);

console.log(`wifi-model-test: ${n - bad}/${n} ok`);
process.exit(bad ? 1 : 0);
