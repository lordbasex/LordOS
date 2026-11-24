// The library (the approved mocks "Main" and "List", 2026-11-22): a toolbar (Machines settings, search, grid or list,
// New machine), chips that filter (all, running, Linux, the other system's, with their counts), the New machine card
// first, then a card per machine — its picture (live while its window shows it, the last one when it's off) with its
// state, CPU and memory while it runs, Start or Open, its name, one line of numbers and its ··· menu — or a row each
// in the list; machines running, memory and free disk at the bottom. Double-click or Return opens or starts; the
// arrows move the selection; right-click or ··· has the machine's menu; the order is in the View menu. FirstRun,
// Requirements and the Pi page are the other screens of this window.
import {
  View, Text, Image, ScrollView, TextInput, useAppWindow, useStore, createStore, admin, fs, wm, windows, revealPath, openFolder, ConfirmDialog, useSystemInfo, statusText, useSettings, settings,
  EmptyState, useNarrow, useWindowWidth, addListener, direction, Overlay, screen, t as tr, battery,
} from '@lordos/sdk';
import { useEffect, useRef, useState } from 'react';
import { canRun, sensorLines, cardLights, budgetLine, isolationGaps, stateView, actionsOf, reqRows, unreadableOf, missingFor, usage, summaryOf, doingOf, thumbSrc, systemOf, libView, filterMachines, sortMachines, noSuspend,
  chipCounts, libGrid, skeletonPlan, seenNext, specParts, restLine, gvtGpu, vcpuAdvice, LIB_VIEWS, LIB_SORTS, menuSpot, popMenuHeight, displayUnder } from './model.js';
import { Assistant } from './assistant.jsx';
import { Configure } from './configure.jsx';
import { Sheet, Sym } from './kit.jsx';
import { BUILTIN } from './profiles.js';
import { loadProfiles, storeRev } from './store.js';
import { usePal, has, gb, bytes, gib, OsMark, Bar, Btn, PopMenu, markColors, overlayRoot, vmRefusal, startPrep } from './ui.jsx';
import { readMeta, readCpuUse, isProtectedMachine } from './meta.js';
import { CloneSheet } from './clone.jsx';
import { withAsk } from './openwith.js';
import { libraryItems, groupMenu, groupShows } from './group.js';
import { GroupMark, TorChip, groupLine, groupStateOf, groupColors, startGroup, stopGroup, FreshDialog, GroupSnapDialog } from './groupui.jsx';

const winIdOf = (m) => `machines#vm-${m.id || m.folder}`;
// how long the Requirements page waits for a word from the package engine before it says the network isn't answering
const STALL_MS = 90000;
// Play asked of a window that's already open (dir → a counter): it starts its machine when it's off (vmwindow.jsx)
export const playAsks = createStore({});
// opens (or brings forward) the machine's window; a new one gets the guest's usual size and the middle of the screen and
// starts the machine itself; one already open is asked to (Play on a card while its window shows Off)
export function openMachine(m, opts) {
  const instance = `vm-${m.id || m.folder}`;
  const id = `machines#${instance}`;
  const fresh = !windows.get().some((w) => w.id === id);
  // a protected machine's window (the anonymity profile) is protected from its first frame: a neutral fill in every capture
  const prot = isProtectedMachine(m.dir, m);
  wm.open('machines', { instance, protected: prot, args: { machine: m.dir, name: m.name, ...(opts?.loadvm ? { loadvm: opts.loadvm } : {}) } });
  if (fresh) { wm.resize(id, 1024, 768 + 48); wm.center(id); }
  else playAsks.set((x) => ({ ...x, [m.dir]: (x[m.dir] || 0) + 1 }));
  // Start in Full Screen (5.3): the window takes its display as it opens
  if (opts?.full) wm.setFullscreen(id, true);
  return id;
}
// files sent from Files' "Open with" (openwith.js): dir → [{ id, path, at }], taken by the machine's window once its
// guest tools can open them
export const openAsks = createStore({});
let askSeq = 0;
export function openInMachine(m, path) {
  if (typeof path !== 'string' || !path.startsWith('/')) return false;
  openAsks.set((x) => withAsk(x, m.dir, path, ++askSeq, Date.now()));
  openMachine(m);
  return true;
}
export function openConfigure(m, section) {
  const instance = `cfg-${m.id || m.folder}`;
  const id = `machines#${instance}`;
  const fresh = !windows.get().some((w) => w.id === id);
  wm.open('machines', { instance, args: { configure: m.dir, name: m.name, section: section || 'general' } });
  if (fresh) { wm.resize(id, 980, 640); wm.center(id); }
}
export function openSnapshots(m) {
  const instance = `snap-${m.id || m.folder}`;
  const id = `machines#${instance}`;
  const fresh = !windows.get().some((w) => w.id === id);
  wm.open('machines', { instance, args: { snapshots: m.dir, name: m.name } });
  if (fresh) { wm.resize(id, 1100, 560); wm.center(id); }
}
export function openStorage() {
  const id = 'machines#storage';
  const fresh = !windows.get().some((w) => w.id === id);
  wm.open('machines', { instance: 'storage', args: { storage: true } });
  if (fresh) { wm.resize(id, 1040, 620); wm.center(id); }
}
export function openLogs(m) {
  const instance = `log-${m.id || m.folder}`;
  const id = `machines#${instance}`;
  const fresh = !windows.get().some((w) => w.id === id);
  wm.open('machines', { instance, args: { logs: m.dir, name: m.name } });
  if (fresh) { wm.resize(id, 980, 600); wm.center(id); }
}

// the status line under the library (the approved mock "Main"): machines running, the memory they use, free space (a
// click on it opens Storage)
function Summary({ label, value, onClick }) {
  const p = usePal();
  return (
    <View onClick={onClick} title={onClick ? tr('machines.storage.menu') : undefined} style={{ flexDirection: 'row', alignItems: 'center', gap: 6 }}>
      <Text style={{ fontSize: 12, color: p.dim, whiteSpace: 'nowrap' }}>{label}</Text>
      <Text style={{ fontSize: 12, fontWeight: 700, color: p.text, whiteSpace: 'nowrap' }}>{value}</Text>
    </View>
  );
}

// "10 CPU · 8 GB · 64 GB · Bridged · Intel card"
export const specLine = (m) => specParts(m).map((x) => (x.k === 'mem' ? gb(x.mb) : tr(`machines.lib.spec.${x.k}`, { n: x.n }))).join(' · ');
const pillStyle = { height: 24, paddingHorizontal: 10, borderRadius: 12, flexDirection: 'row', alignItems: 'center', gap: 6, backgroundColor: 'rgba(0,0,0,0.55)' };
// a machine's state in words and color: running (green), suspended (yellow), a failure (red), off
function stateOf(m, windowed) {
  const v = stateView(m.state, m.reason);
  const bad = unreadableOf(m);
  if (bad) return { text: tr(`machines.unreadable.${bad}`), color: '#ff9a9a', on: false };
  const d = doingOf(m, windowed);
  if (d === 'running') return { text: tr('machines.lib.state.running'), color: '#30d158', on: true };
  return { text: tr(`machines.doing.${d}`), color: v.why ? '#ff9a9a' : d === 'suspended' ? '#ffd60a' : v.active ? '#30d158' : null, on: v.active };
}
// A machine's menu over everything, not cut by the window (the shell's popover layer): below the ··· button or the
// pointer, above them when it doesn't fit, always on the screen (model.js menuSpot)
function CardMenu({ at, root, rl, items, onClose }) {
  const w = 232, spot = menuSpot(at, w, popMenuHeight(items), displayUnder(screen.displays, at.x, at.y) || { x: 0, y: 0, w: screen.width, h: screen.height });
  return (
    <Overlay anchor={root}>
      <View style={{ position: 'absolute', left: spot.left - rl[0], top: spot.top - rl[1], width: w }}>
        <PopMenu x={0} y={0} width={w} items={items} onClose={onClose} />
      </View>
    </Overlay>
  );
}

// ··· : the machine's menu, at the button
function MoreBtn({ m, onMenu }) {
  const p = usePal();
  const ref = useRef(null);
  const [hover, setHover] = useState(false);
  return (
    <View ref={ref} title={tr('machines.lib.moreFor', { name: m.name })} onMouseEnter={() => setHover(true)} onMouseLeave={() => setHover(false)}
      onClick={(e) => { e.stopPropagation?.(); const r = ref.current ? lord.getLayout(ref.current.id) : [e.x, e.y, 0, 0]; onMenu({ x: r[0], y: r[1] + r[3] + 4, above: r[1] - 4 }); }}
      style={{ width: 34, height: 34, borderRadius: 17, alignItems: 'center', justifyContent: 'center', border: `1.5px solid ${p.accent}`,
        backgroundColor: hover ? (p.dark ? 'rgba(255,255,255,0.08)' : 'rgba(0,0,0,0.05)') : 'transparent' }}>
      <Text style={{ fontSize: 15, fontWeight: 700, color: p.accent, marginTop: -4 }}>···</Text>
    </View>
  );
}
// a machine's picture: its last one (live while its window shows it), else its system's colors and mark
function Picture({ m, sys, on, height, radius = 0 }) {
  // a protected machine has no card picture: lord.vm.protect removed it and never writes another; one left from before
  // isn't shown either
  const src = isProtectedMachine(m.dir, m) ? '' : thumbSrc(m.dir, m.thumbTime);
  const [c1, c2] = markColors(sys.id);
  return (
    <View style={{ height, borderRadius: radius, overflow: 'hidden', ...(src ? { backgroundColor: '#000' } : { background: `linear-gradient(135deg, ${c2}, ${c1})` }) }}>
      {src ? <Image src={src} style={{ position: 'absolute', left: 0, top: 0, right: 0, bottom: 0, objectFit: 'cover', opacity: on ? 1 : 0.72 }} /> : (
        <View style={{ position: 'absolute', left: 0, right: 0, top: 0, bottom: 0, alignItems: 'center', justifyContent: 'center' }}><OsMark id={sys.id} size={Math.min(56, height * 0.4)} /></View>
      )}
    </View>
  );
}
const sysOf = (m) => (m.meta?.os ? { id: m.meta.os, name: m.meta.osName || '' } : systemOf(m.name));

