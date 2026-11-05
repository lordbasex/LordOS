// Settings → Network: interfaces, Wi-Fi and VPN (our own WireGuard, OpenVPN). Reads the state without root
// (lord.net, netlink) and makes changes through the administration helper (net-* actions). See docs/NETWORK.md.
import {
  View, Text, useTheme, useLocale, Button, Toggle, Segmented, TextInput,
  admin, fs, formatBytes, keyring, AutofillButton, spacing, typeScale, errorText, statusText,
  ToolbarButton, MenuButton, openUrl,
} from '@lordos/sdk';
import { useEffect, useState } from 'react';
import { Stack, Section, SettingRow, ValueRow, FieldRow, Badge, Note, Message, Actions, StatusDot, tint } from './ui.jsx';

// VPN interfaces (WireGuard, OpenVPN, IPsec, PPTP, OpenConnect, Fortinet): shown under VPN.
const isVpnIf = (i) => ['wireguard', 'tun', 'xfrm', 'ppp'].includes(i.type) || /^(wg|ovpn|ips|ppp|oc|fvpn)-/.test(i.name);
const has = () => typeof lord.net !== 'undefined';

// Reread when the kernel notifies (link/addr/route), and every 2 s for the traffic.
let netGen = 0;
const netSubs = new Set();
globalThis.__lordNet = () => { netGen++; for (const fn of netSubs) fn(netGen); };
function useNet(intervalMs = 2000) {
  const [, setG] = useState(0);
  useEffect(() => {
    const fn = (g) => setG(g);
    netSubs.add(fn);
    const id = setInterval(() => setG((g) => g + 1), intervalMs);
    return () => { netSubs.delete(fn); clearInterval(id); };
  }, []);
  return has() ? { ifs: lord.net.interfaces(), routes: lord.net.routes(), dns: lord.net.dns() } : { ifs: [], routes: [], dns: { servers: [] } };
}

// Notices from the network processes (DHCP, Wi-Fi, VPN) to show status. The tokens are English and
// stable ("net vpn <name> connected"); an "error" carries a code line (server/src/errcode.h) or a
// program's own output, both shown with errorText().
const events = [];
const eventSubs = new Set();
admin.onEvent((text) => {
  if (!text.startsWith('net ')) return;
  vpnPromptEvent(text);
  events.push(text);
  if (events.length > 50) events.shift();
  for (const fn of eventSubs) fn(text);
});

// Fortinet: the two things lord-netd may ask the person while connecting (server/src/net/vpnproc.c).
//   net vpn <name> sso <url>              single sign-on: open the organization's sign-in page and wait
//   net vpn <name> untrusted-cert {json}  the server's certificate couldn't be verified: trust it?
// Kept here (not in a page's state) so they survive switching pages. Only the session that asked to
// connect opens the browser (events reach every session); the others show the waiting state with
// a button to open the page.
const SSO_MINUTES = 5;
const SSO_URL = /^https:\/\/[A-Za-z0-9.-]+:\d{1,5}\/remote\/saml\/start\?redirect=1$/;
const vpnPrompts = { sso: null, cert: null };        // sso: { name, url, since, opened } · cert: { name, sha256, subject, issuer, changed }
const connectingHere = new Set();
function vpnPromptEvent(text) {
  const m = /^net vpn (\S+) (\S+)\s?(.*)$/.exec(text);
  if (!m) return;
  const [, name, what, rest] = m;
  if (what === 'sso' && SSO_URL.test(rest)) {
    const mine = connectingHere.has(name);
    vpnPrompts.sso = { name, url: rest, since: Date.now(), opened: mine && openUrl(rest) };
  } else if (what === 'untrusted-cert') {
    try {
      const j = JSON.parse(rest);
      if (/^[0-9a-f]{64}$/.test(j.sha256)) vpnPrompts.cert = { name, sha256: j.sha256, subject: String(j.subject || ''), issuer: String(j.issuer || ''), changed: !!j.changed };
    } catch (e) { /* not ours */ }
    if (vpnPrompts.sso?.name === name) vpnPrompts.sso = null;
  } else if (['connected', 'disconnected', 'error', 'auth-failed'].includes(what)) {
    if (vpnPrompts.sso?.name === name) vpnPrompts.sso = null;
    if (what !== 'disconnected') connectingHere.delete(name);
  }
}
// 64 hex digits → "AB:CD:…", the way certificate fingerprints are usually shown
const fingerprint = (hex) => String(hex).toUpperCase().match(/../g)?.join(':') || '';

function useNetEvent(fn) { useEffect(() => { eventSubs.add(fn); return () => eventSubs.delete(fn); }, []); }

// The OK reply is a status code line (t('status.<code>')) unless the caller has its own text.
const run = (setMsg, action, arg, okText) => admin.run(action, arg, undefined, { quiet: true }).then(
  (r) => { setMsg({ ok: true, text: okText || statusText(r) }); return r; },
  (e) => { setMsg({ ok: false, text: e.message }); return null; });

const TYPES = ['ethernet', 'wifi', 'wireguard', 'tun', 'bridge', 'veth', 'vlan', 'bond', 'dummy'];
const typeLabel = (type, tr) => (TYPES.includes(type) ? tr(`network.type.${type}`) : type);

