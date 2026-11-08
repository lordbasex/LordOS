// btpowermodel.js — Bluetooth on or off as the user left it (Settings → Bluetooth, the Control Center,
// Quick Settings), pure logic for hardware.jsx (tests: server/tests/bt_power_model_test.mjs).
//
// BlueZ doesn't keep an adapter's power across restarts: with its default AutoEnable=true every adapter
// is powered when bluetoothd finds it (at boot, a dongle plugged in, bluetoothd restarted). That is what
// lets a mouse or keyboard reconnect at the login screen, so it stays (issue #75); the session turns the
// adapter off again when the user's last choice was off. Once per appearance of the adapter: turned on
// later by other means, it stays on.
//
// state: lord.bt.state(); wantOff: settings.bluetoothOff; done: the adapter path already handled (or null).
// Returns { off: power it off now, done: the new value of done }.
export function btPowerAction(state, wantOff, done) {
  const a = state && state.available ? state.adapter : null;
  if (!a || !a.path) return { off: false, done: null };     // gone: handled again when it comes back
  if (done === a.path) return { off: false, done };
  if (!wantOff) return { off: false, done: a.path };
  if (!a.powered) return { off: false, done };               // not powered (yet): AutoEnable may still do it
  return { off: true, done: a.path };
}