// the vCPU recommendation's line under the numbers (Configure → CPU & memory has the reason and "Use N")
function VcpuHint({ m, host }) {
  const p = usePal();
  const adv = host ? vcpuAdvice(m.cpuWindows, m.vcpus, host) : null;
  const next = m.meta?.nextVcpus && m.meta.nextVcpus !== m.vcpus ? m.meta.nextVcpus : 0;
  if (!adv && !next) return null;
  return (
    <View onClick={(e) => { e.stopPropagation?.(); openConfigure(m, 'cpu'); }} style={{ marginTop: 4, alignSelf: 'flex-start' }}>
      <Text style={{ fontSize: 12, color: p.accent, whiteSpace: 'nowrap', overflow: 'hidden', textAlign: 'start' }}>
        {next ? tr('machines.cfg.vcpu.pending', { n: next }) : tr('machines.lib.vcpuHint', { n: adv.n })}</Text>
    </View>
  );
}

// The agent and disk lights (5.1, model.js cardLights): a symbol in the light's color, its words beside it on a wide
// card and always in the tooltip, so no light carries meaning by color alone
const LIGHT_TONE = { on: '#30d158', wait: '#ffd60a', off: 'rgba(242,242,245,0.45)' };
const LIGHT_ICON = { agent: 'agent', disk: 'disks' };
function Lights({ m, u, words }) {
  const ls = cardLights(m, u);
  if (!ls.length) return null;
  return (
    <View style={{ flexDirection: 'row', alignItems: 'center', gap: 8 }}>
      {ls.map((l) => (
        <View key={l.k} title={tr(l.key)} style={{ flexDirection: 'row', alignItems: 'center', gap: 4 }}>
          <Sym name={LIGHT_ICON[l.k]} size={13} color={LIGHT_TONE[l.tone]} />
          {words ? <Text style={{ fontSize: 12, color: '#f2f2f5', whiteSpace: 'nowrap' }}>{tr(l.key)}</Text> : null}
        </View>
      ))}
    </View>
  );
}

function Card({ m, u, sel, onSelect, onMenu, windowed, picH, gpuName, host }) {
  const p = usePal();
  const a = actionsOf(m);
  const st = stateOf(m, windowed);
  const sys = sysOf(m);
  const go = () => (a.open || a.start ? openMachine(m) : null);
  const [hover, setHover] = useState(false);
  return (
    <View onClick={onSelect} onDoubleClick={go} onMouseEnter={() => setHover(true)} onMouseLeave={() => setHover(false)}
      onContextMenu={(e) => { e.stopPropagation?.(); onSelect(); onMenu(e); }}
      style={{ flex: 1, minWidth: 0, borderRadius: 16, overflow: 'hidden', backgroundColor: p.card, border: `1px solid ${sel ? p.accent : p.line}` }}>
      <View style={{ height: picH }}>
        <Picture m={m} sys={sys} on={st.on} height={picH} />
        <View style={{ ...pillStyle, position: 'absolute', insetInlineStart: 12, top: 12 }}>
          {st.color ? <View style={{ width: 8, height: 8, borderRadius: 4, backgroundColor: st.color }} /> : null}
          <Text style={{ fontSize: 12, fontWeight: 600, color: '#f2f2f5', whiteSpace: 'nowrap' }}>{st.text}</Text>
        </View>
        {st.on && u ? (
          <View style={{ ...pillStyle, position: 'absolute', insetInlineEnd: 12, top: 12 }}>
            <Text style={{ fontSize: 12, color: '#f2f2f5', whiteSpace: 'nowrap' }}>{tr('machines.lib.usage', { cpu: u.cpu != null ? Math.round(u.cpu) : '—', mem: u.memBytes ? gib(u.memBytes) : '—' })}</Text>
          </View>
        ) : !st.on && m.gpu && gpuName ? (
          <View style={{ ...pillStyle, position: 'absolute', insetInlineEnd: 12, top: 12 }}>
            <Text style={{ fontSize: 12, color: '#f2f2f5', whiteSpace: 'nowrap' }}>{gpuName}</Text>
          </View>
        ) : null}
        {st.on ? <View style={{ ...pillStyle, position: 'absolute', insetInlineStart: 12, bottom: 12 }}><Lights m={m} u={u} words={picH >= 150} /></View> : null}
        {a.open || a.start ? (
          <View style={{ position: 'absolute', left: 0, right: 0, top: 0, bottom: 0, alignItems: 'center', justifyContent: 'center' }}>
            {a.open ? (
              <View onClick={(e) => { e.stopPropagation?.(); go(); }} title={tr('machines.open')}
                style={{ height: 40, paddingHorizontal: 18, borderRadius: 20, justifyContent: 'center', backgroundColor: hover ? 'rgba(0,0,0,0.75)' : 'rgba(0,0,0,0.6)' }}>
                <Text style={{ fontSize: 14, fontWeight: 600, color: '#fff' }}>{tr('machines.open')}</Text>
              </View>
            ) : (
              <View onClick={(e) => { e.stopPropagation?.(); go(); }} title={tr(m.suspended ? 'machines.lib.resumeName' : 'machines.lib.startName', { name: m.name })}
                style={{ width: 64, height: 64, borderRadius: 32, alignItems: 'center', justifyContent: 'center', backgroundColor: hover ? 'rgba(0,0,0,0.72)' : 'rgba(0,0,0,0.55)' }}>
                <Sym name="play" size={26} color="#ffffff" />
              </View>
            )}
          </View>
        ) : null}
      </View>
      <View style={{ flexDirection: 'row', alignItems: 'flex-start', gap: 8, paddingHorizontal: 18, paddingTop: 14, paddingBottom: 18 }}>
        <View style={{ flex: 1, minWidth: 0 }}>
          <Text style={{ fontSize: 18, fontWeight: 600, color: p.text, whiteSpace: 'nowrap', overflow: 'hidden', textAlign: 'start' }}>{m.name}</Text>
          <Text style={{ marginTop: 4, fontSize: 13, color: p.dim, whiteSpace: 'nowrap', overflow: 'hidden', textAlign: 'start' }}>{specLine(m)}</Text>
          <VcpuHint m={m} host={host} />
        </View>
        <MoreBtn m={m} onMenu={onMenu} />
      </View>
    </View>
  );
}

