// Force Quit (the crown menu, sdk/appcontrol.js): every app with a window; the chosen one ends without
// being asked. Linux and isolated apps get SIGKILL from lord-server (only their own process), built-in
// apps close without their close guard.
import { View, Text, Button, ScrollView, useTheme, useWindows, runningApps, forceQuit, windowIcon, errorText, typeScale, t as tr, SkinRegion, useSkinned, listPane, pickItem, commandBar, pickCommand } from '@lordos/sdk';
import { useState } from 'react';

export function Icon({ size }) {
  return (
    <View style={{ width: size, height: size, borderRadius: size * 0.22, backgroundColor: '#ff453a', alignItems: 'center', justifyContent: 'center' }}>
      <Text style={{ fontSize: size * 0.55, fontWeight: 700, color: '#fff' }}>!</Text>
    </View>
  );
}

export default function ForceQuit() {
  const t = useTheme();
  useWindows();                                  // the list follows windows opening and closing
  const list = runningApps();
  const [sel, setSel] = useState(null);
  const [err, setErr] = useState('');
  const chosen = list.find((g) => g.key === sel) ? sel : null;
  const quit = () => { if (!chosen) return; const e = forceQuit(chosen); setErr(e ? errorText(e) : ''); setSel(null); };
  // (0.8.9.6.2) app skins (docs/THEMES-GUIDE.md, "App skins → the shared shapes"): the running apps as a list pane and
  // Force Quit as a command bar; quitting is Force Quit's own (forceQuit, only an app in the list)
  const skinned = useSkinned('forcequit');
  const pane = skinned ? listPane({ title: tr('forcequit.intro'), items: list.map((g) => ({ key: g.key, label: g.name, detail: g.windows.length > 1 ? tr('forcequit.windows', { count: g.windows.length }) : '', selected: g.key === chosen })) }) : null;
  const bar = skinned ? commandBar({ commands: [{ id: 'quit', label: tr('forcequit.quit'), disabled: !chosen }] }) : null;
  const vm = pane ? { app: 'forcequit', ...pane, commands: bar.commands, error: err, actions: { select: (key) => { const k = pickItem(pane, key); if (k) setSel(k); }, command: (id) => { if (pickCommand(bar, id)) quit(); } } } : null;
  const own = (
    <View style={{ flex: 1, padding: 16, gap: 10 }}>
      <Text style={{ fontSize: typeScale.callout, color: t.text, textAlign: 'start', lineHeight: 1.4 }}>{tr('forcequit.intro')}</Text>
      <View style={{ flex: 1, borderRadius: 8, border: `1px solid ${t.separator}`, overflow: 'hidden' }}>
        <ScrollView style={{ flex: 1 }}>
          {list.length ? list.map((g) => {
            const I = windowIcon(g.windows[0]);
            const on = g.key === chosen;
            return (
              <View key={g.key} onClick={() => setSel(g.key)} onDoubleClick={() => { setSel(g.key); }}
                style={{ flexDirection: 'row', alignItems: 'center', gap: 10, height: 34, paddingHorizontal: 10, backgroundColor: on ? t.accent : 'transparent' }}>
                {I ? <I size={20} /> : <View style={{ width: 20 }} />}
                <Text style={{ flex: 1, fontSize: 13, color: on ? '#fff' : t.text, textAlign: 'start', whiteSpace: 'nowrap' }}>{g.name}</Text>
                {g.windows.length > 1 ? <Text style={{ fontSize: 11, color: on ? '#fff' : t.textDim }}>{tr('forcequit.windows', { count: g.windows.length })}</Text> : null}
              </View>
            );
          }) : <Text style={{ padding: 12, fontSize: 13, color: t.textDim }}>{tr('forcequit.none')}</Text>}
        </ScrollView>
      </View>
      {err ? <Text style={{ fontSize: typeScale.caption, color: '#ff9f0a', textAlign: 'start' }}>{err}</Text> : null}
      <View style={{ flexDirection: 'row', alignItems: 'center' }}>
        <Text style={{ flex: 1, fontSize: typeScale.caption, color: t.textDim, textAlign: 'start' }}>{tr('forcequit.shortcut')}</Text>
        <Button destructive label={tr('forcequit.quit')} disabled={!chosen} onClick={quit} />
      </View>
    </View>
  );
  return <SkinRegion app="forcequit" region="list" model={vm}>{own}</SkinRegion>;
}
