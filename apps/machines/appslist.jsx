// Apps mode's lists (docs/VIRTUALIZATION.md 7.2), shared by the machine's controller panel (vmwindow.jsx AppsController)
// and its status item by the clock (appsstatus.jsx): the search field, Running (the guest's windows) and the guest's apps
// to open. g is what useAppsMode publishes for the machine (appsmode.jsx guests). An app's row says "Opening…" while its
// launch waits for a window (model.js launchPending), a window's row "Closing…" after × and a note when the guest didn't
// close it (closeView).
import { View, Text, Image, TextInput, LetterIcon, Spinner, windowPixelIcons, useStore, t as tr } from '@lordos/sdk';
import { useEffect, useState } from 'react';
import { runningWindows, appInitial, launchPending, closeView, LAUNCH_WAIT_MS, CLOSE_WAIT_MS } from './model.js';
import { focusGuest, closeGuest, launchGuest } from './appsmode.jsx';
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

// draws again once `at + ms` is reached (a waiting row turns into its next state with no new list from the guest)
function useWake(at, ms) {
  const [, setN] = useState(0);
  useEffect(() => {
    if (!at) return undefined;
    const left = at + ms - Date.now();
    if (left <= 0) return undefined;
    const id = setTimeout(() => setN((n) => n + 1), left + 20);
    return () => clearTimeout(id);
  }, [at, ms]);
}

// the guest's windows: a click brings one to the front, × asks the guest to close it; after: what else a click does (the
// status item's menu closes)
export function RunningList({ g, after }) {
  const running = runningWindows(g?.list);
  if (!running.length) return <Empty text={tr('machines.apps.noWindows')} />;
  return running.map((w) => <RunningRow key={w.id} g={g} w={w} winId={g?.wins?.[w.id] || 0}
    onFront={() => { focusGuest(g.dir, w.id); if (after) after(); }} onClose={() => closeGuest(g.dir, w.id)} />);
}

// the guest's apps (model.js launcherApps: the search's matches, each with its icon if the guest sent one); a click opens
// one (appsmode.jsx launchGuest: not again while it waits), then onLaunched(a)
export function LaunchList({ g, apps, onLaunched }) {
  if (!apps.length) return <Empty text={tr(g?.apps?.length ? 'machines.apps.noMatch' : 'machines.apps.noApps')} />;
  return apps.map((a) => <LaunchRow key={a.token} g={g} a={a} onClick={() => { if (g?.dir && launchGuest(g.dir, a) && onLaunched) onLaunched(a); }} />);
}
// the same for Enter in the search: the first match
export function launchFirst(g, apps, onLaunched) {
  const a = apps[0];
  if (a && g?.dir && launchGuest(g.dir, a) && onLaunched) onLaunched(a);
}

// a guest window in Running: its icon (the guest's, as the Dock shows it) and title; after ×, "Closing…", then a note
// when the guest didn't close it
export function RunningRow({ g, w, winId, onFront, onClose }) {
  const p = usePal();
  const [hover, setHover] = useState(false);
  const px = useStore(windowPixelIcons, (m) => (winId ? m[winId] || null : null));
  const entry = g?.closes?.[w.id] || null;
  useWake(entry?.at, CLOSE_WAIT_MS);
  const cv = closeView(entry, g?.list, g?.closeRefused?.[w.id], Date.now());
  const title = w.title || tr('machines.apps.untitled');
  const note = cv?.state === 'refused' ? tr(cv.reason === 'elevated' ? 'machines.win.closeElevated' : 'machines.apps.closeFailed') : '';
  return (
    <View onMouseEnter={() => setHover(true)} onMouseLeave={() => setHover(false)}
      style={{ paddingHorizontal: 6, borderRadius: 7, backgroundColor: hover ? p.hover : 'transparent' }}>
      <View style={{ height: 34, flexDirection: 'row', alignItems: 'center', gap: 4 }}>
        <View onClick={onFront} title={title} style={{ flex: 1, minWidth: 0, height: 34, flexDirection: 'row', alignItems: 'center', gap: 8 }}>
          {px ? <Image pixels={px} style={{ width: 20, height: 20 }} /> : <LetterIcon size={20} name={w.app || title} />}
          <Text style={{ flex: 1, minWidth: 0, fontSize: 13, fontWeight: w.focused ? 700 : 400, color: w.minimized || cv?.state === 'closing' ? p.dim : p.text, textAlign: 'start', whiteSpace: 'nowrap', overflow: 'hidden' }}>{title}</Text>
        </View>
        {cv?.state === 'closing'
          ? <View style={{ flexDirection: 'row', alignItems: 'center', gap: 6, paddingHorizontal: 4 }}><Spinner size={12} /><Text style={{ fontSize: 12, color: p.dim }}>{tr('machines.apps.closing')}</Text></View>
          : <IconBtn glyph="×" size={15} label={tr('machines.apps.closeWindow')} onClick={onClose} />}
      </View>
      {note ? <Text style={{ fontSize: 11, color: p.warn, textAlign: 'start', paddingInlineStart: 28, paddingBottom: 6 }}>{note}</Text> : null}
    </View>
  );
}

// one of the guest's apps to open: its icon when the guest sent one (a.icon: pixels), else a plain tile with its initial;
// "Opening…" while its launch waits for a window (a click then does nothing)
export function LaunchRow({ g, a, onClick }) {
  const p = usePal();
  const [hover, setHover] = useState(false);
  const entry = g?.launches?.[a.token] || null;
  useWake(entry?.at, LAUNCH_WAIT_MS);
  const busy = launchPending(entry, g?.list, Date.now());
  return (
    <View onClick={busy ? undefined : onClick} title={a.name} onMouseEnter={() => setHover(true)} onMouseLeave={() => setHover(false)}
      style={{ height: 32, flexDirection: 'row', alignItems: 'center', gap: 8, paddingHorizontal: 6, borderRadius: 7, backgroundColor: hover && !busy ? p.hover : 'transparent' }}>
      {a.icon ? <Image pixels={a.icon} style={{ width: 20, height: 20, objectFit: 'contain' }} />
        : <View style={{ width: 20, height: 20, borderRadius: 5, alignItems: 'center', justifyContent: 'center', backgroundColor: p.btn }}>
          <Text style={{ fontSize: 11, fontWeight: 700, color: p.dim }}>{appInitial(a.name)}</Text>
        </View>}
      <Text style={{ flex: 1, minWidth: 0, fontSize: 13, color: busy ? p.dim : p.text, textAlign: 'start', whiteSpace: 'nowrap', overflow: 'hidden' }}>{a.name}</Text>
      {busy ? <View style={{ flexDirection: 'row', alignItems: 'center', gap: 6 }}><Spinner size={12} /><Text style={{ fontSize: 12, color: p.dim }}>{tr('machines.apps.opening')}</Text></View> : null}
    </View>
  );
}
