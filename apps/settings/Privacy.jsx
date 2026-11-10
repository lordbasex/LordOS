// Settings → Privacy — per-app permissions («Privacy & Security»). Lists each
// permission with the apps that requested it and lets you revoke it. The Monitor adds its own section
// through registerPrivacySection (see sdk/privacy.js); this page does not know about the Monitor.
import { View, Text, useTheme, useLocale, Segmented, useStore, perms, permsRev, privacySections, useIndicators, share, shareRev, packages, appName, getApp, spacing, typeScale, shortcutsWho, shortcutsWhoText } from '@lordos/sdk';
import { Stack, Section, SettingRow, Badge, Note, StatusDot, tint } from './ui.jsx';

// Small monochrome pictures for the permissions whose usual symbols are color emoji.
function CameraGlyph() {
  return (
    <View style={{ width: 13, height: 10, alignItems: 'center', justifyContent: 'center' }}>
      <View style={{ position: 'absolute', top: 0, width: 5, height: 3, borderRadius: 1, backgroundColor: '#fff' }} />
      <View style={{ position: 'absolute', top: 2, width: 13, height: 8, borderRadius: 2, backgroundColor: '#fff' }} />
      <View style={{ position: 'absolute', top: 4, width: 5, height: 5, borderRadius: 3, backgroundColor: '#30d158' }} />
    </View>
  );
}
function MicGlyph() {
  return (
    <View style={{ width: 10, height: 13, alignItems: 'center' }}>
      <View style={{ width: 5, height: 8, borderRadius: 3, backgroundColor: '#fff' }} />
      <View style={{ width: 2, height: 3, backgroundColor: '#fff' }} />
      <View style={{ width: 7, height: 2, borderRadius: 1, backgroundColor: '#fff' }} />
    </View>
  );
}
function FolderGlyph() {
  return (
    <View style={{ width: 13, height: 10 }}>
      <View style={{ position: 'absolute', left: 0, top: 0, width: 6, height: 3, borderRadius: 1, backgroundColor: '#fff' }} />
      <View style={{ position: 'absolute', left: 0, top: 2, width: 13, height: 8, borderRadius: 2, backgroundColor: '#fff' }} />
    </View>
  );
}

// Permissions shown with their symbol (a glyph or a drawing) and color (the order is the page order).
const SHOWN = [
  ['camera', CameraGlyph, '#30d158'], ['microphone', MicGlyph, '#ff9f0a'], ['screen', '▭', '#0a84ff'],
  ['files', FolderGlyph, '#30b0c7'], ['network', '⇅', '#0a84ff'], ['clipboard', '⎘', '#5e5ce6'],
  ['notifications', '◉', '#ff375f'], ['apps', '▦', '#8e8e93'], ['listen', '⇄', '#34c759'],
  ['usb', '⇆', '#8e8e93'], ['bluetooth', 'ᛒ', '#0a84ff'], ['status-item', '◔', '#8e8e93'], ['shortcuts', '❖', '#5e5ce6'],
];

// A Linux app's "shortcuts" name: the installed app's whose .desktop entry runs the program (or Flatpak app)
// lord-server checked it is (lord.wayland.shortcutsName reads the entries once per key), else the identity's short
// form ("flatpak:<id>", "program:<path>", server/src/wl/kbinhibit.h)
const linuxNames = new Map();
function linuxName(id, who) {
  if (!linuxNames.has(id)) {
    let n = '';
    try { n = (typeof lord !== 'undefined' && lord.wayland?.shortcutsName?.(id)) || ''; } catch (e) { /* no compositor */ }
    linuxNames.set(id, n);
  }
  return linuxNames.get(id) || who.short;
}

// A decision's app as the person knows it: a LordOS app's name, or a Linux app's for "shortcuts"
function nameOf(id, perm) {
  const who = perm === 'shortcuts' ? shortcutsWho(id) : null;
  if (who && who.kind !== 'lord') return linuxName(id, who);
  return appName(getApp(id)) || id;
}