// A machine group's ONE card (docs/VIRTUALIZATION.md 19.8, the approved mock): the group's mark (its members are
// protected machines: no picture of them is kept), its state, a "Tor" chip with the gateway's dot, Start or Open (the
// workstation's window), its name and "Group of 2 machines · CPU · memory", its ··· menu
function GroupPicture({ height, on, radius = 0 }) {
  return (
    <View style={{ height, borderRadius: radius, overflow: 'hidden', background: `linear-gradient(135deg, ${groupColors[0]}, ${groupColors[1]})`, opacity: on ? 1 : 0.85,
      alignItems: 'center', justifyContent: 'center' }}>
      <GroupMark size={Math.min(56, height * 0.4)} />
    </View>
  );
}
const gwOn = (item) => !!item.gw && stateView(item.gw.state, item.gw.reason).active;
function GroupCard({ item, sel, onSelect, onMenu, onGo, picH }) {
  const p = usePal();
  const st = groupStateOf(item);
  const wsOn = !!item.ws && stateView(item.ws.state, item.ws.reason).active;
  const [hover, setHover] = useState(false);
  const m = { name: item.group.name };
  return (
    <View onClick={onSelect} onDoubleClick={onGo} onMouseEnter={() => setHover(true)} onMouseLeave={() => setHover(false)}
      onContextMenu={(e) => { e.stopPropagation?.(); onSelect(); onMenu(e); }}
      style={{ flex: 1, minWidth: 0, borderRadius: 16, overflow: 'hidden', backgroundColor: p.card, border: `1px solid ${sel ? p.accent : p.line}` }}>
      <View style={{ height: picH }}>
        <GroupPicture height={picH} on={st.on} />
        <View style={{ ...pillStyle, position: 'absolute', insetInlineStart: 12, top: 12 }}>
          {st.color ? <View style={{ width: 8, height: 8, borderRadius: 4, backgroundColor: st.color }} /> : null}
          <Text style={{ fontSize: 12, fontWeight: 600, color: '#f2f2f5', whiteSpace: 'nowrap' }}>{st.text}</Text>
        </View>
        <View style={{ position: 'absolute', insetInlineEnd: 12, top: 13 }}><TorChip on={gwOn(item)} /></View>
        {item.state !== 'damaged' ? (
          <View style={{ position: 'absolute', left: 0, right: 0, top: 0, bottom: 0, alignItems: 'center', justifyContent: 'center' }}>
            {wsOn ? (
              <View onClick={(e) => { e.stopPropagation?.(); onGo(); }} title={tr('machines.open')}
                style={{ height: 40, paddingHorizontal: 18, borderRadius: 20, justifyContent: 'center', backgroundColor: hover ? 'rgba(0,0,0,0.75)' : 'rgba(0,0,0,0.6)' }}>
                <Text style={{ fontSize: 14, fontWeight: 600, color: '#fff' }}>{tr('machines.open')}</Text>
              </View>
            ) : (
              <View onClick={(e) => { e.stopPropagation?.(); onGo(); }} title={tr('machines.lib.startName', { name: item.group.name })}
                style={{ width: 64, height: 64, borderRadius: 32, alignItems: 'center', justifyContent: 'center', backgroundColor: hover ? 'rgba(0,0,0,0.72)' : 'rgba(0,0,0,0.55)' }}>
                <Sym name="play" size={26} color="#ffffff" />
              </View>
            )}
          </View>
        ) : null}
      </View>
      <View style={{ flexDirection: 'row', alignItems: 'flex-start', gap: 8, paddingHorizontal: 18, paddingTop: 14, paddingBottom: 18 }}>
        <View style={{ flex: 1, minWidth: 0 }}>
          <Text style={{ fontSize: 18, fontWeight: 600, color: p.text, whiteSpace: 'nowrap', overflow: 'hidden', textAlign: 'start' }}>{item.group.name}</Text>
          <Text style={{ marginTop: 4, fontSize: 13, color: p.dim, whiteSpace: 'nowrap', overflow: 'hidden', textAlign: 'start' }}>{groupLine(item)}</Text>
          {item.group.disposable ? <Text style={{ marginTop: 4, fontSize: 12, color: p.accent, whiteSpace: 'nowrap', overflow: 'hidden', textAlign: 'start' }}>{tr('machines.group.disposableOn')}</Text> : null}
        </View>
        <MoreBtn m={m} onMenu={onMenu} />
      </View>
    </View>
  );
}
function GroupRow({ item, sel, onSelect, onMenu, onGo }) {
  const p = usePal();
  const st = groupStateOf(item);
  const narrow = useNarrow();
  const wsOn = !!item.ws && stateView(item.ws.state, item.ws.reason).active;
  return (
    <View onClick={onSelect} onDoubleClick={onGo} onContextMenu={(e) => { e.stopPropagation?.(); onSelect(); onMenu(e); }}
      style={{ flexDirection: 'row', alignItems: 'center', gap: 18, padding: 12, borderRadius: 14, backgroundColor: p.card, border: `1px solid ${sel ? p.accent : p.line}` }}>
      <View style={{ width: narrow ? 96 : 132, height: narrow ? 58 : 78 }}><GroupPicture height={narrow ? 58 : 78} on={st.on} radius={10} /></View>
      <View style={{ flex: 1, minWidth: 0 }}>
        <View style={{ flexDirection: 'row', alignItems: 'center', gap: 10 }}>
          <Text style={{ flexShrink: 1, fontSize: 17, fontWeight: 600, color: p.text, whiteSpace: 'nowrap', overflow: 'hidden', textAlign: 'start' }}>{item.group.name}</Text>
          <TorChip on={gwOn(item)} />
          <Text style={{ fontSize: 12, color: st.color || p.dim, whiteSpace: 'nowrap' }}>{st.text}</Text>
        </View>
        <Text style={{ marginTop: 4, fontSize: 13, color: p.dim, whiteSpace: 'nowrap', overflow: 'hidden', textAlign: 'start' }}>{groupLine(item)}</Text>
      </View>
      {item.state !== 'damaged' ? (
        <View onClick={(e) => { e.stopPropagation?.(); onGo(); }} style={{ height: 34, paddingHorizontal: 16, borderRadius: 17, justifyContent: 'center', backgroundColor: wsOn ? p.accent : p.btn }}>
          <Text style={{ fontSize: 13, fontWeight: 600, color: wsOn ? '#fff' : p.text }}>{tr(wsOn ? 'machines.open' : 'machines.start')}</Text>
        </View>
      ) : null}
      <MoreBtn m={{ name: item.group.name }} onMenu={onMenu} />
    </View>
  );
}

// the first card: New machine
function NewCard({ onClick, picH, row }) {
  const p = usePal();
  const [hover, setHover] = useState(false);
  const plus = (s) => (
    <View style={{ width: s, height: s, borderRadius: s / 2, alignItems: 'center', justifyContent: 'center', backgroundColor: p.dark ? '#f2f2f5' : '#2b2b31' }}>
      <Text style={{ fontSize: s * 0.55, fontWeight: 300, color: p.dark ? '#18181c' : '#fff', marginTop: -s * 0.06 }}>+</Text>
    </View>
  );
  const text = (
    <View style={{ flex: row ? 1 : undefined, minWidth: 0 }}>
      <Text style={{ fontSize: row ? 17 : 18, fontWeight: 600, color: p.text, textAlign: 'start' }}>{tr('machines.newMachine')}</Text>
      <Text style={{ marginTop: 4, fontSize: 13, color: p.dim, textAlign: 'start', whiteSpace: 'nowrap', overflow: 'hidden' }}>{tr('machines.win.lib.newHint')}</Text>
    </View>
  );
  const box = { borderRadius: row ? 14 : 16, overflow: 'hidden', backgroundColor: hover ? p.raised : p.card, border: `1px dashed ${p.dark ? '#4a4a55' : 'rgba(0,0,0,0.25)'}` };
  if (row) return (
    <View onClick={onClick} onMouseEnter={() => setHover(true)} onMouseLeave={() => setHover(false)} style={{ ...box, flexDirection: 'row', alignItems: 'center', gap: 18, padding: 12 }}>
      <View style={{ width: 132, height: 78, borderRadius: 10, alignItems: 'center', justifyContent: 'center', backgroundColor: p.raised }}>{plus(32)}</View>
      {text}
    </View>
  );
  return (
    <View onClick={onClick} onMouseEnter={() => setHover(true)} onMouseLeave={() => setHover(false)} style={{ ...box, flex: 1, minWidth: 0 }}>
      <View style={{ height: picH, alignItems: 'center', justifyContent: 'center', backgroundColor: p.raised }}>{plus(72)}</View>
      <View style={{ paddingHorizontal: 18, paddingTop: 14, paddingBottom: 18 }}>{text}</View>
    </View>
  );
}

// one machine in the list view (the approved mock "List"): picture, name and its state in color, the numbers line, CPU
// and memory while it runs (else since when it's off), Start or Open, ···
function ListRow({ m, u, sel, onSelect, onMenu, windowed, host }) {
  const p = usePal();
  const a = actionsOf(m);
  const st = stateOf(m, windowed);
  const sys = sysOf(m);
  const go = () => (a.open || a.start ? openMachine(m) : null);
  const narrow = useNarrow();       // a small window: no CPU and memory column
  const rest = st.on ? null : restLine(m);
  const restText = !rest ? '' : rest.k === 'profile' ? tr('machines.lib.rest.profile', { name: profileName(rest.id) }) : tr(`machines.lib.rest.${rest.k}`, { n: rest.n });
  return (
    <View onClick={onSelect} onDoubleClick={go} onContextMenu={(e) => { e.stopPropagation?.(); onSelect(); onMenu(e); }}
      style={{ flexDirection: 'row', alignItems: 'center', gap: 18, padding: 12, borderRadius: 14, backgroundColor: p.card, border: `1px solid ${sel ? p.accent : p.line}` }}>
      <View style={{ width: narrow ? 96 : 132, height: narrow ? 58 : 78 }}><Picture m={m} sys={sys} on={st.on} height={narrow ? 58 : 78} radius={10} /></View>
      <View style={{ flex: 1, minWidth: 0 }}>
        <View style={{ flexDirection: 'row', alignItems: 'center', gap: 10 }}>
          <Text style={{ flexShrink: 1, fontSize: 17, fontWeight: 600, color: p.text, whiteSpace: 'nowrap', overflow: 'hidden', textAlign: 'start' }}>{m.name}</Text>
          <View style={{ flexDirection: 'row', alignItems: 'center', gap: 6 }}>
            {st.on && st.color ? <View style={{ width: 8, height: 8, borderRadius: 4, backgroundColor: st.color }} /> : null}
            <Text style={{ fontSize: 12, color: st.color || p.dim, whiteSpace: 'nowrap' }}>{st.text}</Text>
          </View>
          {st.on ? <View style={{ ...pillStyle }}><Lights m={m} u={u} /></View> : null}
        </View>
        <Text style={{ marginTop: 4, fontSize: 13, color: p.dim, whiteSpace: 'nowrap', overflow: 'hidden', textAlign: 'start' }}>{specLine(m)}</Text>
        <VcpuHint m={m} host={host} />
      </View>
      {narrow ? null : st.on ? (
        <View style={{ width: 180, gap: 6 }}>
          <Text style={{ fontSize: 12, color: p.dim, textAlign: 'start' }}>{tr('machines.card.cpu', { v: u?.cpu != null ? `${Math.round(u.cpu)} %` : '—' })}</Text>
          <Bar frac={u?.cpu != null ? u.cpu / 100 : 0} color={p.accent} height={4} />
          <Text style={{ fontSize: 12, color: p.dim, textAlign: 'start' }}>{tr('machines.lib.memOf', { a: u?.memBytes ? gib(u.memBytes) : '—', b: gb(m.memoryMb) })}</Text>
          <Bar frac={u?.memBytes ? u.memBytes / (m.memoryMb * 1048576) : 0} color={p.cpu} height={4} />
        </View>
      ) : <Text style={{ width: 180, fontSize: 12, color: p.dim, textAlign: 'start' }}>{restText}</Text>}
      {a.open ? (
        <View onClick={(e) => { e.stopPropagation?.(); go(); }} style={{ height: 34, paddingHorizontal: 16, borderRadius: 17, justifyContent: 'center', backgroundColor: p.accent }}>
          <Text style={{ fontSize: 13, fontWeight: 600, color: '#fff' }}>{tr('machines.open')}</Text>
        </View>
      ) : (
        <View onClick={(e) => { e.stopPropagation?.(); go(); }} title={tr(m.suspended ? 'machines.lib.resumeName' : 'machines.lib.startName', { name: m.name })}
          style={{ width: 34, height: 34, borderRadius: 17, alignItems: 'center', justifyContent: 'center', opacity: a.start ? 1 : 0.4, backgroundColor: p.dark ? '#30303a' : 'rgba(0,0,0,0.08)' }}>
          <Sym name="play" size={16} color={p.dark ? '#ffffff' : p.text} />
        </View>
      )}
      <MoreBtn m={m} onMenu={onMenu} />
    </View>
  );
}

