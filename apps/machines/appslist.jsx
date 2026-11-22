// Apps mode's lists (docs/VIRTUALIZATION.md 7.2), shared by the machine's controller panel (vmwindow.jsx AppsController)
// and its status item by the clock (appsstatus.jsx): the search field, Running (the guest's windows) and the guest's apps
// to open. g is what useAppsMode publishes for the machine (appsmode.jsx guests).
import { View, Text, Image, TextInput, LetterIcon, windowPixelIcons, useStore, t as tr } from '@lordos/sdk';
import { useState } from 'react';
import { runningWindows, appInitial } from './model.js';
import { focusGuest, closeGuest } from './appsmode.jsx';
import { usePal, IconBtn } from './ui.jsx';
import { Sym } from './kit.jsx';

// a section's title ("Running", "Open an app")
export function ListHead({ label }) {
  const p = usePal();
  return <Text style={{ fontSize: 11, fontWeight: 700, color: p.dim, textAlign: 'start', paddingHorizontal: 4 }}>{label}</Text>;
}

// the search over the guest's apps: Enter opens the first match, Esc clears it first and then calls onCancel
export function AppsSearch({ value, onChange, onSubmit, onCancel, autoFocus }) {
  const p = usePal();
  return (
    <View style={{ height: 34, flexDirection: 'row', alignItems: 'center', gap: 8, paddingHorizontal: 10, borderRadius: 9, backgroundColor: p.raised }}>
      <Sym name="search" size={16} color={p.dim} />
      <TextInput value={value} onChange={onChange} placeholder={tr('machines.apps.search')} size={13} autoFocus={autoFocus}
        onSubmit={onSubmit} onCancel={() => (value ? onChange('') : onCancel && onCancel())}
        style={{ flex: 1, minWidth: 0, backgroundColor: 'transparent', borderWidth: 0, border: 'none' }} />
    </View>
  );
}

const Empty = ({ text }) => {
  const p = usePal();
  return <Text style={{ fontSize: 12, color: p.dim, textAlign: 'start', padding: 8 }}>{text}</Text>;
};

// the guest's windows: a click brings one to the front, × asks the guest to close it; after: what else a click does (the
// status item's menu closes)
export function RunningList({ g, after }) {
  const running = runningWindows(g?.list);
  if (!running.length) return <Empty text={tr('machines.apps.noWindows')} />;
  return running.map((w) => <RunningRow key={w.id} w={w} winId={g?.wins?.[w.id] || 0}
    onFront={() => { focusGuest(g.dir, w.id); if (after) after(); }} onClose={() => closeGuest(g.dir, w.id)} />);
}

// the guest's apps (model.js launcherApps: the search's matches, each with its icon if the guest sent one)
export function LaunchList({ g, apps, onLaunch }) {
  if (!apps.length) return <Empty text={tr(g?.apps?.length ? 'machines.apps.noMatch' : 'machines.apps.noApps')} />;
  return apps.map((a) => <LaunchRow key={a.token} a={a} onClick={() => onLaunch(a)} />);
}

// a guest window in Running: its icon (the guest's, as the Dock shows it) and title
export function RunningRow({ w, winId, onFront, onClose }) {
  const p = usePal();
  const [hover, setHover] = useState(false);
  const px = useStore(windowPixelIcons, (m) => (winId ? m[winId] || null : null));
  const title = w.title || tr('machines.apps.untitled');
  return (
    <View onMouseEnter={() => setHover(true)} onMouseLeave={() => setHover(false)}
      style={{ height: 34, flexDirection: 'row', alignItems: 'center', gap: 4, paddingHorizontal: 6, borderRadius: 7, backgroundColor: hover ? p.hover : 'transparent' }}>
      <View onClick={onFront} title={title} style={{ flex: 1, minWidth: 0, height: 34, flexDirection: 'row', alignItems: 'center', gap: 8 }}>
        {px ? <Image pixels={px} style={{ width: 20, height: 20 }} /> : <LetterIcon size={20} name={w.app || title} />}
        <Text style={{ flex: 1, minWidth: 0, fontSize: 13, fontWeight: w.focused ? 700 : 400, color: w.minimized ? p.dim : p.text, textAlign: 'start', whiteSpace: 'nowrap', overflow: 'hidden' }}>{title}</Text>
      </View>
      <IconBtn glyph="×" size={15} label={tr('machines.apps.closeWindow')} onClick={onClose} />
    </View>
  );
}

// one of the guest's apps to open: its icon when the guest sent one (a.icon: pixels), else a plain tile with its initial
export function LaunchRow({ a, onClick }) {
  const p = usePal();
  const [hover, setHover] = useState(false);
  return (
    <View onClick={onClick} title={a.name} onMouseEnter={() => setHover(true)} onMouseLeave={() => setHover(false)}
      style={{ height: 32, flexDirection: 'row', alignItems: 'center', gap: 8, paddingHorizontal: 6, borderRadius: 7, backgroundColor: hover ? p.hover : 'transparent' }}>
      {a.icon ? <Image pixels={a.icon} style={{ width: 20, height: 20, objectFit: 'contain' }} />
        : <View style={{ width: 20, height: 20, borderRadius: 5, alignItems: 'center', justifyContent: 'center', backgroundColor: p.btn }}>
          <Text style={{ fontSize: 11, fontWeight: 700, color: p.dim }}>{appInitial(a.name)}</Text>
        </View>}
      <Text style={{ flex: 1, minWidth: 0, fontSize: 13, color: p.text, textAlign: 'start', whiteSpace: 'nowrap', overflow: 'hidden' }}>{a.name}</Text>
    </View>
  );
}