// A decision this page lists: for "shortcuts", only those whose app lord-server can tell (the first format's
// "desktop:<app_id>" and "exe:<path>" were never released and are ignored)
const listed = (d) => d.perm !== 'shortcuts' || !!shortcutsWho(d.app);

const PermBadge = ({ glyph: G, color }) => (typeof G === 'string' ? <Badge glyph={G} color={color} size={22} /> : <Badge color={color} size={22}><G /></Badge>);

// Apps that declared a permission (manifest) or already have a saved decision.
function appsFor(perm, decisions) {
  const set = new Map();
  for (const p of packages.list()) if ((p.permissions || []).includes(perm)) set.set(p.id, p.name);
  for (const d of decisions) if (d.perm === perm && listed(d)) set.set(d.app, nameOf(d.app, perm));
  return [...set.entries()];
}

function PermSection({ perm, glyph, color }) {
  const { t: tr } = useLocale();
  useStore(permsRev);
  const decisions = perms.list();
  const apps = appsFor(perm, decisions);
  if (!apps.length) return null;
  return (
    <Section icon={<PermBadge glyph={glyph} color={color} />} title={tr(`privacy.perm.${perm}`)} footer={tr(`privacy.permHint.${perm}`)}>
      {apps.map(([id, name]) => (
        <SettingRow key={id} label={name} hint={perm === 'shortcuts' ? shortcutsWhoText(id) || undefined : undefined}>
          <Segmented small value={perms.get(id, perm)} onChange={(s) => perms.set(id, perm, s)}
            options={[['allow', tr('privacy.allowShort')], ['ask', tr('privacy.askShort')], ['deny', tr('privacy.denyShort')]]} />
        </SettingRow>
      ))}
    </Section>
  );
}

// What is using the camera or the microphone right now.
function InUse() {
  const { t: tr } = useLocale();
  const list = useIndicators();
  if (!list.length) return null;
  return (
    <Section>
      {list.map((d) => (
        <SettingRow key={d.kind} icon={<StatusDot color={d.kind === 'camera' ? tint.ok : tint.warn} />} label={`${tr(`privacy.inUse.${d.kind}`)}: ${d.apps.join(', ')}`} />
      ))}
    </Section>
  );
}

// The screen-sharing API's two permissions: decisions in a root-owned file (sdk/share.js), so they aren't in perms.
function ShareSection({ perm }) {
  const { t: tr } = useLocale();
  useStore(shareRev);
  const list = share.grants().filter((g) => g.perm === perm);
  if (!list.length) return null;
  return (
    <Section icon={<PermBadge glyph="▣" color="#ff453a" />} title={tr(`privacy.perm.${perm}`)} footer={tr(`privacy.permHint.${perm}`)}>
      {list.map((g) => (
        <SettingRow key={g.id} label={g.id.replace(/^(desktop|lpk):/, '')} hint={g.exe}>
          <Segmented value={g.state} onChange={(v) => share.set(g.id, perm, v, g.exe)}
            options={[['allow', tr('privacy.allowShort')], ['ask', tr('privacy.askShort')], ['deny', tr('privacy.denyShort')]]} />
        </SettingRow>
      ))}
    </Section>
  );
}

export function PrivacySettings() {
  const { t: tr } = useLocale();
  const sections = useStore(privacySections);
  return (
    <Stack>
      <Note style={{ fontSize: typeScale.callout }}>{tr('privacy.intro')}</Note>
      <InUse />
      {/* slots registered by other parts (e.g. the Monitor: its on/off switch and the apps' network) */}
      {sections.map((s) => <s.Component key={s.id} />)}
      {SHOWN.map(([perm, glyph, color]) => <PermSection key={perm} perm={perm} glyph={glyph} color={color} />)}
      <ShareSection perm="screen-share" />
      <ShareSection perm="remote-input" />
    </Stack>
  );
}