const statusOf = (i, tr) => (!i.up ? tr('network.status.off') : !i.carrier && i.type !== 'wireguard' && i.type !== 'tun' ? tr('network.status.noCable')
  : i.addresses.some((a) => a.scope === 'global') ? tr('network.status.connected') : tr('network.status.noAddress'));
const dotColor = (i) => (!i.up ? tint.off : i.addresses.some((a) => a.scope === 'global') ? tint.ok : tint.warn);

// ---- one interface ----

function InterfacePage({ name, net }) {
  const t = useTheme();
  const { t: tr } = useLocale();
  const i = net.ifs.find((x) => x.name === name);
  const [msg, setMsg] = useState(null);
  const saved = has() ? lord.net.config(name) : null;
  const [form, setForm] = useState(null);
  useEffect(() => { setMsg(null); setForm(null); }, [name]);
  if (!i) return <Note>{tr('network.gone', { name })}</Note>;
  const gw = net.routes.find((r) => r.dst === 'default' && r.dev === name);
  const isAdmin = admin.available();
  const sys = i.managed === 'system';
  const f = form || { method: saved?.method || 'dhcp', address: saved?.addresses?.[0] || '', gateway: saved?.gateway || '', dns: (saved?.dns || []).join(' ') };
  const apply = () => {
    const lines = [name, `method=${f.method}`];
    if (f.method === 'static') {
      if (f.address) lines.push(`address=${f.address.trim()}`);
      if (f.gateway) lines.push(`gateway=${f.gateway.trim()}`);
      for (const d of f.dns.split(/[\s,]+/).filter(Boolean)) lines.push(`dns=${d}`);
    }
    run(setMsg, 'net-config', lines.join('\n'), tr('network.applied')).then(() => setForm(null));
  };
  const field = { flex: 1 };
  return (
    <Stack>
      <Section>
        <SettingRow minHeight={52} icon={<Badge glyph={i.type === 'wifi' ? '◠' : '⇅'} color={i.up ? '#0a84ff' : tint.off} size={32} />}
          label={name} labelStyle={{ fontWeight: 600 }} hint={`${typeLabel(i.type, tr)} · ${statusOf(i, tr)}`}>
          <StatusDot color={dotColor(i)} />
          {!sys && isAdmin && <Toggle small on={i.up} onChange={(on) => run(setMsg, on ? 'net-up' : 'net-down', name)} />}
        </SettingRow>
      </Section>
      <Message msg={msg} />
      <Section>
        {i.addresses.filter((a) => a.scope !== 'host').map((a) => (
          <ValueRow key={`${a.address}/${a.prefix}`} label={a.family === 4 ? 'IPv4' : 'IPv6'} value={`${a.address}/${a.prefix}${a.dynamic ? ` · ${tr('network.dynamic')}` : ''}`} />
        ))}
        {gw && <ValueRow label={tr('network.gateway')} value={gw.gateway} />}
        {gw && net.dns.servers.length > 0 && <ValueRow label="DNS" value={net.dns.servers.join(', ')} />}
        {i.mac && i.type !== 'wireguard' && <ValueRow label="MAC" value={i.mac} />}
        <ValueRow label={tr('network.traffic')} value={tr('network.trafficValue', { rx: formatBytes(i.rxBytes), tx: formatBytes(i.txBytes) })} />
      </Section>
      {sys ? (
        <Section title={tr('network.config')} footer={tr('network.systemManaged')}>
          {isAdmin && <SettingRow label={tr('network.manage')}><Button small label={tr('network.manage')} onClick={() => run(setMsg, 'net-manage', name, tr('network.managedNow'))} /></SettingRow>}
        </Section>
      ) : i.type === 'wireguard' || i.type === 'tun' ? (
        <Section title={tr('network.config')} footer={tr('network.vpnInterface')} />
      ) : (
        <View style={{ gap: spacing.m }}>
          <Section title={tr('network.config')} footer={!isAdmin ? tr('network.needAdmin') : ''}>
            <SettingRow label={tr('network.method')}>
              <Segmented small value={f.method} onChange={(method) => setForm({ ...f, method })}
                options={[['dhcp', tr('network.dhcp')], ['static', tr('network.static')], ['off', tr('network.off')]]} />
            </SettingRow>
            {f.method === 'static' && <FieldRow label={tr('network.address')}><TextInput value={f.address} onChange={(address) => setForm({ ...f, address })} placeholder="192.168.1.20/24" style={field} /></FieldRow>}
            {f.method === 'static' && <FieldRow label={tr('network.gateway')}><TextInput value={f.gateway} onChange={(gateway) => setForm({ ...f, gateway })} placeholder="192.168.1.1" style={field} /></FieldRow>}
            {f.method === 'static' && <FieldRow label="DNS"><TextInput value={f.dns} onChange={(dns) => setForm({ ...f, dns })} placeholder="1.1.1.1 9.9.9.9" style={field} /></FieldRow>}
          </Section>
          <Actions>
            {i.managed === 'lordos' && isAdmin && saved && <Button label={tr('network.unmanage')} onClick={() => run(setMsg, 'net-unmanage', name)} />}
            <Button primary label={tr('network.apply')} disabled={!isAdmin} onClick={() => isAdmin && apply()} />
          </Actions>
        </View>
      )}
    </Stack>
  );
}