// The library's placeholders until its first read is in (model.js skeletonPlan): cards or rows of the real ones' size,
// muted blocks that pulse slowly, still with Reduce motion. A block's height is a line of the real text's size (an
// invisible character in it), so nothing moves when the machines replace them.
function usePulse() {
  const still = useStore(settings, (s) => !!s.reduceMotion);
  const [on, setOn] = useState(false);
  useEffect(() => {
    if (still) return undefined;
    const id = setInterval(() => setOn((v) => !v), 900);
    return () => clearInterval(id);
  }, [still]);
  return still ? 0.8 : on ? 0.55 : 1;
}
function SkelLine({ size, width, top = 0, op }) {
  const p = usePal();
  return (
    <View style={{ width, marginTop: top, borderRadius: 6, backgroundColor: p.btn, opacity: op }}>
      <Text style={{ fontSize: size, color: 'transparent', whiteSpace: 'nowrap' }}>{' '}</Text>
    </View>
  );
}
function SkelCard({ picH, op }) {
  const p = usePal();
  return (
    <View style={{ flex: 1, minWidth: 0, borderRadius: 16, overflow: 'hidden', backgroundColor: p.card, border: `1px solid ${p.line}` }}>
      <View style={{ height: picH, backgroundColor: p.raised, opacity: op }} />
      <View style={{ flexDirection: 'row', alignItems: 'flex-start', gap: 8, paddingHorizontal: 18, paddingTop: 14, paddingBottom: 18 }}>
        <View style={{ flex: 1, minWidth: 0 }}>
          <SkelLine size={18} width="45%" op={op} />
          <SkelLine size={13} width="75%" top={4} op={op} />
        </View>
        <View style={{ width: 34, height: 34, borderRadius: 17, backgroundColor: p.btn, opacity: op }} />
      </View>
    </View>
  );
}
function SkelRow({ op }) {
  const p = usePal();
  const narrow = useNarrow();
  return (
    <View style={{ flexDirection: 'row', alignItems: 'center', gap: 18, padding: 12, borderRadius: 14, backgroundColor: p.card, border: `1px solid ${p.line}` }}>
      <View style={{ width: narrow ? 96 : 132, height: narrow ? 58 : 78, borderRadius: 10, backgroundColor: p.raised, opacity: op }} />
      <View style={{ flex: 1, minWidth: 0 }}>
        <SkelLine size={17} width="40%" op={op} />
        <SkelLine size={13} width="70%" top={4} op={op} />
      </View>
      {narrow ? null : <View style={{ width: 180 }}><SkelLine size={12} width="60%" op={op} /></View>}
      <View style={{ width: 34, height: 34, borderRadius: 17, backgroundColor: p.btn, opacity: op }} />
      <View style={{ width: 34, height: 34, borderRadius: 17, backgroundColor: p.btn, opacity: op }} />
    </View>
  );
}
// the chips' row while loading: four muted pills of their height
function SkelChips({ pad }) {
  const p = usePal();
  const op = usePulse();
  return (
    <View style={{ flexDirection: 'row', flexWrap: 'wrap', gap: 8, paddingHorizontal: pad, paddingTop: 8, paddingBottom: 4 }}>
      {[64, 96, 80, 104].map((w, i) => <View key={i} style={{ width: w, height: 30, borderRadius: 15, backgroundColor: p.raised, opacity: op }} />)}
    </View>
  );
}
// the grid or the list as the person left it, the New machine card real (it needs nothing), n placeholders after it
function Skeleton({ view, n, grid, onNew }) {
  const op = usePulse();
  const label = tr('machines.lib.loading');
  if (view === 'list') return (
    <ScrollView style={{ flex: 1 }} contentStyle={{ paddingHorizontal: grid.pad, paddingVertical: 16, gap: 12 }}>
      <NewCard row onClick={onNew} />
      <View accessibilityLabel={label} style={{ gap: 12 }}>{Array.from({ length: n }, (_, i) => <SkelRow key={i} op={op} />)}</View>
    </ScrollView>
  );
  const cells = [{ isNew: true }, ...Array.from({ length: n }, (_, i) => ({ k: i }))];
  const rows = [];
  for (let i = 0; i < cells.length; i += grid.cols) rows.push(cells.slice(i, i + grid.cols));
  return (
    <ScrollView style={{ flex: 1 }} contentStyle={{ paddingHorizontal: grid.pad, paddingVertical: 16, gap: grid.gap }}>
      {rows.map((r, i) => (
        <View key={i} accessibilityLabel={i ? undefined : label} style={{ flexDirection: 'row', gap: grid.gap }}>
          {r.map((c) => (c.isNew ? <NewCard key="new" picH={grid.picH} onClick={onNew} /> : <SkelCard key={c.k} picH={grid.picH} op={op} />))}
          {Array.from({ length: grid.cols - r.length }, (_, k) => <View key={`f${k}`} style={{ flex: 1 }} />)}
        </View>
      ))}
    </ScrollView>
  );
}

// FirstRun (empty library) and its "one step first" side when virtualization isn't usable yet
function FirstRun({ host, onNew, onImport, onCheck, onCanRun }) {
  const p = usePal();
  const info = useSystemInfo(60000);
  const [busy, setBusy] = useState(false);
  const [msg, setMsg] = useState('');
  const allow = async () => {
    setBusy(true); setMsg('');
    try { setMsg(statusText(await admin.run('vm-kvm', ''))); } catch (e) { setMsg(e.message); }
    setBusy(false); onCheck();
  };
  const side = host.kvmDevice && !host.kvm ? 'allow' : !host.kvmDevice ? 'off' : null;
  const narrow = useNarrow();       // a small window: the two panes one above the other
  return (
    <View style={{ flex: 1, flexDirection: narrow ? 'column' : 'row', gap: 16, padding: 16 }}>
      <View style={{ flex: 1, padding: narrow ? 16 : 28, alignItems: 'center', justifyContent: 'center', gap: 10, borderRadius: 18, backgroundColor: side ? p.raised : 'transparent' }}>
        <AppMark size={96} />
        <Text style={{ marginTop: 8, fontSize: 20, fontWeight: 700, color: p.text }}>{tr('machines.empty')}</Text>
        <Text style={{ maxWidth: 420, fontSize: 13, lineHeight: 1.5, color: p.dim, textAlign: 'center' }}>{tr('machines.first.addHint')}</Text>
        <View style={{ flexDirection: 'row', gap: 8, marginTop: 6 }}>
          <Btn primary label={tr('machines.newMachine')} onClick={onNew} />
          <Btn label={tr('machines.first.import')} onClick={onImport} />
          {onCanRun ? <Btn label={tr('machines.pi.menu')} onClick={onCanRun} /> : null}
        </View>
      </View>
      {side ? (
        <View style={{ flex: 1, padding: narrow ? 16 : 28, gap: 12, borderRadius: 18, backgroundColor: p.raised }}>
          {side === 'allow' ? <>
            <Text style={{ fontSize: 20, fontWeight: 700, color: p.text, textAlign: 'start' }}>{tr('machines.first.stepTitle')}</Text>
            <Text style={{ fontSize: 14, lineHeight: 1.5, color: p.dim, textAlign: 'start' }}>{tr('machines.first.stepText')}</Text>
            <Btn big primary label={busy ? tr('machines.req.allowing') : tr('machines.first.turnOn')} disabled={busy || !admin.available() || !info.admin} onClick={allow} style={{ alignSelf: 'flex-start' }} />
            {msg ? <Text style={{ fontSize: 13, color: p.dim, textAlign: 'start' }}>{msg}</Text> : null}
          </> : <>
            <Text style={{ fontSize: 20, fontWeight: 700, color: p.text, textAlign: 'start' }}>{tr('machines.first.offTitle')}</Text>
            <Text style={{ fontSize: 14, lineHeight: 1.5, color: p.dim, textAlign: 'start' }}>{tr('machines.first.offText')}</Text>
            <View style={{ padding: 12, borderRadius: 10, backgroundColor: p.warnBg }}><Text style={{ fontSize: 13, lineHeight: 1.5, color: p.text, textAlign: 'start' }}>{tr('machines.first.emulated')}</Text></View>
            <View style={{ flexDirection: 'row', gap: 8 }}><Btn big label={tr('machines.first.checkAgain')} onClick={onCheck} /><Btn big label={tr('machines.first.continueEmulated')} onClick={onNew} /></View>
          </>}
        </View>
      ) : null}
    </View>
  );
}

