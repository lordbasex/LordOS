// Settings → General (machine and software details) and Updates (Debian and LordOS).
import {
  View, Text, Button, useTheme, useLocale, useSystemInfo, useServerStats, lordosVersion, screen, formatBytes, formatDuration, formatNumber,
  admin, wm, LordOSLogo, LordOSWordmark, spacing, typeScale, standaloneDesktop,
} from '@lordos/sdk';
import { useState, version as reactVersion } from 'react';
import { Stack, Section, ValueRow, Note, Message, Actions } from './ui.jsx';

// Card with "key … value" rows (kept for other pages that list details).
export function Card({ title, rows }) {
  return (
    <Section title={title}>
      {rows.filter(Boolean).map(([k, v]) => <ValueRow key={k} label={k} value={v} />)}
    </Section>
  );
}

export function General() {
  const t = useTheme();
  const { t: tr } = useLocale();
  const info = useSystemInfo(5000);
  const st = useServerStats(3000);
  const cpu = info.cpu.replace(/\(R\)|\(TM\)|CPU|Processor/g, '').replace(/\s+/g, ' ').trim();
  const ver = lordosVersion(info);
  return (
    <Stack>
      <View style={{ alignItems: 'center', gap: spacing.xs, paddingTop: spacing.xs }}>
        <LordOSLogo size={72} />
        <View style={{ marginTop: spacing.s }}><LordOSWordmark size={typeScale.largeTitle} crown={false} /></View>
        <Text style={{ fontSize: typeScale.callout, color: t.textDim }}>{ver.channel ? tr('about.versionChannel', { v: ver.version, channel: ver.channel }) : tr('about.version', { v: ver.version })}</Text>
        <Text style={{ fontSize: typeScale.caption, color: t.textDim }}>{`${info.hostname} · ${info.os}`}</Text>
      </View>
      <Card title={tr('settings.general.machine')} rows={[
        [tr('settings.general.name'), info.hostname],
        [tr('about.cpu'), tr('about.cpuValue', { cpu: cpu.slice(0, 40) || tr('about.cpuUnknown'), count: info.cores })],
        [tr('about.memory'), tr('shell.cc.memDetail', { used: formatBytes((info.memTotalKB - info.memAvailableKB) * 1024), total: formatBytes(info.memTotalKB * 1024) })],
        info.diskTotal ? [tr('settings.general.storage'), tr('settings.general.storageValue', { free: formatBytes(info.diskFree), total: formatBytes(info.diskTotal) })] : null,
        [tr('about.display'), `${screen.width} × ${screen.height}`],
        [tr('about.uptime'), formatDuration(info.uptime)],
      ]} />
      <Card title={tr('settings.general.software')} rows={[
        ['LordOS', ver.channel ? `${ver.version} ${ver.channel}` : ver.version],
        ver.build ? [tr('about.buildLabel'), ver.build] : null,
        [tr('about.os'), info.os],
        [tr('about.kernel'), `${info.kernel} (${info.arch})`],
        [tr('about.server'), 'lord-server (C · DRM/KMS · evdev · Wayland)'],
        [tr('about.engine'), `QuickJS + React ${reactVersion}`],
        [tr('about.ram'), `${formatNumber(st.rssKB / 1024, 1)} MB (JS ${formatNumber(st.jsKB / 1024, 1)} MB)`],
      ]} />
    </Stack>
  );
}

export function Updates() {
  const { t: tr, formatDate } = useLocale();
  const [busy, setBusy] = useState(false);
  const [msg, setMsg] = useState(null);
  const pkg = typeof lord.pkg !== 'undefined' && lord.pkg.available();
  const ups = pkg ? lord.pkg.upgrades() : [];
  const at = pkg ? lord.pkg.lastUpdate() : 0;
  if (standaloneDesktop) return (
    <Stack>
      <Note>{tr('store.host.description')}</Note>
      <Actions><Button primary label={tr('settings.updates.open')} onClick={() => wm.open('store', { args: { page: 'updates' } })} /></Actions>
    </Stack>
  );
  const check = () => {
    setBusy(true);
    admin.run('pkg-update').then(() => setMsg({ ok: true, text: tr('settings.updates.checked') }), (e) => setMsg({ ok: false, text: e.message }))
      .finally(() => setBusy(false));
  };
  return (
    <Stack>
      <Section footer={tr('settings.updates.hint')}>
        <ValueRow label={tr('settings.updates.debian')} value={ups.length ? tr('settings.updates.available', { count: ups.length }) : tr('settings.updates.none')} />
        <ValueRow label={tr('settings.updates.last')} value={at ? formatDate(new Date(at * 1000), 'long') : '—'} />
      </Section>
      <Actions>
        <Button label={busy ? tr('settings.updates.checking') : tr('settings.updates.check')} onClick={() => !busy && admin.available() && check()} />
        <Button primary label={tr('settings.updates.open')} onClick={() => wm.open('store', { args: { page: 'updates' } })} />
      </Actions>
      <Message msg={msg} />
      {!admin.available() && <Note tone="warn">{tr(standaloneDesktop ? 'settings.host.adminUnavailable' : 'settings.admin.unavailable')}</Note>}
    </Stack>
  );
}