// ---- Wi-Fi ----

const bars = (dbm) => (dbm >= -55 ? '▂▄▆█' : dbm >= -67 ? '▂▄▆' : dbm >= -78 ? '▂▄' : '▂');

function WifiPage({ net }) {
  const t = useTheme();
  const { t: tr } = useLocale();
  const radios = net.ifs.filter((i) => i.type === 'wifi');
  const [dev, setDev] = useState(radios[0]?.name || '');
  const [list, setList] = useState([]);
  const [scanning, setScanning] = useState(false);
  const [pick, setPick] = useState(null);
  const [pass, setPass] = useState('');
  const [msg, setMsg] = useState(null);
  const known = has() ? lord.net.wifiKnown() : [];
  const cur = net.ifs.find((i) => i.name === dev);
  const scan = () => {
    if (!dev) return;
    setScanning(true);
    const found = [];
    admin.run('net-wifi-scan', dev, (line) => { try { found.push(JSON.parse(line)); } catch (e) { /* text */ } }, { quiet: true })
      .then(() => setList(found.sort((a, b) => b.signal - a.signal)), (e) => setMsg({ ok: false, text: e.message }))
      .finally(() => setScanning(false));
  };
  useEffect(() => { scan(); }, [dev]);
  useNetEvent((text) => {
    const m = /^net wifi (\S+) (connected|disconnected|wrong-key|no-sae)\s?(.*)$/.exec(text);
    if (!m || m[1] !== dev) return;
    if (m[2] === 'connected') setMsg({ ok: true, text: tr('network.wifi.connected', { ssid: m[3] }) });
    else if (m[2] === 'wrong-key') setMsg({ ok: false, text: tr('network.wifi.wrongKey') });
    else if (m[2] === 'no-sae') setMsg({ ok: false, text: tr('network.wifi.noSae') });
  });
  if (!radios.length) return <Stack><Section title="Wi-Fi" footer={tr('network.wifi.none')} /></Stack>;
  const connect = (n, password) => {
    setPick(null);
    setPass('');
    // the password also goes to the session keyring (Passwords → Session keyring; docs/PASSWORDS.md)
    if (password && n.ssid) keyring.set('settings', { service: `wifi:${n.ssid}`, secret: password, label: `Wi-Fi ${n.ssid}` }).catch(() => {});
    run(setMsg, 'net-wifi-connect', `${dev}\n${n.ssid}\n${password || ''}`, tr('network.wifi.connecting', { ssid: n.ssid }));
  };
  const dim = { fontSize: typeScale.caption, color: t.textDim, whiteSpace: 'nowrap' };
  return (
    <Stack>
      <Section title="Wi-Fi" accessory={(
        <View style={{ flexDirection: 'row', alignItems: 'center', gap: spacing.s }}>
          {radios.length > 1 && <Segmented small value={dev} onChange={setDev} options={radios.map((r) => [r.name, r.name])} />}
          <Button small label={scanning ? tr('network.wifi.scanning') : tr('network.wifi.scan')} onClick={() => !scanning && scan()} />
          {cur?.up && <Button small label={tr('network.wifi.disconnect')} onClick={() => run(setMsg, 'net-wifi-disconnect', dev)} />}
        </View>
      )}>
        {list.length === 0 ? <SettingRow label={scanning ? tr('network.wifi.scanning') : tr('network.wifi.noNetworks')} labelStyle={{ color: t.textDim }} /> : null}
        {list.map((n, idx) => (
          <View key={`${n.bssid}${idx}`}>
            <SettingRow onClick={() => (n.security === 'open' || n.known ? connect(n, '') : setPick(n))}
              icon={<Text style={{ width: 30, fontSize: typeScale.callout, color: t.accent, direction: 'ltr' }}>{bars(n.signal)}</Text>}
              label={n.ssid || tr('network.wifi.hidden')} labelStyle={{ whiteSpace: 'nowrap', overflow: 'hidden' }}>
              {n.known && <Text style={dim}>{tr('network.wifi.saved')}</Text>}
              <Text style={dim}>{n.security === 'open' ? tr('network.wifi.open') : n.security.toUpperCase()}</Text>
              <Text style={{ ...dim, width: 64, textAlign: 'end', direction: 'ltr' }}>{`${n.signal} dBm`}</Text>
            </SettingRow>
            {pick === n && (
              <View style={{ flexDirection: 'row', alignItems: 'center', gap: spacing.s, paddingBottom: spacing.m }}>
                <TextInput secure autoFocus value={pass} onChange={setPass} placeholder={tr('network.wifi.password')} onSubmit={() => connect(n, pass)}
                  onCancel={() => setPick(null)} style={{ flex: 1 }} />
                <AutofillButton app="settings" service={`wifi:${n.ssid}`} ssid={n.ssid} onFill={({ password }) => setPass(password || '')} />
                <Button primary label={tr('network.wifi.join')} onClick={() => connect(n, pass)} />
              </View>
            )}
          </View>
        ))}
      </Section>
      <Message msg={msg} />
      {known.length > 0 && (
        <Section title={tr('network.wifi.known')}>
          {known.map((k) => (
            <SettingRow key={k.ssid} label={k.ssid}>
              {admin.available() && <Button small label={tr('network.wifi.forget')} onClick={() => run(setMsg, 'net-wifi-forget', k.ssid)} />}
            </SettingRow>
          ))}
        </Section>
      )}
    </Stack>
  );
}

