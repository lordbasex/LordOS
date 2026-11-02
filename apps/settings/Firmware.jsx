// Settings → Board firmware (only on a BCM2712 board: Raspberry Pi 5, 500, CM5): the bootloader in
// the board's EEPROM, the newest one the installed rpi-eeprom package carries, and Update. Two closed
// admin actions do the work (server/src/hw/hw_admin.c): rpi-eeprom-status (read-only, anyone) and
// rpi-eeprom-update (administrators), both running only /usr/bin/rpi-eeprom-update with a fixed
// argument list; the reply is "<uptodate|available|pending> <current> <latest> <release>". The
// update is finished by the board itself at the next restart. See docs/SETTINGS.md, "Board firmware".
import { View, Text, useTheme, useLocale, Button, admin, session, useSystemInfo, spacing, typeScale } from '@lordos/sdk';
import { useEffect, useState } from 'react';
import { GroupBox, ValueRow, Note, Stack, StatusDot, Actions, tint } from './ui.jsx';

const PACKAGE = 'rpi-eeprom';
const RELEASES = ['default', 'latest'];

// The helper's OK line, checked again here (it comes from the root side, already validated there).
export function parseEepromStatus(line) {
  const m = /^(uptodate|available|pending) (\d{1,10}) (\d{1,10}) ([a-z]{1,15})$/.exec(String(line || ''));
  if (!m) return null;
  return { state: m[1], current: Number(m[2]), latest: Number(m[3]), release: m[4] };
}

export function FirmwareSettings() {
  const t = useTheme();
  const { t: tr, formatDate } = useLocale();
  const info = useSystemInfo(60000);
  const [st, setSt] = useState(undefined);      // undefined: asking; null: failed (err says why)
  const [err, setErr] = useState(null);
  const [busy, setBusy] = useState('');         // '' | 'check' | 'update' | 'install'
  const isAdmin = !!info.admin && admin.available();

  const check = () => {
    setBusy('check');
    return admin.run('rpi-eeprom-status', '', null, { quiet: true }).then((ok) => {
      const s = parseEepromStatus(ok);
      setSt(s);
      setErr(s ? null : { code: 'hw.eepromFailed', text: tr('errors.hw.eepromFailed') });
    }, (e) => { setSt(null); setErr({ code: e.code, text: e.message }); }).finally(() => setBusy(''));
  };
  useEffect(() => { check(); }, []);

  const update = () => {
    setBusy('update');
    admin.run('rpi-eeprom-update', '', null, { quiet: true }).then((ok) => {
      const s = parseEepromStatus(ok);
      if (s) { setSt(s); setErr(null); } else check();
    }, (e) => setErr({ code: e.code, text: e.message })).finally(() => setBusy(''));
  };

  // rpi-eeprom missing (a board set up by hand): offer it, with the package engine
  const install = () => {
    setBusy('install');
    const go = () => admin.run('pkg-install', PACKAGE);
    go().catch((e) => {
      if (e.code !== 'pkg.noIndex') throw e;
      return admin.run('pkg-update', '').then(go);
    }).then(() => check(), (e) => { setErr({ code: e.code, text: e.message }); setBusy(''); });
  };

  const day = (ts) => (ts ? formatDate(new Date(ts * 1000), 'long') : tr('settings.firmware.unknown'));
  const missing = err?.code === 'hw.noEeprom';
  const canRestart = typeof session.canPower === 'function' && session.canPower();
  const state = st?.state;
  const color = state === 'uptodate' ? tint.ok : state === 'available' ? tint.warn : state === 'pending' ? tint.warn : tint.off;
  const stateText = state ? tr(`settings.firmware.state.${state}`) : busy === 'check' ? tr('settings.firmware.checking') : '—';
  const releaseText = st ? (RELEASES.includes(st.release) ? tr(`settings.firmware.release.${st.release}`) : st.release) : '—';

  return (
    <Stack>
      <GroupBox title={tr('settings.firmware.bootloader')} footer={tr('settings.firmware.hint')}>
        <View style={{ flexDirection: 'row', alignItems: 'center', gap: spacing.s, paddingVertical: spacing.m }}>
          <StatusDot color={color} size={10} />
          <Text style={{ flex: 1, minWidth: 0, fontSize: typeScale.headline, fontWeight: 600, color: t.text, textAlign: 'start' }}>{stateText}</Text>
        </View>
        <ValueRow label={tr('settings.firmware.current')} value={st ? day(st.current) : '—'} />
        <ValueRow label={tr('settings.firmware.latest')} value={st ? day(st.latest) : '—'} />
        <ValueRow label={tr('settings.firmware.releaseLabel')} value={releaseText} />
        <Actions style={{ paddingVertical: spacing.s }}>
          <Button small label={tr('settings.firmware.check')} onClick={check} disabled={!!busy} />
          {state === 'available' && (
            <Button small primary label={busy === 'update' ? tr('settings.firmware.updating') : tr('settings.firmware.update')}
              onClick={update} disabled={!!busy || !isAdmin} />
          )}
          {state === 'pending' && canRestart && <Button small primary label={tr('settings.firmware.restart')} onClick={() => session.restart()} />}
        </Actions>
      </GroupBox>
      {state === 'pending' && <Note tone="warn">{tr(canRestart ? 'settings.firmware.pendingHint' : 'settings.firmware.pendingHow')}</Note>}
      {state === 'available' && !isAdmin && <Note>{tr('settings.firmware.needAdmin')}</Note>}
      {busy === 'update' && <Note>{tr('settings.firmware.updatingHint')}</Note>}
      {missing ? (
        <GroupBox footer={tr('settings.firmware.missingHint')}>
          <ValueRow label={tr('settings.firmware.missing')} value="">
            <Button small label={busy === 'install' ? tr('settings.firmware.installing') : tr('settings.firmware.install')}
              onClick={install} disabled={!!busy || !isAdmin} />
          </ValueRow>
        </GroupBox>
      ) : err ? <Note tone="error">{err.text}</Note> : null}
    </Stack>
  );
}