export function AppMark({ size: s }) {
  return (
    <View style={{ width: s, height: s, borderRadius: s * 0.225, background: 'linear-gradient(180deg, #8aa4ff, #2f3fb8)' }}>
      <View style={{ position: 'absolute', left: s * 0.2, top: s * 0.23, width: s * 0.6, height: s * 0.45, borderRadius: s * 0.07, backgroundColor: '#fff' }} />
      <View style={{ position: 'absolute', left: s * 0.3, top: s * 0.32, width: s * 0.4, height: s * 0.27, borderRadius: s * 0.06, background: 'linear-gradient(160deg, #5ad1a8, #2a8f7a)' }} />
      <View style={{ position: 'absolute', left: s * 0.36, top: s * 0.74, width: s * 0.28, height: s * 0.05, borderRadius: s * 0.025, backgroundColor: '#fff' }} />
    </View>
  );
}

// Requirements: what this computer has for machines, with the closed admin action that installs what's missing
export function Requirements({ host, onBack, onChanged, archs }) {
  const p = usePal();
  const info = useSystemInfo(60000);
  const [busy, setBusy] = useState(false);
  const [msg, setMsg] = useState(null);
  const list = archs || [host.arch];
  const missing = missingFor(host, list, { windows: true });
  const rows = reqRows(host, list, missing);
  const install = async () => {
    setBusy(true); setMsg(null);
    // a network that doesn't answer: the engine gives up on its own, but the page never waits more than STALL_MS
    // without a word from it
    let last = Date.now(), stalled = false;
    // the engine's progress lines ("progress lists 1 5", "progress download 1048576 2097152") said in words
    const said = (line) => {
      last = Date.now();
      const m = /^progress (\w+) (\d+) (\d+)/.exec(String(line || ''));
      const mb = (x) => Math.round(Number(x) / 1048576 * 10) / 10;
      setMsg({ ok: true, text: !m ? statusText(line) : m[1] === 'download' ? tr('machines.req.progress.download', { a: mb(m[2]), b: mb(m[3]) })
        : m[1] === 'lists' ? tr('machines.req.progress.lists', { a: m[2], b: m[3] }) : tr('machines.req.installing') });
    };
    const watch = setInterval(() => {
      if (Date.now() - last < STALL_MS || stalled) return;
      stalled = true; setMsg({ ok: false, text: tr('machines.req.stalled') }); setBusy(false);
    }, 2000);
    const go = () => admin.run('vm-enable', list.join(' '), said);
    try {
      try { await go(); } catch (e) { if (e.code === 'pkg.busy') throw e; last = Date.now(); await admin.run('pkg-update', '', said); await go(); }
      if (!stalled) setMsg({ ok: true, text: tr('machines.req.installed') });
    } catch (e) {
      // no package source answered (the network is down or blocks them): said as that, not as a verification problem
      if (!stalled) setMsg({ ok: false, text: e.code === 'pkg.noVerifiedSource' ? tr('machines.req.offline') : e.message });
    }
    clearInterval(watch);
    setBusy(false); onChanged();
  };
  return (
    <View style={{ flex: 1, paddingHorizontal: 30, paddingVertical: 26, gap: 12, borderRadius: 18, backgroundColor: p.raised }}>
      <Text style={{ fontSize: 22, fontWeight: 700, color: p.text, textAlign: 'start' }}>{tr('machines.req.page')}</Text>
      <Text style={{ fontSize: 13, color: p.dim, textAlign: 'start' }}>{tr('machines.req.for', { arch: list.join(', ') })}</Text>
      {/* the rows scroll when they don't fit (Windows' own rows make the list tall); the buttons below always show */}
      <ScrollView style={{ flex: 1, minHeight: 0 }}>
        <View style={{ gap: 12 }}>
          {rows.map(([k, st]) => (
            <View key={k} style={{ minHeight: 46, paddingHorizontal: 14, paddingVertical: 6, flexDirection: 'row', alignItems: 'center', gap: 12, borderRadius: 10, backgroundColor: p.card }}>
              <View style={{ flex: 1 }}>
                <Text style={{ fontSize: 13, fontWeight: 700, color: p.text, textAlign: 'start' }}>{tr(`machines.req.row.${k.split('.')[0]}`, { arch: k.split('.')[1] || '' })}</Text>
                <Text style={{ fontSize: 12, color: p.dim, textAlign: 'start' }}>{tr(`machines.req.use.${k.split('.')[0]}`)}</Text>
              </View>
              <Text style={{ fontSize: 12, fontWeight: 700, color: st === 'ready' ? '#8ce8b0' : '#ffc56e' }}>{tr(`machines.req.st.${st}`)}</Text>
            </View>
          ))}
        </View>
      </ScrollView>
      {/* how it's going, in the place of the note under the rows, so the page keeps its height */}
      <View style={{ flexDirection: 'row', alignItems: 'center', gap: 8 }}>
        {msg ? <Text style={{ flex: 1, fontSize: 13, color: msg.ok ? p.dim : '#ff453a', textAlign: 'start' }}>{msg.text}</Text>
          : <Text style={{ flex: 1, fontSize: 12, color: p.dim, textAlign: 'start' }}>{admin.available() && info.admin ? tr('machines.req.fromStore') : tr('machines.req.notAdmin')}</Text>}
        {onBack ? <Btn label={tr('machines.back')} onClick={onBack} /> : null}
        <Btn primary label={busy ? tr('machines.req.installing') : tr('machines.req.installMissing')} disabled={busy || !missing.length || !admin.available() || !info.admin} onClick={install} />
      </View>
    </View>
  );
}

// What this computer can run (5.7, model.js canRun): generated from what the host reports, for amd64 and arm64 alike;
// the sensors (temperature, fan, a Pi's power supply) while machines run; the warnings that matter here
const RUN_TONE = { native: '#8ce8b0', slow: '#ffc56e', limited: '#ffc56e', no: '#ff9a9a' };
function ThisComputer({ host, list, gvt, onBack }) {
  const p = usePal();
  const rows = canRun(host, { gvt });
  const emu = list.filter((m) => m.accel === 'tcg');
  const anyOn = list.some((m) => stateView(m.state, m.reason).active);
  const [sens, setSens] = useState(null);
  useEffect(() => {
    if (!anyOn || !has() || !lord.vm.sensors) { setSens(null); return undefined; }
    const tick = () => setSens(lord.vm.sensors());
    tick();
    const timer = setInterval(tick, 2000);
    return () => clearInterval(timer);
  }, [anyOn]);
  const sl = sensorLines(sens);
  const narrow = useNarrow();       // a small window: the notes under the table, in one scrolling column
  const page = (
    <View style={{ flex: narrow ? undefined : 1, flexDirection: narrow ? 'column' : 'row', gap: 16 }}>
      <View style={{ flex: narrow ? undefined : 1, padding: 24, gap: 12, borderRadius: 18, backgroundColor: p.raised }}>
        <Text style={{ fontSize: 20, fontWeight: 700, color: p.text, textAlign: 'start' }}>{tr('machines.pi.title', { board: host.board || tr('machines.pi.thisComputer'), ram: gb(host.ramMb) })}</Text>
        <Text style={{ fontSize: 14, color: p.dim, textAlign: 'start' }}>{tr('machines.run.native', { arch: host.arch, cpus: host.cpus, mem: gb(host.memMaxMb) })}</Text>
        <ScrollView style={{ flex: narrow ? undefined : 1 }} contentStyle={{ gap: 8 }}>
          {rows.map((r) => (
            <View key={r.k} style={{ minHeight: 48, paddingHorizontal: 14, paddingVertical: 6, flexDirection: 'row', alignItems: 'center', gap: 14, borderRadius: 10, backgroundColor: p.card }}>
              <Text style={{ width: 76, fontSize: 12, fontWeight: 700, color: RUN_TONE[r.st], textAlign: 'start' }}>{tr(`machines.pi.st.${r.st}`)}</Text>
              <View style={{ flex: 1 }}>
                <Text style={{ fontSize: 13, fontWeight: 700, color: p.text, textAlign: 'start' }}>{tr(r.k, r.vars)}</Text>
                <Text style={{ fontSize: 12, color: p.dim, textAlign: 'start' }}>{tr(r.note, r.vars)}</Text>
              </View>
            </View>
          ))}
        </ScrollView>
        <Btn label={tr('machines.back')} onClick={onBack} style={{ alignSelf: 'flex-start' }} />
      </View>
      <View style={{ width: narrow ? undefined : 340, gap: 12 }}>
        {sl.length ? (
          <View style={{ padding: 18, borderRadius: 18, backgroundColor: p.raised, gap: 6 }}>
            <Text style={{ fontSize: 16, fontWeight: 700, color: p.text, textAlign: 'start' }}>{tr('machines.run.sensors')}</Text>
            {sl.map((l) => <Text key={l.key} style={{ fontSize: 13, color: l.warn ? '#ff9f0a' : p.dim, textAlign: 'start' }}>{tr(l.key, l.vars)}</Text>)}
          </View>
        ) : null}
        {emu.length ? (
          <View style={{ padding: 18, borderRadius: 18, backgroundColor: p.warnBg }}>
            <Text style={{ fontSize: 16, fontWeight: 700, color: p.text, textAlign: 'start' }}>{tr('machines.pi.emuTitle')}</Text>
            <Text style={{ marginTop: 8, fontSize: 13, lineHeight: 1.5, color: p.text, textAlign: 'start' }}>{tr('machines.pi.emuText', { name: emu[0].name })}</Text>
          </View>
        ) : null}
        {host.libDevice === 'sd' ? (
          <View style={{ padding: 18, borderRadius: 18, backgroundColor: p.raised }}>
            <Text style={{ fontSize: 16, fontWeight: 700, color: p.text, textAlign: 'start' }}>{tr('machines.pi.sdTitle')}</Text>
            <Text style={{ marginTop: 8, fontSize: 13, lineHeight: 1.5, color: p.dim, textAlign: 'start' }}>{tr('machines.pi.sdText')}</Text>
          </View>
        ) : null}
      </View>
    </View>
  );
  return narrow ? <ScrollView style={{ flex: 1 }}>{page}</ScrollView> : page;
}