// ---- VPN ----

// Configurations found in the home folder (Downloads, Documents, Desktop).
function findVpnFiles() {
  const out = [];
  for (const dir of ['', 'Descargas', 'Downloads', 'Documentos', 'Documents', 'Escritorio', 'Desktop']) {
    let list = [];
    try { list = fs.list(dir || '.'); } catch (e) { continue; }
    for (const e of list) if (!e.dir && /\.(conf|ovpn)$/i.test(e.name) && e.size < 256 * 1024) out.push({ rel: dir ? `${dir}/${e.name}` : e.name, name: e.name });
  }
  return out;
}

const cleanName = (s) => s.replace(/\.(conf|ovpn)$/i, '').replace(/[^A-Za-z0-9_-]/g, '-').slice(0, 24) || 'vpn';

const VPN_LABEL = { wireguard: 'WireGuard', openvpn: 'OpenVPN', ipsec: 'IPsec / IKEv2', pptp: 'PPTP', openconnect: 'Cisco AnyConnect', fortinet: 'Fortinet' };
const FILE_TYPES = ['wireguard', 'openvpn'];

// Form fields for each type (those that do not come in a file).
// [key, label, options, show(form), secret, optional]
const FORM = {
  ipsec: [['server'], ['auth', null, ['eap', 'psk']], ['user', null, null, (f) => f.auth !== 'psk'], ['password', null, null, (f) => f.auth !== 'psk', true],
    ['psk', null, null, (f) => f.auth === 'psk', true], ['remote_id', null, null, null, false, true], ['routes', null, null, null, false, true]],
  pptp: [['server'], ['user'], ['password', null, null, null, true], ['routes', null, null, null, false, true]],
  openconnect: [['server'], ['protocol', null, ['anyconnect', 'gp', 'pulse']], ['user'], ['password', null, null, null, true],
    ['usergroup', null, null, null, false, true], ['servercert', null, null, null, false, true]],
  // Fortinet: user name and password, or single sign-on (the organization's sign-in page in the
  // browser; docs/NETWORK.md, "Fortinet with single sign-on")
  fortinet: [['host'], ['port', null, null, null, false, true], ['auth', null, ['password', 'sso']],
    ['user', null, null, (f) => f.auth !== 'sso'], ['password', null, null, (f) => f.auth !== 'sso', true],
    ['sso-port', null, null, (f) => f.auth === 'sso', false, true], ['trusted-cert', null, null, null, false, true],
    ['routes', null, null, null, false, true]],
};
const FORM_DEFAULTS = { ipsec: { auth: 'eap' }, openconnect: { protocol: 'anyconnect' }, fortinet: { port: '443', auth: 'password' } };
const PLACEHOLDER = { 'sso-port': '8020' };

// The trash can of a row's Delete button, drawn (a lid and a body).
function TrashGlyph({ color }) {
  return (
    <View style={{ width: 12, height: 14, alignItems: 'center' }}>
      <View style={{ width: 4, height: 2, borderTopLeftRadius: 1, borderTopRightRadius: 1, backgroundColor: color }} />
      <View style={{ width: 12, height: 1.5, backgroundColor: color }} />
      <View style={{ width: 9, height: 9.5, marginTop: 1, borderBottomLeftRadius: 2, borderBottomRightRadius: 2, border: `1.5px solid ${color}`, borderTopWidth: 0,
        flexDirection: 'row', justifyContent: 'space-evenly', paddingTop: 1.5, paddingBottom: 1.5 }}>
        <View style={{ width: 1, backgroundColor: color }} />
        <View style={{ width: 1, backgroundColor: color }} />
      </View>
    </View>
  );
}

