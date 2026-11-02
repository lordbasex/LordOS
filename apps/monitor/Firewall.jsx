// Firewall.jsx — Settings → Privacy → "Apps' network" (registered with registerPrivacySection,
// like the Monitor item). See docs/FIREWALL.md, step 4.
//   - status of lord-netperm (the service that applies the rules in the kernel) and the mode;
//   - Trusted apps: allowed without logging (explicit denials still apply);
//   - Restricted apps: one checkbox per restriction (no internet = "LAN only", no LAN, no network,
//     no incoming connections, and the non-network ones: camera, microphone…). Every one is
//     enforced (docs/FIREWALL.md, "Non-network restrictions"). The "LordOS" row is the desktop
//     itself: it does not offer files, USB, clipboard or Bluetooth, which the shell always uses.
//     For Linux apps each checkbox says when it applies (all at once, files at the next launch)
//     and the clipboard isn't offered yet; a help line per restriction says what it covers.
// The rules are saved by lord-server (lord.fw); this screen writes no files. Drawn like the
// Settings sections: 13/600 headers, inset GroupBoxes with hairlines, footers.
import { View, Text, Button, Segmented, ScrollView, GroupBox, useTheme, useLocale, useStore, useLauncherApps, firewall, firewallState, spacing, typeScale, errorText, netModel } from '@lordos/sdk';
import { useState } from 'react';

// Firewall id of a launcher app: the built-in ones run inside LordOS.
// Isolated built-in apps (Passwords) have their own scope, like a package.
const fwId = (a) => (a.kind === 'linux' ? a.id : a.kind === 'package' || a.isolated ? `lpk:${a.id}` : 'lordos');
// The desktop shell and the built-in apps share "lordos": these would cut the desktop off itself.
const DESKTOP_ALWAYS = ['no-files', 'no-usb', 'no-clipboard', 'no-bluetooth'];
// Linux apps: the clipboard of a Wayland app isn't mediated per app yet, so it isn't offered.
const LINUX_NOT_YET = ['no-clipboard'];
// For a Linux app every restriction applies at once (new connections, device opens, sockets,
// sound streams, notifications), except files: the app's view is built when it starts.
const NEXT_LAUNCH = ['no-files'];
const isLinux = (id) => id.startsWith('desktop:');
// (0.8.5) LordOS's components with their own scope (Mail, Store, Printers, Terminal, Blocklists): they
// take restrictions like an app; the system services (system:*) and the VPN row never do
const { COMPONENTS } = netModel;
const COMPONENT_IDS = Object.keys(COMPONENTS).filter((id) => id.startsWith('lordos:') && !COMPONENTS[id].system);
const NET_ONLY = ['no-internet', 'no-lan', 'no-network', 'no-incoming'];
const offered = (id) => (COMPONENTS[id] ? NET_ONLY : id === 'lordos' ? firewall.RESTRICTIONS.filter((f) => !DESKTOP_ALWAYS.includes(f))
  : isLinux(id) ? firewall.RESTRICTIONS.filter((f) => !LINUX_NOT_YET.includes(f)) : firewall.RESTRICTIONS);

function useAppIndex() {
  const list = useLauncherApps();
  const byId = new Map();
  for (const a of list) {
    const id = fwId(a);
    if (id !== 'lordos' && !byId.has(id)) byId.set(id, a);
  }
  return byId;
}

function AppLabel({ id, apps, tr }) {
  const t = useTheme();
  const a = apps.get(id);
  const Icon = a?.Icon;
  return (
    <View style={{ flex: 1, flexDirection: 'row', alignItems: 'center', gap: 8, minWidth: 0 }}>
      {Icon ? <Icon size={24} /> : <View style={{ width: 24, height: 24, borderRadius: 6, backgroundColor: t.control }} />}
      <View style={{ flex: 1, minWidth: 0, gap: 1 }}>
        <Text style={{ fontSize: typeScale.body, color: t.text, whiteSpace: 'nowrap', overflow: 'hidden', textAlign: 'start' }}>{id === 'lordos' ? tr('monitor.lordos') : COMPONENTS[id] ? tr(COMPONENTS[id].label) : a?.name || id.replace(/^(desktop|lpk):/, '')}</Text>
        <Text style={{ fontSize: 10, color: t.textDim, whiteSpace: 'nowrap', overflow: 'hidden', fontFamily: 'monospace', textAlign: 'start' }}>{id}</Text>
      </View>
    </View>
  );
}

