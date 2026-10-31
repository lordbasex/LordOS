// Settings → Bluetooth: on/off, my devices (connect, disconnect, forget) and nearby devices
// (pair). BlueZ over the system bus, from lord-server (server/src/hw/bt.c); PIN and passkey
// requests are drawn by the system (sdk/hardware.jsx, BluetoothPrompt). Headphones and speakers
// show up in Sound through PipeWire. See docs/SETTINGS.md, "Bluetooth".
import { View, Text, useTheme, useLocale, Toggle, Button, admin, useBluetooth, onBluetoothResult } from '@lordos/sdk';
import { useEffect, useState } from 'react';
import { GroupBox, SettingRow, PageHeader, Note, Stack } from './ui.jsx';

// Monochrome glyphs (the usual 🎧 🖱 📱 are color emoji on the system fonts).
const GLYPHS = [[/headset|headphones/, '♪'], [/audio/, '♪'], [/keyboard/, '⌨'], [/mouse|tablet/, '◖'], [/gaming|joystick/, '✜'],
  [/phone/, '▯'], [/computer/, '▭'], [/camera/, '◉'], [/printer/, '⎙']];
const glyphOf = (icon) => (GLYPHS.find(([re]) => re.test(icon || '')) || [null, 'ᛒ'])[1];

export function BtIcon({ size = 44 }) {
  return (
    <View style={{ width: size, height: size, borderRadius: size * 0.24, background: 'linear-gradient(180deg, #3a9bff, #0a6cd6)', alignItems: 'center', justifyContent: 'center' }}>
      <Text style={{ fontSize: size * 0.58, fontWeight: 700, color: '#fff', lineHeight: size }}>ᛒ</Text>
    </View>
  );
}

function DeviceIcon({ icon }) {
  const t = useTheme();
  return (
    <View style={{ width: 28, height: 28, borderRadius: 14, backgroundColor: t.control, alignItems: 'center', justifyContent: 'center' }}>
      <Text style={{ fontSize: 14, color: t.text }}>{glyphOf(icon)}</Text>
    </View>
  );
}

function Device({ d, tr }) {
  const t = useTheme();
  const status = d.busy ? tr('prefs.bt.working') : d.connected ? tr('prefs.bt.connected') : tr('prefs.bt.notConnected');
  const battery = d.battery >= 0 ? ` · ${tr('prefs.bt.battery', { pct: d.battery })}` : '';
  if (!d.paired) {
    return (
      <SettingRow icon={<DeviceIcon icon={d.icon} />} label={d.name} hint={d.rssi != null ? tr('prefs.bt.signal', { dbm: d.rssi }) : ''}>
        {d.busy ? <Text style={{ fontSize: 12, color: t.textDim }}>{tr('prefs.bt.pairing')}</Text>
          : <Button small label={tr('prefs.bt.connect')} onClick={() => lord.bt.pair(d.path)} />}
      </SettingRow>
    );
  }
  return (
    <SettingRow icon={<DeviceIcon icon={d.icon} />} label={d.name} hint={`${status}${battery}`}>
      <View style={{ flexDirection: 'row', alignItems: 'center', gap: 6 }}>
        {!d.busy && <Button small label={d.connected ? tr('prefs.bt.disconnect') : tr('prefs.bt.connect')}
          onClick={() => (d.connected ? lord.bt.disconnect(d.path) : lord.bt.connect(d.path))} />}
        {/* MenuButton's menu would sit under the next group (zIndex only orders siblings) */}
        <Button small plain destructive label={tr('prefs.bt.forget')} onClick={() => lord.bt.forget(d.path)} />
      </View>
    </SettingRow>
  );
}

const SEARCH_MS = 40000;

export function BluetoothSettings() {
  const t = useTheme();
  const { t: tr } = useLocale();
  const st = useBluetooth();
  const [err, setErr] = useState('');
  const a = st?.adapter;
  // a search of its own, for a while: the kernel doesn't listen for paired devices coming back while one runs
  const [search, setSearch] = useState(0);
  useEffect(() => {
    if (!a?.powered) return undefined;
    lord.bt.discover(true);
    const stop = setTimeout(() => lord.bt.discover(false), SEARCH_MS);
    return () => { clearTimeout(stop); lord.bt.discover(false); };
  }, [a?.powered, a?.path, search]);
  useEffect(() => onBluetoothResult((r) => {
    if (r.ok || r.op === 'stop') { if (r.ok && r.op !== 'stop') setErr(''); return; }
    const key = /Blocked|rfkill/i.test(r.error) ? 'blocked' : /profile-unavailable|NotAvailable|NotSupported/i.test(r.error) ? 'noProfile'
      : /AuthenticationFailed|AuthenticationRejected|Rejected/i.test(r.error) ? 'authFailed'
      : /Canceled|AuthenticationCanceled/i.test(r.error) ? 'canceled' : /AlreadyExists/i.test(r.error) ? '' : 'failed';
    if (key) setErr(key);
  }), []);
  if (!st || !st.available || !a) {
    return (
      <Stack>
        <PageHeader icon={<BtIcon />} title={tr('prefs.bt.title')} subtitle={tr(st?.available ? 'prefs.bt.noAdapter' : 'prefs.bt.noService')} />
      </Stack>
    );
  }
  const mine = st.devices.filter((d) => d.paired).sort((x, y) => (y.connected - x.connected) || x.name.localeCompare(y.name));
  const near = st.devices.filter((d) => !d.paired && (d.named || d.busy)).sort((x, y) => (y.rssi ?? -200) - (x.rssi ?? -200));
  const togglePower = (on) => {
    setErr('');
    lord.bt.power(on);
  };
  return (
    <Stack>
      <PageHeader icon={<BtIcon />} title={tr('prefs.bt.title')}
        subtitle={a.powered ? tr('prefs.bt.visibleAs', { name: a.name || a.address }) : tr('prefs.bt.offHint')}>
        <Toggle small on={a.powered} onChange={togglePower} />
      </PageHeader>
      {err === 'blocked' ? (
        <GroupBox>
          <SettingRow label={tr('prefs.bt.err.blocked')}>
            <Button small label={tr('prefs.bt.unblock')} onClick={() => admin.run('bt-unblock').then(() => { setErr(''); lord.bt.power(true); }, (e) => setErr(e.message))} />
          </SettingRow>
        </GroupBox>
      ) : err ? <Note tone="warn">{tr(`prefs.bt.err.${err}`) || err}</Note> : null}
      {a.powered && (
        <GroupBox title={tr('prefs.bt.myDevices')}>
          {mine.length ? mine.map((d) => <Device key={d.path} d={d} tr={tr} />) : <SettingRow label={tr('prefs.bt.noDevices')} />}
        </GroupBox>
      )}
      {a.powered && (
        <GroupBox title={a.discovering ? tr('prefs.bt.nearbySearching') : tr('prefs.bt.nearby')} footer={tr('prefs.bt.nearbyHint')}>
          {near.length ? near.map((d) => <Device key={d.path} d={d} tr={tr} />)
            : <SettingRow label={a.discovering ? tr('prefs.bt.searching') : tr('prefs.bt.noneNearby')} />}
        </GroupBox>
      )}
      {a.powered && !a.discovering ? <View style={{ alignItems: 'flex-end' }}><Button small label={tr('prefs.bt.searchAgain')} onClick={() => setSearch((n) => n + 1)} /></View> : null}
      {a.powered ? <Note>{tr('prefs.bt.audioHint')}</Note> : null}
      {!st.agent && a.powered ? <Note tone="warn">{tr('prefs.bt.noAgent')}</Note> : null}
    </Stack>
  );
}