// edit: { name, type, values, secrets } (net-vpn-config) to change a saved VPN instead of adding one.
function VpnImport({ onDone, edit }) {
  const t = useTheme();
  const { t: tr } = useLocale();
  const types = has() && lord.net.vpnTypes ? lord.net.vpnTypes() : FILE_TYPES.map((type) => ({ type, available: true }));
  const [type, setType] = useState(edit ? edit.type : null);
  const [files] = useState(findVpnFiles);
  const [file, setFile] = useState(null);
  const [name, setName] = useState(edit ? edit.name : '');
  const [form, setForm] = useState(edit ? { ...(FORM_DEFAULTS[edit.type] || {}), ...edit.values } : {});
  const [msg, setMsg] = useState(null);
  const [enabling, setEnabling] = useState(null);
  const [, setTick] = useState(0);
  const pick = (tp) => { setType(tp.type); setForm({ ...(FORM_DEFAULTS[tp.type] || {}) }); setFile(null); setMsg(null); setName(''); };
  const enable = (tp) => {
    setEnabling(tp.type);
    admin.run('net-vpn-enable', tp.type, undefined, { quiet: true }).then(
      () => { setMsg({ ok: true, text: tr('network.vpn.enabled', { name: VPN_LABEL[tp.type] }) }); setTick((x) => x + 1); },
      (e) => setMsg({ ok: false, text: e.message })).finally(() => setEnabling(null));
  };
  const kept = (k) => edit && edit.secrets.includes(k);        // a saved secret the form doesn't show: kept if left empty
  const doImport = () => {
    let config;
    const vpnName = edit ? edit.name : cleanName(name || (file ? file.name : type));
    if (FILE_TYPES.includes(type)) {
      try { config = type === 'openvpn' ? lord.net.prepareOvpn(`${fs.home()}/${file.rel}`) : fs.read(file.rel); } catch (e) { setMsg({ ok: false, text: e.message }); return; }
    } else {
      const fields = FORM[type].filter(([k, , , show]) => !show || show(form));
      const missing = fields.find(([k, , , , , optional]) => !optional && !kept(k) && !String(form[k] || '').trim());
      if (missing) { setMsg({ ok: false, text: tr('network.vpn.missing', { field: tr(`network.vpn.field.${missing[0]}`) }) }); return; }
      config = fields.filter(([k]) => String(form[k] || '').trim()).map(([k]) => `${k}=${String(form[k]).trim()}`).join('\n');
    }
    if (edit) run(setMsg, 'net-vpn-edit', `${type}\n${vpnName}\n${config}`, tr('network.vpn.saved')).then((r) => r != null && onDone());
    else run(setMsg, 'net-vpn-import', `${type}\n${vpnName}\n${config}`, tr('network.vpn.imported')).then((r) => r != null && onDone());
  };
  const cur = types.find((x) => x.type === type);
  const files4 = files.filter((f) => (type === 'openvpn' ? /\.ovpn$/i : /\.conf$/i).test(f.rel));
  const field = { flex: 1 };
  return (
    <View style={{ gap: spacing.m }}>
      <Section title={edit ? tr('network.vpn.editTitle', { name: edit.name }) : tr('network.vpn.importTitle')}>
        {!edit && (
          <View style={{ flexDirection: 'row', flexWrap: 'wrap', gap: spacing.s, paddingVertical: spacing.m }}>
            {types.map((tp) => {
              const on = type === tp.type;
              return (
                <View key={tp.type} onClick={() => pick(tp)} style={{ minWidth: 112, minHeight: 44, justifyContent: 'center', paddingHorizontal: 10, paddingVertical: 7, borderRadius: 8, gap: 2,
                  backgroundColor: on ? t.accent : t.control }}>
                  <Text style={{ fontSize: typeScale.callout, fontWeight: 600, color: on ? '#fff' : t.text, textAlign: 'start' }}>{VPN_LABEL[tp.type] || tp.type}</Text>
                  {(tp.optional || tp.insecure) && <Text style={{ fontSize: 10, color: on ? '#fff' : tp.insecure ? tint.warn : t.textDim, textAlign: 'start' }}>
                    {tp.insecure ? tr('network.vpn.insecure') : tp.available ? tr('network.vpn.optionalOn') : tr('network.vpn.optionalOff')}</Text>}
                </View>
              );
            })}
          </View>
        )}
        {cur && cur.optional && !cur.available && (
          <SettingRow label={VPN_LABEL[cur.type]} hint={tr('network.vpn.enableHint', { name: VPN_LABEL[cur.type], pkg: cur.package })}>
            <Button small primary label={enabling ? tr('network.vpn.enabling') : tr('network.vpn.enable')} onClick={() => !enabling && admin.available() && enable(cur)} />
          </SettingRow>
        )}
        {cur && cur.available && FILE_TYPES.includes(type) && (
          <View style={{ gap: 2, paddingVertical: spacing.s }}>
            <Text style={{ fontSize: typeScale.caption, color: t.textDim, lineHeight: 1.35, marginBottom: spacing.xs, textAlign: 'start' }}>{tr('network.vpn.importHint')}</Text>
            {files4.map((f) => (
              <View key={f.rel} onClick={() => { setFile(f); setName(cleanName(f.name)); }} style={{ height: 26, justifyContent: 'center', paddingHorizontal: spacing.s, borderRadius: 6,
                backgroundColor: file === f ? t.accent : 'transparent' }}>
                <Text style={{ fontSize: typeScale.callout, color: file === f ? '#fff' : t.text, direction: 'ltr', textAlign: 'start' }}>{`~/${f.rel}`}</Text>
              </View>
            ))}
            {!files4.length && <Text style={{ fontSize: typeScale.callout, color: t.textDim, textAlign: 'start' }}>{tr('network.vpn.noFiles')}</Text>}
          </View>
        )}
        {cur && cur.available && FORM[type] ? FORM[type].filter(([, , , show]) => !show || show(form)).map(([k, , options, , secret, optional]) => (
          <FieldRow key={k} label={tr(`network.vpn.field.${k}`)} hint={optional || kept(k) ? tr('network.vpn.optional') : ''} width={options ? 300 : 260}>
            {options ? <Segmented small value={form[k]} onChange={(v) => setForm({ ...form, [k]: v })} options={options.map((o) => [o, tr(`network.vpn.opt.${o}`)])} />
              : <TextInput secure={secret} value={form[k] || ''} onChange={(v) => setForm({ ...form, [k]: v.replace(/[\r\n]/g, '') })}
                placeholder={kept(k) ? tr('network.vpn.unchanged') : PLACEHOLDER[k] || ''} style={field} />}
          </FieldRow>
        )) : null}
        {cur && cur.available && type === 'fortinet' && form.auth === 'sso' && (
          <Text style={{ fontSize: typeScale.caption, color: t.textDim, lineHeight: 1.35, paddingVertical: spacing.s, textAlign: 'start' }}>{tr('network.vpn.ssoHint')}</Text>
        )}
        {cur && cur.available && (FORM[type] || file) && (
          <FieldRow label={tr('network.vpn.name')} width={260}>
            {edit ? <Text style={{ fontSize: typeScale.body, color: t.textDim, textAlign: 'end' }}>{edit.name}</Text>
              : <TextInput value={name} onChange={(v) => setName(v.replace(/[^A-Za-z0-9_-]/g, '').slice(0, 24))} placeholder={type} style={field} />}
          </FieldRow>
        )}
      </Section>
      {cur && cur.insecure && <Note tone="warn">{tr('network.vpn.pptpWarning')}</Note>}
      <Message msg={msg} />
      <Actions>
        <Button label={edit ? tr('network.cancel') : tr('network.close')} onClick={onDone} />
        {cur && cur.available && (FORM[type] || file) && <Button primary label={edit ? tr('network.vpn.save') : tr('network.vpn.import')} onClick={doImport} />}
      </Actions>
    </View>
  );
}

