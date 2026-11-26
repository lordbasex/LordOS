// About LordOS: real system data (/proc) and display server stats.
import { version as reactVersion } from 'react';
import { useEffect, useMemo, useRef, useState } from 'react';
import { View, Text, Button, ScrollView, wm, LordOSLogo, LordOSWordmark, Lumo, screen, lordosVersion, useTheme, useSystemInfo, useServerStats, memUsedKB, formatBytes, formatDuration, formatNumber, typeScale, codeStatus, isSystemApp, t as tr, SkinRegion, useSkinned } from '@lordos/sdk';

export function Icon({ size }) {
  return <LordOSLogo size={size} />;
}

// One line of the spec list, as in a classic "About" window: the label in a trailing-aligned column
// and the value at its side.
function InfoRow({ k, v }) {
  const t = useTheme();
  return (
    <View style={{ flexDirection: 'row', alignItems: 'center', minHeight: 20, gap: 10 }}>
      <Text style={{ width: 118, fontSize: typeScale.callout, color: t.textDim, textAlign: 'end', whiteSpace: 'nowrap', overflow: 'hidden' }}>{k}</Text>
      <View style={{ flex: 1, minWidth: 0, overflow: 'hidden' }}>
        <Text style={{ fontSize: typeScale.callout, fontWeight: 500, color: t.text, whiteSpace: 'nowrap' }}>{v}</Text>
      </View>
    </View>
  );
}

// A small pill: a colored dot and a label (System app, Verified, Development build).
function Badge({ color, label }) {
  const t = useTheme();
  return (
    <View style={{ flexDirection: 'row', alignItems: 'center', gap: 5, paddingHorizontal: 8, paddingVertical: 2, borderRadius: 10,
      backgroundColor: t.mode === 'dark' ? 'rgba(255,255,255,0.08)' : 'rgba(0,0,0,0.05)' }}>
      <View style={{ width: 7, height: 7, borderRadius: 4, backgroundColor: color }} />
      <Text style={{ fontSize: typeScale.caption, color: t.text, whiteSpace: 'nowrap' }}>{label}</Text>
    </View>
  );
}

// The state of LordOS's own code, as lord-server checked it (docs/SIGNING.md, "Verified system code").
const CODE = {
  verified: { color: '#30d158', key: 'about.codeVerified' },
  development: { color: '#ff9f0a', key: 'about.codeDevelopment' },
  invalid: { color: '#ff453a', key: 'about.codeInvalid' },
};

// The height of a box after layout: the content keeps at least the window's height (the credits at the bottom) and
// scrolls in a window shorter than it, such as a tile.
function useHeight(ref) {
  const [h, setH] = useState(0);
  useEffect(() => {
    const read = () => { try { if (ref.current) { const x = Math.round(lord.getLayout(ref.current.id)[3]); setH((o) => (Math.abs(o - x) > 1 ? x : o)); } } catch (e) { /* not laid out yet */ } };
    read();
    const id = setInterval(read, 400);
    return () => clearInterval(id);
  }, []);
  return h;
}