// Drop-down list to add an app that is not in either list yet.
function AddApp({ apps, exclude, onPick, tr }) {
  const t = useTheme();
  const [open, setOpen] = useState(false);
  const choices = ['lordos', ...COMPONENT_IDS, ...apps.keys()].filter((id) => !exclude.has(id));
  return (
    <View style={{ gap: spacing.s, paddingVertical: spacing.s }}>
      <View style={{ flexDirection: 'row' }}>
        <Button small label={open ? tr('firewall.close') : tr('firewall.add')} onClick={() => setOpen(!open)} />
      </View>
      {open && (
        <View style={{ height: 180, borderRadius: 8, border: `1px solid ${t.separator}`, backgroundColor: t.mode === 'dark' ? 'rgba(0,0,0,0.12)' : '#ffffff' }}>
          <ScrollView style={{ flex: 1, padding: 4 }}>
            {choices.map((id) => (
              <View key={id} onClick={() => { setOpen(false); onPick(id); }} style={{ flexDirection: 'row', paddingHorizontal: 8, paddingVertical: 5, borderRadius: 5 }}>
                <AppLabel id={id} apps={apps} tr={tr} />
              </View>
            ))}
            {!choices.length && <Text style={{ fontSize: 12, color: t.textDim, padding: 8 }}>{tr('firewall.noMoreApps')}</Text>}
          </ScrollView>
        </View>
      )}
    </View>
  );
}

function Check({ on, label, onChange }) {
  const t = useTheme();
  return (
    <View onClick={() => onChange(!on)} style={{ flexDirection: 'row', alignItems: 'center', gap: 5, height: 22, paddingHorizontal: 8, borderRadius: 6,
      backgroundColor: on ? 'rgba(255,69,58,0.18)' : t.control }}>
      <View style={{ width: 12, height: 12, borderRadius: 3, border: `1px solid ${on ? '#ff453a' : t.textDim}`, backgroundColor: on ? '#ff453a' : 'transparent',
        alignItems: 'center', justifyContent: 'center' }}>
        {on ? <Text style={{ fontSize: 9, color: '#fff', fontWeight: 700 }}>✓</Text> : null}
      </View>
      <Text style={{ fontSize: 11, color: on ? t.text : t.textDim }}>{label}</Text>
    </View>
  );
}