// a round toolbar square (the gear) and the + (accent filled), 40 × 40 as in the mock
function Square({ icon, title, onClick, primary }) {
  const p = usePal();
  const [hover, setHover] = useState(false);
  return (
    <View title={title} onClick={onClick} onMouseEnter={() => setHover(true)} onMouseLeave={() => setHover(false)}
      style={{ width: 40, height: 40, borderRadius: 10, alignItems: 'center', justifyContent: 'center',
        backgroundColor: primary ? p.accent : hover ? p.btn : p.raised, opacity: primary && hover ? 0.88 : 1 }}>
      <Sym name={icon} size={primary ? 22 : 20} color={primary ? '#ffffff' : p.accent} />
    </View>
  );
}
function Chip({ label, on, onClick }) {
  const p = usePal();
  return (
    <View onClick={onClick} style={{ height: 30, paddingHorizontal: 14, borderRadius: 15, justifyContent: 'center',
      backgroundColor: on ? p.accent : 'transparent', border: on ? 'none' : `1px solid ${p.line}` }}>
      <Text style={{ fontSize: 13, fontWeight: on ? 600 : 400, color: on ? '#fff' : p.text, whiteSpace: 'nowrap' }}>{label}</Text>
    </View>
  );
}
// a profile's name for the list rows ("Profile Games")
const own = { rev: -1, list: [] };
export function profileName(id) {
  if (BUILTIN.includes(id) || id === 'testing') return tr(`machines.as.profile.${id}`);
  if (own.rev !== storeRev.get()) { own.rev = storeRev.get(); own.list = loadProfiles(); }
  return own.list.find((x) => x.id === id)?.name || '';
}
// Machines settings (the approved mocks "Settings" and "Systems"): its own window
export function openSettings(section) {
  const id = 'machines#settings';
  const fresh = !windows.get().some((w) => w.id === id);
  wm.open('machines', { instance: 'settings', args: { settings: section || 'profiles', nonce: Date.now() } });
  if (fresh) { wm.resize(id, 1100, 720); wm.center(id); }
}