// The server's certificate couldn't be verified (or changed): trust it, or not.
function CertPrompt({ cert, onTrust, onCancel }) {
  const t = useTheme();
  const { t: tr } = useLocale();
  const isAdmin = admin.available();
  const mono = { fontSize: typeScale.caption, color: t.text, direction: 'ltr', textAlign: 'start', fontFamily: 'monospace', lineHeight: 1.4 };
  return (
    <View style={{ gap: spacing.m }}>
      <Section title={cert.changed ? tr('network.vpn.certChangedTitle', { name: cert.name }) : tr('network.vpn.certTitle')}>
        <Text style={{ fontSize: typeScale.callout, color: t.text, lineHeight: 1.4, paddingVertical: spacing.s, textAlign: 'start' }}>
          {cert.changed ? tr('network.vpn.certChangedBody', { name: cert.name }) : tr('network.vpn.certBody', { name: cert.name })}</Text>
        <View style={{ gap: 2, paddingVertical: spacing.xs }}>
          <Text style={{ fontSize: typeScale.caption, color: t.textDim, textAlign: 'start' }}>{tr('network.vpn.certFingerprint')}</Text>
          <Text style={mono}>{fingerprint(cert.sha256)}</Text>
        </View>
        {cert.subject ? <ValueRow label={tr('network.vpn.certSubject')} value={cert.subject} /> : null}
        {cert.issuer ? <ValueRow label={tr('network.vpn.certIssuer')} value={cert.issuer} /> : null}
      </Section>
      {!isAdmin && <Note>{tr('network.vpn.certNeedAdmin')}</Note>}
      <Actions>
        <Button label={tr('network.cancel')} onClick={onCancel} />
        {isAdmin && <Button primary label={tr('network.vpn.certTrust')} onClick={onTrust} />}
      </Actions>
    </View>
  );
}

// Single sign-on: the sign-in page is open in the browser; lord-netd waits for it (SSO_MINUTES).
function SsoWait({ sso, onCancel }) {
  const t = useTheme();
  const { t: tr } = useLocale();
  const [, setNow] = useState(0);
  useEffect(() => { const id = setInterval(() => setNow((x) => x + 1), 1000); return () => clearInterval(id); }, []);
  const left = Math.max(0, SSO_MINUTES * 60 - Math.round((Date.now() - sso.since) / 1000));
  return (
    <View style={{ gap: spacing.m }}>
      <Section title={tr('network.vpn.ssoWaitTitle')}>
        <Text style={{ fontSize: typeScale.callout, color: t.text, lineHeight: 1.4, paddingVertical: spacing.s, textAlign: 'start' }}>
          {sso.opened ? tr('network.vpn.ssoWaitBody', { name: sso.name }) : tr('network.vpn.ssoOpenBody', { name: sso.name })}</Text>
        <Text style={{ fontSize: typeScale.caption, color: t.textDim, direction: 'ltr', textAlign: 'start', paddingBottom: spacing.s }}>{sso.url}</Text>
        <Text style={{ fontSize: typeScale.caption, color: t.textDim, textAlign: 'start', paddingBottom: spacing.s }}>
          {tr('network.vpn.ssoTimeLeft', { time: `${Math.floor(left / 60)}:${String(left % 60).padStart(2, '0')}` })}</Text>
      </Section>
      <Actions>
        <Button label={tr('network.cancel')} onClick={onCancel} />
        <Button primary={!sso.opened} label={tr('network.vpn.ssoOpen')} onClick={() => { if (openUrl(sso.url)) sso.opened = true; else sso.noBrowser = true; setNow((x) => x + 1); }} />
      </Actions>
      {sso.noBrowser && !sso.opened && <Note tone="warn">{tr('network.vpn.ssoNoBrowser')}</Note>}
    </View>
  );
}