export default function About() {
  const t = useTheme();
  const box = useRef(null);
  const boxH = useHeight(box);
  const info = useSystemInfo(5000);
  const st = useServerStats(2000);
  const cpu = info.cpu.replace(/\(R\)|\(TM\)|CPU|Processor/g, '').replace(/ Rev [\d.]+$/, '').replace(/\s+/g, ' ').trim();
  const ver = lordosVersion(info);
  const code = useMemo(() => codeStatus(), []);
  const caseId = useMemo(() => { try { return lord.fan.caseConfig(); } catch (e) { return null; } }, []);
  const cs = CODE[code.refused?.length ? 'invalid' : code.state] || CODE.development;
  const codeNote = code.state === 'verified' && !code.refused?.length ? tr('about.codeVerifiedNote', { count: code.files })
    : code.state === 'development' ? tr('about.codeDevNote') : null;
  // (0.8.9.6.2) app skins (docs/THEMES-GUIDE.md, "App skins → About"): the banner (the version and the verified-code state)
  // and the facts as label and value rows; the numbers are read by About, the skin only shows them
  const skinned = useSkinned('about');
  const vm = skinned ? {
    app: 'about',
    version: ver.channel ? tr('about.versionChannel', { v: ver.version, channel: ver.channel }) : tr('about.version', { v: ver.version }),
    build: ver.build ? tr('about.build', { b: ver.build }) : '', code: tr(cs.key), codeNote: codeNote || '', tagline: tr('about.tagline'),
    rows: [
      [tr('about.os'), info.os], [tr('about.kernel'), `${info.kernel} (${info.arch})`], [tr('about.host'), tr('about.hostValue', { host: info.hostname, user: info.user })],
      [tr('about.cpu'), tr('about.cpuValue', { cpu: cpu.slice(0, 36) || tr('about.cpuUnknown'), count: info.cores })],
      [tr('about.memory'), tr('shell.cc.memDetail', { used: formatBytes(memUsedKB(info) * 1024), total: formatBytes(info.memTotalKB * 1024) })],
      [tr('about.uptime'), formatDuration(info.uptime)],
    ].map(([label, value]) => ({ label, value: String(value || '') })),
    actions: { systemReport: () => wm.open('sysinfo') },
  } : null;
  return (
    <View ref={box} style={{ flex: 1 }}>
    <ScrollView style={{ flex: 1 }}>
    <View style={{ minHeight: boxH, alignItems: 'center', paddingHorizontal: 20, paddingTop: 20, paddingBottom: 16 }}>
      <SkinRegion app="about" region="banner" model={vm}>
      <Icon size={64} />
      <View style={{ marginTop: 12 }}><LordOSWordmark size={typeScale.largeTitle} crown={false} /></View>
      <Text style={{ fontSize: typeScale.callout, color: t.textDim, marginTop: 2 }}>{ver.channel ? tr('about.versionChannel', { v: ver.version, channel: ver.channel }) : tr('about.version', { v: ver.version })}</Text>
      {ver.build ? <Text style={{ fontSize: typeScale.caption, color: t.textDim, marginTop: 1 }}>{tr('about.build', { b: ver.build })}</Text> : null}
      <View style={{ flexDirection: 'row', gap: 6, marginTop: 8 }}>
        {isSystemApp('about') ? <Badge color="#0a84ff" label={tr('about.systemApp')} /> : null}
        <Badge color={cs.color} label={tr(cs.key)} />
      </View>
      {codeNote ? <Text style={{ fontSize: typeScale.caption, color: code.state === 'development' ? '#ff9f0a' : t.textDim, marginTop: 4, textAlign: 'center' }}>{codeNote}</Text> : null}
      <Text style={{ fontSize: typeScale.callout, color: t.textDim, marginTop: 10, maxWidth: 320, textAlign: 'center', lineHeight: 17 }}>{tr('about.tagline')}</Text>
      </SkinRegion>
      <View style={{ alignSelf: 'stretch', marginTop: 16, gap: 2 }}>
        <InfoRow k={tr('about.os')} v={info.os} />
        <InfoRow k={tr('about.kernel')} v={`${info.kernel} (${info.arch})`} />
        <InfoRow k={tr('about.host')} v={tr('about.hostValue', { host: info.hostname, user: info.user })} />
        <InfoRow k={tr('about.cpu')} v={tr('about.cpuValue', { cpu: cpu.slice(0, 36) || tr('about.cpuUnknown'), count: info.cores })} />
        <InfoRow k={tr('about.memory')} v={tr('shell.cc.memDetail', { used: formatBytes(memUsedKB(info) * 1024), total: formatBytes(info.memTotalKB * 1024) })} />
        {caseId === 'argon-one-v5' ? <InfoRow k={tr('about.case')} v={tr('about.caseArgonV5')} /> : null}
        <InfoRow k={tr('about.uptime')} v={formatDuration(info.uptime)} />
        {info.bootSeconds > 0 ? (
          <InfoRow k={tr('about.bootTime')} v={tr(info.bootShown === 'session' ? 'about.bootToDesktop' : 'about.bootToLogin', { s: formatNumber(info.bootSeconds, 3) })} />
        ) : null}
        <View style={{ height: 1, backgroundColor: t.separator, marginVertical: 8, marginHorizontal: 24 }} />
        <InfoRow k={tr('about.server')} v="lord-server (C · DRM/KMS · evdev)" />
        <InfoRow k={tr('about.engine')} v={`QuickJS + React ${reactVersion}`} />
        <InfoRow k={tr('about.codeLabel')} v={tr(cs.key)} />
        <InfoRow k={tr('about.display')} v={`${screen.width} × ${screen.height}`} />
        <InfoRow k={tr('about.ram')} v={`${formatNumber(st.rssKB / 1024, 1)} MB (JS ${formatNumber(st.jsKB / 1024, 1)} MB)`} />
        <InfoRow k={tr('about.render')} v={tr('about.renderValue', { ms: formatNumber(st.layoutMs + st.paintMs, 1), count: st.nodes })} />
        {/* main-thread pauses this session: the stall detector's count (lord-server's log has each one) */}
        {st.stalls != null ? <InfoRow k={tr('about.pauses')} v={tr('about.pausesValue', { count: formatNumber(st.stalls, 0), ms: formatNumber(st.stallWorstMs || 0, 0) })} /> : null}
      </View>
      <View style={{ marginTop: 12 }}>
        <Button small label={tr('about.systemReport')} onClick={() => wm.open('sysinfo')} />
      </View>
      <View style={{ flex: 1, minHeight: 12 }} />
      <View style={{ alignSelf: 'stretch', flexDirection: 'row', alignItems: 'center', gap: 8, paddingHorizontal: 24 }}>
        <Lumo pose="avatar" size={24} />
        <Text style={{ flex: 1, fontSize: typeScale.caption, color: t.textDim, textAlign: 'start', lineHeight: 1.4 }}>{tr('about.credits')}</Text>
      </View>
      {/* trademarks (maintainer, 2026-11-22): the names and the logos Machines may download on request (docs/TRADEMARKS.md) */}
      <Text style={{ alignSelf: 'stretch', paddingHorizontal: 24, fontSize: typeScale.caption, color: t.textDim, textAlign: 'start', lineHeight: 1.4 }}>{tr('about.trademarks')}</Text>
    </View>
    </ScrollView>
    </View>
  );
}