export function Library() {
  const p = usePal();
  const { id: winId, focused } = useAppWindow();
  const [host, setHost] = useState(() => (has() ? lord.vm.host() : null));
  const [list, setList] = useState([]);
  const [groups, setGroups] = useState([]);           // the library's machine groups (lord.vm.groups(), 19.8)
  const [fresh, setFresh] = useState(null);           // a group's "Destroy and start fresh…" dialog
  const [gsnap, setGsnap] = useState(null);           // a group's "Group snapshot…" dialog
  // false until the first read is in: the first frame draws placeholders in the view's layout (Skeleton), never the
  // empty library (FirstRun) of a library that has machines
  const [loaded, setLoaded] = useState(false);
  const [sel, setSel] = useState(null);
  const [page, setPage] = useState('main');          // main, new, requirements, pi: the last three as sheets over the library
  const [cfg, setCfg] = useState(null);              // Configure as a sheet: { dir, section }
  const [newMode, setNewMode] = useState(null);
  const [menu, setMenu] = useState(null);
  const [del, setDel] = useState(null);
  const [error, setError] = useState('');
  const [usages, setUsages] = useState({});
  const [discard, setDiscard] = useState(null);
  const [copying, setCopying] = useState(null);
  const [query, setQuery] = useState('');
  const [gpuName, setGpuName] = useState('');
  const [s0] = useSettings();
  useStore(storeRev);
  const narrow = useNarrow();
  const lv = libView(s0.machinesView);
  const winW = useWindowWidth();
  const putView = (patch) => settings.set({ machinesView: { ...libView(settings.get().machinesView), ...patch } });
  const wins = useStore(windows, (ws) => ws.map((w) => w.id).join(' '));
  const prev = useRef({});
  // lord-vm jobs started from here (start with no window, discard a saved state): followed until they end, so the
  // engine frees them, and a failure is shown
  const jobs = useRef([]);
  const follow = (id, name) => { if (!id) { setError(vmRefusal(tr('machines.cfg.refused'))); return; } jobs.current.push({ id, name }); };
  const root = useRef(null);
  overlayRoot.current = root.current;

  // the list with each machine's disk use, card picture time, app notes, and CPU/memory/disk activity while it runs
  const read = () => {
    if (!has()) return;
    jobs.current = jobs.current.filter((j) => {
      const r = lord.vm.job(j.id);
      if (!r || !r.done) return !!r;
      if (!r.ok) setError(`${j.name}: ${r.out.trim().split('\n').filter((l) => l.startsWith('lord-vm:')).pop()?.replace(/^lord-vm:\s*/, '') || r.out.trim()}`);
      return false;
    });
    const ms = lord.vm.list();
    const next = {};
    for (const m of ms) {
      const s = lord.vm.stats(m.dir) || {};
      m.diskBytes = s.diskBytes;
      m.baseBytes = s.baseBytes || 0;
      try { const st = fs.stat(`${m.dir}/thumb.png`); m.thumbTime = st?.mtime || 0; } catch (e) { m.thumbTime = 0; }
      m.meta = readMeta(m.dir);
      m.cpuWindows = readCpuUse(m.dir);
      const u = usage(prev.current[m.dir], s, m.vcpus);
      if (u) next[m.dir] = u;
      prev.current[m.dir] = s.at ? s : null;
    }
    setList(ms);
    try { setGroups(typeof lord.vm.groups === 'function' ? lord.vm.groups() || [] : []); } catch (e) { setGroups([]); }
    setUsages(next);
    setLoaded(true);
  };
  const refresh = () => { if (has()) { setHost(lord.vm.host()); read(); } };
  useEffect(() => {
    wm.setTitle(winId, tr('machines.title'));
    read();
    // the graphics card's name, for an off machine's card that has one
    try { const g = typeof lord.vm?.gpus === 'function' ? gvtGpu(lord.vm.gpus()) : null; if (g?.name) setGpuName(g.name); } catch (e) { /* none */ }
    const timer = setInterval(read, 1500);
    return () => clearInterval(timer);
  }, []);
  useEffect(() => {
    wm.setMenus(winId, [{ label: tr('machines.menu.machine'), items: [
      { label: tr('machines.newMachine'), onClick: () => setPage('new') },
      { label: tr('machines.first.import'), onClick: () => { setNewMode('import'); setPage('new'); } },
      '-',
      { label: tr('machines.set.menu'), onClick: () => openSettings('general') },
      { label: tr('machines.req.menu'), onClick: () => setPage('requirements') },
      { label: tr('machines.storage.menu'), onClick: openStorage },
      { label: tr('machines.pi.menu'), onClick: () => setPage('pi') },
    ] }, { label: tr('machines.lib.view'), items: [
      ...LIB_VIEWS.map((k) => ({ label: tr(k === 'grid' ? 'machines.lib.viewGrid' : 'machines.lib.viewList'), checked: lv.view === k, onClick: () => putView({ view: k }) })),
      '-',
      ...LIB_SORTS.map((k) => ({ label: tr(`machines.lib.sort.${k}`), checked: lv.sort === k, onClick: () => putView({ sort: k }) })),
    ] }]);
  }, [host?.arch, lv.view, lv.sort]);

  // a group's members are one card, the group's (19.8)
  const items = libraryItems(list, groups);
  const sorted = host ? sortMachines(filterMachines(items.machines, { query, show: lv.show }), lv.sort) : [];
  const groupCells = host ? items.groups.filter((g) => groupShows(g, { query, show: lv.show })) : [];
  // what it showed, for the next first frame's placeholders (only written when it changed)
  useEffect(() => {
    if (!loaded) return;
    const s = seenNext(settings.get().machinesSeen, list.length, sorted.length, query);
    if (s) settings.set({ machinesSeen: s });
  }, [loaded, list.length, sorted.length, query]);
  // the keyboard: arrows move the selection, Return opens or starts it, Escape clears the search
  const keys = useRef(null);
  keys.current = (e) => {
    if (page !== 'main' || cfg || menu || del || discard || copying || fresh || gsnap) return;
    const i = sorted.findIndex((m) => m.dir === sel);
    const cols = lv.view === 'list' ? 1 : Math.max(1, keys.cols || 1);
    const fwd = direction() === 'rtl' ? -1 : 1;
    const step = { ArrowDown: cols, ArrowUp: -cols, ArrowRight: lv.view === 'list' ? 0 : fwd, ArrowLeft: lv.view === 'list' ? 0 : -fwd }[e.key];
    if (step !== undefined && step !== 0 && sorted.length) {
      e.preventDefault?.();
      setSel(sorted[Math.max(0, Math.min(sorted.length - 1, i < 0 ? 0 : i + step))].dir);
    } else if (e.key === 'Enter' && i >= 0) {
      const m = sorted[i], a = actionsOf(m);
      if (a.open || a.start) { e.preventDefault?.(); openMachine(m); }
    } else if (e.key === 'Escape' && query) setQuery('');
  };
  useEffect(() => (focused ? addListener('keydown', (e) => keys.current?.(e)) : undefined), [focused]);
  if (!host) return <View style={{ flex: 1, alignItems: 'center', justifyContent: 'center', backgroundColor: p.bg }}><Text style={{ color: p.dim }}>{tr('machines.noBackend')}</Text></View>;

  // a group counts once (its card), under the chips its card shows with
  const counts = chipCounts(items.machines, query);
  for (const g of items.groups) for (const k of Object.keys(counts)) if (groupShows(g, { query, show: k })) counts[k]++;
  const sum = summaryOf(list, usages, host);
  // the resource budget (3.7): what the running machines hold of what machines may use here, and memory pressure
  const bl = has() && lord.vm.budget && list.length ? budgetLine(lord.vm.budget()) : null;
  // "Weaker isolation on this kernel" (3.3): what of QEMU's jail this kernel can't give
  const gaps = isolationGaps(host, null);
  // on battery with machines running: Pause All on the toolbar (6.14; the notice is ties.js's)
  const bat = battery.get();          // not a hook: below the early return; the list is read again every 1.5 s
  const runningNow = list.filter((m) => m.state === 'running');
  const pauseAll = bat?.discharging && runningNow.length ? () => { for (const m of runningNow) lord.vm.stop(m.dir, 'pause'); refresh(); } : null;
  const missing = missingFor(host, [host.arch]);
  const remove = (m) => {
    setDel(null);
    // its transfer folder stays, moved aside ("<name> (old date)"), so a new machine of that name starts with its own (6.7.1)
    if (m.transfer && has()) lord.vm.transferAside(m.dir);
    try { fs.trash(m.dir); setError(''); } catch (e) { setError(tr('machines.deleteFailed', { name: m.name })); }
    refresh();
  };
  const menuFor = (m) => {
    const a = actionsOf(m);
    const stop = (how) => lord.vm.stop(m.dir, how);
    return [
      a.start ? { label: tr('machines.start'), onClick: () => openMachine(m) } : { label: tr('machines.open'), disabled: !a.open, onClick: () => openMachine(m) },
      a.start ? { label: tr('machines.cmd.startFullscreen'), onClick: () => openMachine(m, { full: true }) } : null,
      a.start ? { label: tr('machines.cmd.startNoWindow'), onClick: () => startPrep(m).then(() => follow(lord.vm.startDetached(m.dir), m.name),
        (e) => setError(e.message)) } : null,
      { label: tr('machines.cmd.pause'), disabled: !a.pause, onClick: () => stop('pause') },
      { label: tr('machines.cmd.resume'), disabled: !a.resume, onClick: () => stop('resume') },
      { label: tr('machines.cmd.suspend'), disabled: !a.suspend || noSuspend(null, m.gpu), onClick: () => stop('suspend') },
      { label: tr('machines.cmd.shutdown'), disabled: !a.shutdown, onClick: () => stop('shutdown') },
      { label: tr('machines.cmd.reset'), disabled: !a.reset, onClick: () => stop('reset') },
      { label: tr('machines.cmd.poweroff'), destructive: true, disabled: !a.poweroff, onClick: () => stop('poweroff') },
      '-',
      { label: tr('machines.cmd.configure'), onClick: () => setCfg({ dir: m.dir }) },
      { label: tr('machines.cmd.snapshots'), onClick: () => openSnapshots(m) },
      { label: tr('machines.cmd.clone'), onClick: () => setCopying(m) },
      { label: tr('machines.cmd.logs'), onClick: () => openLogs(m) },
      { label: tr('machines.cmd.showInFiles'), onClick: () => revealPath(m.dir) },
      ...(m.transfer ? [{ label: tr('machines.tb.transferOpen'), onClick: () => { const x = lord.vm.transferFolder(m.dir); if (x) openFolder(x); } }] : []),
      m.iso && a.remove && !m.suspended ? { label: tr('machines.eject'), onClick: () => { setError(lord.vm.eject(m.dir) ? '' : tr('machines.ejectFailed', { name: m.name })); refresh(); } } : null,
      m.suspended && a.start ? { label: tr('machines.fail.discardState'), destructive: true, onClick: () => setDiscard(m) } : null,
      '-',
      { label: tr('machines.cmd.remove'), destructive: true, disabled: !a.remove, onClick: () => setDel(m) },
    ];
  };
  // a group's card: Play starts the group (the gateway, then the workstation in its window), Open shows the workstation
  const goGroup = (item) => {
    if (item.ws && stateView(item.ws.state, item.ws.reason).active) { openMachine(item.ws); return; }
    setError('');
    startGroup(item, (ws) => openMachine(ws), host).then((why) => { if (why) setError(why); refresh(); });
  };
  const groupMenuFor = (item) => groupMenu(item).map((x) => (x === '-' ? x : {
    label: tr(`machines.group.menu.${x.k}`), disabled: x.disabled, destructive: x.destructive, checked: x.k === 'disposable' ? x.checked : undefined,
    onClick: () => {
      if (x.k === 'start') goGroup(item);
      else if (x.k === 'showWorkstation') openMachine(item.ws);
      else if (x.k === 'showGateway') openMachine(item.gw);
      else if (x.k === 'shutdown' || x.k === 'poweroff') stopGroup(item, x.k).then(refresh);
      else if (x.k === 'snapshot') setGsnap(item);
      else if (x.k === 'configure') setCfg({ dir: item.ws.dir, group: item.group.dir });
      else if (x.k === 'disposable') follow(lord.vm.groupSet(item.group.dir, { disposable: !item.group.disposable }), item.group.name);
      else if (x.k === 'logs') openLogs(item.ws);
      else if (x.k === 'showInFiles') revealPath(item.group.dir);
      else if (x.k === 'fresh') setFresh(item);
    },
  }));
  const rl = root.current ? lord.getLayout(root.current.id) : [0, 0, 1000, 700];
  // as many cards as fit at 300 px or more, the New machine card first; each picture about half as tall as wide. The
  // window's width from the window store: the root isn't laid out yet in the first frames
  const grid = libGrid(winW > 0 ? winW : rl[2], narrow);
  const { pad, gap, cols, picH } = grid;
  keys.cols = cols;
  const skel = loaded ? null : skeletonPlan(s0.machinesSeen);
  const shell = !!list.length || (skel && !skel.empty);     // the chips and the status line: with machines, or their placeholders
  const cells = [{ isNew: true }, ...groupCells.map((g) => ({ isGroup: true, dir: g.group.dir, item: g })), ...sorted];
  const groupProps = (g) => ({ item: g, sel: sel === g.group.dir, onSelect: () => setSel(g.group.dir), onGo: () => goGroup(g),
    onMenu: (e) => setMenu({ x: e.x, y: e.y, above: e.above, group: g }) });
  const rows = [];
  for (let i = 0; i < cells.length; i += cols) rows.push(cells.slice(i, i + cols));
  const cardProps = (m) => ({ m, u: usages[m.dir], sel: sel === m.dir, onSelect: () => setSel(m.dir), windowed: wins.split(' ').includes(winIdOf(m)), host,
    onMenu: (e) => setMenu({ x: e.x, y: e.y, m }) });
  const newMachine = () => setPage('new');

  let body;
  // the assistant, the requirements and the Pi page are sheets over the library, which stays behind them
  const sheet = page === 'new' ? (
    <Sheet width={940} height={700} modal onClose={() => { setPage('main'); setNewMode(null); }}>
      <Assistant host={host} mode={newMode} names={list.map((m) => m.name)} groupNames={groups.map((g) => g.name)}
        onGroup={(dir) => { setPage('main'); setNewMode(null); refresh(); const it = libraryItems(lord.vm.list(), lord.vm.groups()).groups.find((x) => x.group.dir === dir); if (it) { setSel(dir); goGroup(it); } }}
        onClose={() => { setPage('main'); setNewMode(null); refresh(); }}
        onCreated={(m) => { setPage('main'); setNewMode(null); refresh(); if (m) openMachine(m); }} onConfigure={(m) => { setPage('main'); refresh(); if (m) setCfg({ dir: m.dir, section: 'cpu' }); }}
        onNeeds={() => setPage('requirements')} />
    </Sheet>
  ) : page === 'requirements' ? (
    <Sheet width={760} height={600} onClose={() => setPage('main')}><View style={{ flex: 1, padding: 18 }}><Requirements host={host} onBack={() => setPage('main')} onChanged={refresh} /></View></Sheet>
  ) : page === 'pi' ? (
    <Sheet width={820} height={640} onClose={() => setPage('main')}><View style={{ flex: 1, padding: 18 }}><ThisComputer host={host} list={list} gvt={!!gpuName} onBack={() => setPage('main')} /></View></Sheet>
  ) : null;
  if (skel && !skel.empty) body = <Skeleton view={lv.view} n={skel.cards} grid={grid} onNew={newMachine} />;
  else if (!list.length) body = <FirstRun host={host} onNew={newMachine} onImport={() => { setNewMode('import'); setPage('new'); }} onCheck={refresh} onCanRun={() => setPage('pi')} />;
  else if (!sorted.length && !groupCells.length) body = <EmptyState icon="⌕" hint={query.trim() ? tr('machines.lib.noMatch', { q: query.trim() }) : (lv.show === 'windows' ? tr('machines.win.lib.none') : tr(`machines.lib.none.${lv.show}`))} />;
  else if (lv.view === 'list') body = (
    <ScrollView style={{ flex: 1 }} contentStyle={{ paddingHorizontal: pad, paddingVertical: 16, gap: 12 }}>
      <NewCard row onClick={newMachine} />
      {groupCells.map((g) => <GroupRow key={g.group.dir} {...groupProps(g)} />)}
      {sorted.map((m) => <ListRow key={m.dir} {...cardProps(m)} />)}
    </ScrollView>
  );
  else body = (
    <ScrollView style={{ flex: 1 }} contentStyle={{ paddingHorizontal: pad, paddingVertical: 16, gap }}>
      {rows.map((r, i) => (
        <View key={i} style={{ flexDirection: 'row', gap }}>
          {r.map((c) => (c.isNew ? <NewCard key="new" picH={picH} onClick={newMachine} /> : c.isGroup ? <GroupCard key={c.dir} {...groupProps(c.item)} picH={picH} />
            : <Card key={c.dir} {...cardProps(c)} picH={picH} gpuName={gpuName} />))}
          {Array.from({ length: cols - r.length }, (_, k) => <View key={`f${k}`} style={{ flex: 1 }} />)}
        </View>
      ))}
    </ScrollView>
  );

  const chip = (k, label) => <Chip key={k} label={label} on={lv.show === k} onClick={() => putView({ show: k })} />;
  return (
    <View ref={root} style={{ flex: 1, backgroundColor: p.bg }}>
      <View style={{ flexDirection: 'row', alignItems: 'center', gap: 12, paddingHorizontal: pad, paddingTop: 16, paddingBottom: 8 }}>
        <Square icon="gear" title={tr('machines.set.title')} onClick={() => openSettings()} />
        <View style={{ flex: 1, minWidth: 0, height: 40, flexDirection: 'row', alignItems: 'center', gap: 10, paddingHorizontal: 14, borderRadius: 10, backgroundColor: p.raised }}>
          <Sym name="search" size={18} color={p.dim} />
          <TextInput value={query} onChange={setQuery} placeholder={tr('machines.lib.searchMachines')} onCancel={() => setQuery('')} size={15}
            style={{ flex: 1, minWidth: 0, backgroundColor: 'transparent', borderWidth: 0, border: 'none' }} />
        </View>
        {missing.length || (host.kvmDevice && !host.kvm) ? <Square icon="security" title={tr('machines.req.menu')} onClick={() => setPage('requirements')} /> : null}
        {host.arch === 'arm64' && host.board && !narrow ? <Square icon="general" title={tr('machines.pi.menu')} onClick={() => setPage('pi')} /> : null}
        <View style={{ flexDirection: 'row', padding: 3, borderRadius: 10, backgroundColor: p.raised }}>
          {[['grid', 'grid', 'machines.lib.viewGrid'], ['list', 'list', 'machines.lib.viewList']].map(([k, icon, l]) => (
            <View key={k} title={tr(l)} onClick={() => putView({ view: k })}
              style={{ width: 40, height: 34, borderRadius: 8, alignItems: 'center', justifyContent: 'center', backgroundColor: lv.view === k ? p.btn : 'transparent' }}>
              <Sym name={icon} size={18} color={lv.view === k ? p.text : p.dim} />
            </View>
          ))}
        </View>
        {pauseAll ? <Square icon="pause" title={tr('machines.battery.pauseAll')} onClick={pauseAll} /> : null}
        <Square primary icon="plus" title={tr('machines.newMachine')} onClick={newMachine} />
      </View>
      {list.length ? (
        <View style={{ flexDirection: 'row', flexWrap: 'wrap', gap: 8, paddingHorizontal: pad, paddingTop: 8, paddingBottom: 4 }}>
          {chip('all', tr('machines.lib.chip.all', { n: counts.all }))}
          {chip('running', tr('machines.lib.chip.running', { n: counts.running }))}
          {chip('linux', tr('machines.lib.chip.linux', { n: counts.linux }))}
          {chip('windows', tr('machines.win.lib.chip', { n: counts.windows }))}
        </View>
      ) : shell ? <SkelChips pad={pad} /> : null}
      {error ? <Text style={{ marginHorizontal: pad, marginTop: 10, fontSize: 13, color: '#ff453a', textAlign: 'start' }}>{error}</Text> : null}
      {gaps.length ? (
        <View title={gaps.map((g) => tr(`machines.weak.${g}`)).join(' ')} style={{ marginHorizontal: pad, marginTop: 10, paddingHorizontal: 12, paddingVertical: 8, borderRadius: 10, flexDirection: 'row', alignItems: 'center', gap: 8,
          backgroundColor: p.dark ? 'rgba(255,159,10,0.14)' : 'rgba(255,159,10,0.12)' }}>
          <Sym name="security" size={16} color="#ff9f0a" />
          <Text style={{ flex: 1, minWidth: 0, fontSize: 13, color: p.text, textAlign: 'start' }}>{tr('machines.weak.title')} {gaps.map((g) => tr(`machines.weak.${g}`)).join(' ')}</Text>
        </View>
      ) : null}
      {body}
      {shell ? (
        <View style={{ height: 40, flexDirection: 'row', alignItems: 'center', gap: narrow ? 14 : 24, paddingHorizontal: pad, borderTopWidth: 1, borderColor: p.line, overflow: 'hidden' }}>
          {list.length ? <Summary label={tr('machines.sum.running')} value={tr('machines.sum.of', { a: sum.running.a, b: sum.running.b })} /> : null}
          {list.length && sum.memory ? <Summary label={tr('machines.sum.memory')} value={tr('machines.sum.ofGb', { a: gib(sum.memory.a), b: gib(sum.memory.b) })} /> : null}
          {list.length && sum.free ? <Summary label={tr('machines.lib.freeDisk')} value={bytes(sum.free.a)} onClick={openStorage} /> : null}
          {bl && !narrow ? <Summary label={tr('machines.sum.budget')} value={tr('machines.sum.ofGb', { a: bl.usedGb, b: bl.maxGb })} /> : null}
          {bl?.pressure ? <Text title={tr('machines.sum.pressureHint')} style={{ fontSize: 12, fontWeight: 600, color: '#ff9f0a', whiteSpace: 'nowrap' }}>{tr('machines.sum.pressure')}</Text> : null}
        </View>
      ) : null}
      {menu ? <CardMenu at={menu} root={root} rl={rl} items={menu.group ? groupMenuFor(menu.group) : menuFor(menu.m)} onClose={() => setMenu(null)} /> : null}
      {sheet}
      {cfg ? <Sheet width={1000} height={700} modal onClose={() => setCfg(null)}>
        <Configure key={cfg.dir} dir={cfg.dir} group={cfg.group} section={cfg.group ? cfg.section || 'group' : cfg.section} onMember={(d) => setCfg((c) => ({ ...c, dir: d, section: 'group' }))} onClose={() => { setCfg(null); refresh(); }} />
      </Sheet> : null}
      {copying ? <CloneSheet m={copying} running={stateView(copying.state, copying.reason).active} onClose={() => setCopying(null)}
        onDone={(out) => { setCopying(null); refresh(); if (out?.dir) setSel(out.dir); }} /> : null}
      {discard ? <ConfirmDialog title={tr('machines.discard.title', { name: discard.name })} detail={tr('machines.discard.detail')} okLabel={tr('machines.fail.discardState')} cancelLabel={tr('machines.cancel')} destructive
        onOk={() => { const d = discard; setDiscard(null); follow(lord.vm.discardState(d.dir), d.name); }} onCancel={() => setDiscard(null)} /> : null}
      {fresh ? <FreshDialog item={fresh} onCancel={() => setFresh(null)} onDone={(id) => { const g = fresh; setFresh(null); follow(id, g.group.name); refresh(); }} /> : null}
      {gsnap ? <GroupSnapDialog item={gsnap} onCancel={() => setGsnap(null)} onDone={(ids) => { const g = gsnap; setGsnap(null); for (const id of ids) follow(id, g.group.name); }} /> : null}
      {del ? <ConfirmDialog title={tr('machines.deleteTitle', { name: del.name })} detail={tr('machines.deleteDetail')} okLabel={tr('machines.deleteOk')} cancelLabel={tr('machines.cancel')} destructive
        onOk={() => remove(del)} onCancel={() => setDel(null)} /> : null}
    </View>
  );
}