function VpnPage({ net }) {
  const t = useTheme();
  const { t: tr } = useLocale();
  const [importing, setImporting] = useState(false);
  const [editing, setEditing] = useState(null);   // net-vpn-config of the VPN being edited
  const [msg, setMsg] = useState(null);
  const [status, setStatus] = useState({});
  const [auth, setAuth] = useState(null);         // { name, user, pass }
  const [, setTick] = useState(0);
  const list = has() ? lord.net.vpnList() : [];
  const refresh = () => {
    const st = {};
    admin.run('net-vpn-status', '', (line) => { try { const j = JSON.parse(line); st[j.name] = j; } catch (e) { /* text */ } }, { quiet: true })
      .then(() => setStatus(st), () => {});
  };
  useEffect(() => { refresh(); const id = setInterval(refresh, 4000); return () => clearInterval(id); }, []);
  useNetEvent((text) => {
    const m = /^net vpn (\S+) (\S+)\s?(.*)$/.exec(text);
    if (!m) return;
    if (m[2] === 'connected') setMsg({ ok: true, text: tr('network.vpn.connectedMsg', { name: m[1] }) });
    else if (m[2] === 'need-auth') setAuth({ name: m[1], user: '', pass: '' });
    else if (m[2] === 'auth-failed') setMsg({ ok: false, text: tr('network.vpn.authFailed', { name: m[1] }) });
    else if (m[2] === 'untrusted-cert' || m[2] === 'sso') setMsg(null);
    else if (m[2] === 'error' || m[2] === 'log') {
      const why = m[3] ? errorText(m[3]) : tr('network.vpn.error');
      setMsg({ ok: false, text: m[1] === '-' ? why : `${m[1]}: ${why}` });
    }
    refresh();
    setTick((x) => x + 1);
  });
  const isAdmin = admin.available();
  const up = (v) => { connectingHere.add(v.name); return run(setMsg, 'net-vpn-up', v.name).then(refresh); };
  const down = (v) => { connectingHere.delete(v.name); return run(setMsg, 'net-vpn-down', v.name).then(refresh); };
  const startEdit = (v) => {
    let cfg = null;
    admin.run('net-vpn-config', v.name, (line) => { try { cfg = JSON.parse(line); } catch (e) { /* text */ } }, { quiet: true })
      .then(() => { if (cfg) { setImporting(false); setEditing({ ...cfg, secrets: cfg.secrets || [] }); } }, (e) => setMsg({ ok: false, text: e.message }));
  };
  const field = { flex: 1 };
  const cert = vpnPrompts.cert, sso = vpnPrompts.sso;
  return (
    <Stack>
      {importing && <VpnImport onDone={() => { setImporting(false); setTick((x) => x + 1); }} />}
      {editing && <VpnImport key={editing.name} edit={editing} onDone={() => { setEditing(null); setTick((x) => x + 1); }} />}
      {cert && (
        <CertPrompt cert={cert} onCancel={() => { vpnPrompts.cert = null; setTick((x) => x + 1); }}
          onTrust={() => {
            vpnPrompts.cert = null;
            setTick((x) => x + 1);
            run(setMsg, 'net-vpn-trust', `${cert.name}\n${cert.sha256}`, tr('network.vpn.trustedMsg', { name: cert.name })).then((r) => r != null && up({ name: cert.name }));
          }} />
      )}
      {sso && !cert && <SsoWait sso={sso} onCancel={() => { vpnPrompts.sso = null; down({ name: sso.name }); setTick((x) => x + 1); }} />}
      {auth && (
        <View style={{ gap: spacing.m }}>
          <Section title={tr('network.vpn.authTitle', { name: auth.name })}>
            <FieldRow label={tr('network.vpn.user')}>
              <TextInput value={auth.user} onChange={(user) => setAuth({ ...auth, user })} placeholder={tr('network.vpn.user')} style={field} />
            </FieldRow>
            <FieldRow label={tr('network.vpn.password')}>
              <TextInput secure value={auth.pass} onChange={(pass) => setAuth({ ...auth, pass })} placeholder={tr('network.vpn.password')} style={field} />
              <AutofillButton app="settings" service={`vpn:${auth.name}`} onFill={({ username, password }) => setAuth({ ...auth, user: username || auth.user, pass: password || '' })} />
            </FieldRow>
          </Section>
          <Actions>
            <Button label={tr('network.cancel')} onClick={() => setAuth(null)} />
            <Button primary label={tr('network.vpn.saveConnect')} onClick={() => {
              const a = auth;
              setAuth(null);
              if (a.pass) keyring.set('settings', { service: `vpn:${a.name}`, account: a.user, secret: a.pass, label: `VPN ${a.name}` }).catch(() => {});
              run(setMsg, 'net-vpn-credentials', `${a.name}\n${a.user}\n${a.pass}`).then((r) => r != null && up({ name: a.name }));
            }} />
          </Actions>
        </View>
      )}
      <Section title="VPN" accessory={isAdmin && !importing && !editing ? <Button small label={tr('network.vpn.add')} onClick={() => setImporting(true)} /> : null}>
        {list.length === 0 ? <SettingRow label={tr('network.vpn.none')} labelStyle={{ color: t.textDim }} /> : null}
        {list.map((v) => {
          const st = status[v.name] || {};
          const on = !!st.active;
          const busy = on || !!st.running;
          const peer = st.peers?.[0];
          const iface = net.ifs.find((i) => i.name === v.ifname);
          const who = v.sso ? ` · ${tr('network.vpn.sso')}` : v.user ? ` · ${v.user}` : '';
          const lines = [
            `${VPN_LABEL[v.type] || v.type} · ${v.server || ''}${who}${v.fullTunnel && v.type !== 'openconnect' ? ` · ${tr('network.vpn.fullTunnel')}` : ''}${v.insecure ? ` · ⚠ ${tr('network.vpn.insecure')}` : ''}`,
            on && peer?.lastHandshake ? tr('network.vpn.handshake', { s: Math.max(0, Math.round(Date.now() / 1000 - peer.lastHandshake)), rx: formatBytes(peer.rxBytes), tx: formatBytes(peer.txBytes) }) : null,
            on && v.type !== 'wireguard' && iface ? `${iface.addresses.find((a) => a.family === 4)?.address || ''} · ${tr('network.trafficValue', { rx: formatBytes(iface.rxBytes), tx: formatBytes(iface.txBytes) })}` : null,
            v.type === 'wireguard' && v.publicKey ? tr('network.vpn.publicKey', { key: v.publicKey }) : null,
          ].filter(Boolean);
          const editable = isAdmin && FORM[v.type];
          return (
            <SettingRow key={v.name} minHeight={52} icon={<StatusDot color={on ? tint.ok : st.running ? tint.warn : tint.off} />}
              label={v.name} labelStyle={{ fontWeight: 600 }} hint={lines.join('\n')}>
              {isAdmin && v.type === 'openvpn' && v.needsAuth && <Button small label={tr('network.vpn.credentials')} onClick={() => setAuth({ name: v.name, user: '', pass: '' })} />}
              {editable && (
                <MenuButton small title={tr('network.vpn.more')}
                  items={[{ label: tr('network.vpn.edit'), disabled: busy, onClick: () => startEdit(v) }]} />
              )}
              {isAdmin && (
                <ToolbarButton small title={tr('network.vpn.delete')} icon={<TrashGlyph color={tint.error} />}
                  onClick={() => run(setMsg, 'net-vpn-delete', v.name).then(() => setTick((x) => x + 1))} />
              )}
              <Toggle small on={busy} onChange={(x) => (x ? up(v) : down(v))} />
            </SettingRow>
          );
        })}
      </Section>
      <Message msg={msg} />
    </Stack>
  );
}