export function FirewallPrivacy() {
  const t = useTheme();
  const { t: tr } = useLocale();
  useStore(firewallState);                        // redrawn when a rule changes
  const apps = useAppIndex();
  if (!firewall.available()) return null;
  const st = firewall.state();
  const status = firewall.status() || {};
  const trusted = st.trusted || [];
  const restricted = st.restricted || {};
  const inLists = new Set([...trusted, ...Object.keys(restricted)]);
  const statusText = !status.daemon ? tr('firewall.status.noDaemon') : !status.bpf ? tr('firewall.status.noBpf', { error: errorText(status.error || '') }) :
    tr('firewall.status.on', { dns: tr(`firewall.dns.${status.dns || 'none'}`) });
  const setFlag = (id, flag, on) => {
    const cur = (restricted[id] || []).filter((f) => offered(id).includes(f));
    const next = on ? [...cur, flag] : cur.filter((f) => f !== flag);
    firewall.setRestricted(id, next.length ? next : []);
  };
  const head = (text) => <Text style={{ fontSize: typeScale.body, fontWeight: 600, color: t.text, paddingHorizontal: spacing.xs, textAlign: 'start' }}>{text}</Text>;
  const foot = (text, color) => <Text style={{ fontSize: typeScale.caption, color: color || t.textDim, lineHeight: 1.4, marginHorizontal: spacing.xs, textAlign: 'start' }}>{text}</Text>;
  const empty = <Text style={{ fontSize: typeScale.body, color: t.textDim, paddingVertical: 9, textAlign: 'start' }}>{tr('firewall.none')}</Text>;
  return (
    <View style={{ gap: 20 }}>
      <View style={{ gap: 6 }}>
        {head(tr('firewall.title'))}
        <GroupBox>
          <View style={{ minHeight: 36, flexDirection: 'row', alignItems: 'center', gap: spacing.m, paddingVertical: 7 }}>
            <View style={{ flex: 1, minWidth: 0, gap: 2 }}>
              <Text style={{ fontSize: typeScale.body, color: t.text, textAlign: 'start' }}>{tr('firewall.mode')}</Text>
              <Text style={{ fontSize: typeScale.caption, color: t.textDim, lineHeight: 1.35, textAlign: 'start' }}>{tr(st.mode === 'off' ? 'firewall.mode.offHint' : st.mode === 'deny' ? 'firewall.mode.denyHint' : 'firewall.mode.silentHint')}</Text>
            </View>
            <Segmented small value={st.mode === 'off' || st.mode === 'deny' ? st.mode : 'silent'} onChange={(m) => firewall.setMode(m)}
              options={[['silent', tr('firewall.mode.silent')], ['deny', tr('firewall.mode.deny')], ['off', tr('firewall.mode.off')]]} />
          </View>
        </GroupBox>
        {foot(statusText, status.daemon && status.bpf ? null : '#ff9f0a')}
      </View>

      <View style={{ gap: 6 }}>
        {head(tr('firewall.trusted'))}
        <GroupBox>
          {trusted.map((id) => (
            <View key={id} style={{ minHeight: 40, flexDirection: 'row', alignItems: 'center', gap: spacing.s, paddingVertical: 6 }}>
              <AppLabel id={id} apps={apps} tr={tr} />
              <Button small label={tr('firewall.remove')} onClick={() => firewall.setTrusted(id, false)} />
            </View>
          ))}
          {!trusted.length && empty}
          <AddApp apps={apps} exclude={inLists} tr={tr} onPick={(id) => firewall.setTrusted(id, true)} />
        </GroupBox>
        {foot(tr('firewall.trustedHint'))}
      </View>

      <View style={{ gap: 6 }}>
        {head(tr('firewall.restricted'))}
        <GroupBox>
          {Object.keys(restricted).sort().map((id) => (
            <View key={id} style={{ gap: spacing.s, paddingVertical: spacing.s }}>
              <View style={{ flexDirection: 'row', alignItems: 'center', gap: spacing.s }}>
                <AppLabel id={id} apps={apps} tr={tr} />
                <Button small label={tr('firewall.remove')} onClick={() => firewall.setRestricted(id, [])} />
              </View>
              <View style={{ flexDirection: 'row', flexWrap: 'wrap', gap: 6, paddingHorizontal: 32 }}>
                {offered(id).map((f) => (
                  <Check key={f} on={restricted[id].includes(f)} onChange={(on) => setFlag(id, f, on)}
                    label={isLinux(id) && NEXT_LAUNCH.includes(f) ? tr('firewall.whenNextLaunch', { name: tr(`firewall.r.${f}`) }) : tr(`firewall.r.${f}`)} />
                ))}
              </View>
              {isLinux(id) && <Text style={{ fontSize: typeScale.caption, color: t.textDim, lineHeight: 1.35, paddingHorizontal: 32, textAlign: 'start' }}>{tr('firewall.linuxWhen')}</Text>}
              {id === 'lordos' && <Text style={{ fontSize: typeScale.caption, color: t.textDim, lineHeight: 1.35, paddingHorizontal: 32, textAlign: 'start' }}>{tr('firewall.desktopAlways')}</Text>}
            </View>
          ))}
          {!Object.keys(restricted).length && empty}
          <AddApp apps={apps} exclude={inLists} tr={tr} onPick={(id) => firewall.setRestricted(id, ['no-internet'])} />
        </GroupBox>
        {foot(tr('firewall.restrictedHint'))}
        {Object.keys(restricted).length > 0 && (
          <View style={{ gap: 3, marginHorizontal: spacing.xs }}>
            {firewall.RESTRICTIONS.map((f) => (
              <Text key={f} style={{ fontSize: typeScale.caption, color: t.textDim, lineHeight: 1.4, textAlign: 'start' }}>
                {tr('firewall.helpLine', { name: tr(`firewall.r.${f}`), help: tr(`firewall.help.${f}`) })}
              </Text>
            ))}
          </View>
        )}
      </View>
      {foot(tr('firewall.footer'))}
    </View>
  );
}
