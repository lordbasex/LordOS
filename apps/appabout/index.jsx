// About <app> (the app menu's first item, sdk/appcontrol.js): the app's icon, name, its own version
// (manifest "version"; a package's version), where it comes from and whether it runs isolated.
import { View, Text, useTheme, useAppWindow, useLauncherApps, getApp, wm, typeScale, isSystemApp, appName, t as tr } from '@lordos/sdk';
import { useEffect } from 'react';

export function Icon({ size }) {
  return (
    <View style={{ width: size, height: size, borderRadius: size * 0.22, backgroundColor: '#8e8e93', alignItems: 'center', justifyContent: 'center' }}>
      <Text style={{ fontSize: size * 0.55, fontWeight: 700, color: '#fff' }}>i</Text>
    </View>
  );
}

export default function AppAbout() {
  const t = useTheme();
  const { id, args } = useAppWindow();
  const key = String(args?.key || '');
  const apps = useLauncherApps();
  const a = apps.find((x) => x.id === key) || null;
  const own = getApp(key);
  const name = a ? a.name : own ? appName(own) : key;
  useEffect(() => { if (id) wm.setTitle(id, tr('appabout.title', { app: name })); }, [id, name]);
  const AppIcon = a?.Icon || own?.Icon;
  const kind = !a ? '' : a.kind === 'linux' ? tr('appabout.linux') : a.package ? tr('appabout.package') : isSystemApp(a.id) ? tr('appabout.system') : tr('appabout.lordos');
  const isolated = !!(own?.isolated || own?.package);
  const version = own?.version || a?.version || '';
  const desc = a?.comment || own?.description || a?.genericName || '';
  return (
    <View style={{ flex: 1, alignItems: 'center', paddingHorizontal: 22, paddingTop: 22, paddingBottom: 18, gap: 6 }}>
      {AppIcon ? <AppIcon size={64} /> : <Icon size={64} />}
      <Text style={{ fontSize: typeScale.title2, fontWeight: 700, color: t.text, marginTop: 6, textAlign: 'center' }}>{name}</Text>
      {version ? <Text style={{ fontSize: typeScale.callout, color: t.textDim }}>{tr('appabout.version', { v: version })}</Text> : null}
      {kind ? <Text style={{ fontSize: typeScale.caption, color: t.textDim, textAlign: 'center' }}>{kind}</Text> : null}
      {isolated ? <Text style={{ fontSize: typeScale.caption, color: t.textDim }}>{tr('appabout.isolated')}</Text> : null}
      {desc ? <Text style={{ fontSize: typeScale.callout, color: t.text, marginTop: 8, textAlign: 'center', lineHeight: 1.4, maxWidth: 280 }}>{desc}</Text> : null}
      <View style={{ flex: 1 }} />
      <Text style={{ fontSize: typeScale.caption2 || 10, color: t.textDim }}>{a?.kind === 'linux' ? String(key).replace(/^desktop:/, '') : key}</Text>
    </View>
  );
}