// ---- Settings section ----

// At the top, a row with the connections, Wi-Fi and VPN; below, the one chosen.
let lastPage = null;
export function NetworkSettings({ page: initial }) {
  const t = useTheme();
  const { t: tr } = useLocale();
  const net = useNet();
  const shown = net.ifs.filter((i) => !isVpnIf(i) && i.type !== 'loopback');
  const [page, setPageState] = useState(initial || lastPage || (shown[0] ? `if:${shown[0].name}` : 'wifi'));
  const setPage = (p) => { lastPage = p; setPageState(p); };
  useEffect(() => { if (initial) setPage(initial); }, [initial]);
  if (!has()) return <Stack><Note tone="warn">{tr('network.unavailable')}</Note></Stack>;
  const tabs = [...shown.map((i) => [`if:${i.name}`, i.name, dotColor(i)]), ['wifi', 'Wi-Fi'], ['vpn', 'VPN']];
  const raised = t.mode === 'dark' ? 'rgba(255,255,255,0.18)' : '#ffffff';
  return (
    <View style={{ gap: spacing.xl }}>
      {/* the connections, Wi-Fi and VPN as a neutral segmented control (the open one raised) */}
      <View style={{ alignItems: 'center' }}>
        <View style={{ flexDirection: 'row', flexWrap: 'wrap', padding: 2, gap: 2, borderRadius: 8, backgroundColor: t.control }}>
          {tabs.map(([id, label, dot]) => (
            <View key={id} onClick={() => setPage(id)} style={{ flexDirection: 'row', alignItems: 'center', gap: 6, paddingHorizontal: 12, height: 24,
              borderRadius: 6, backgroundColor: page === id ? raised : 'transparent', boxShadow: page === id ? '0 1px 2px rgba(0,0,0,0.18)' : 'none' }}>
              {dot && <StatusDot color={dot} size={7} />}
              <Text style={{ fontSize: typeScale.callout, fontWeight: page === id ? 600 : 500, color: t.text }}>{label}</Text>
            </View>
          ))}
        </View>
      </View>
      {page.startsWith('if:') && <InterfacePage name={page.slice(3)} net={net} />}
      {page === 'wifi' && <WifiPage net={net} />}
      {page === 'vpn' && <VpnPage net={net} />}
    </View>
  );
}
