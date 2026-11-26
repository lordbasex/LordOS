// Machines (alpha) — other systems in windows, each machine supervised by lord-vm in its own sandbox and its picture
// drawn by lord-vmdisplay into a media node (docs/VIRTUALIZATION.md, "Phase 1 implementation", The app). One window
// is the Control Center (library.jsx, with the New Machine assistant, Requirements and the Pi page); the same app
// opened with args.machine is a machine's window, args.configure its Configure window, args.logs its logs, args.guestWin
// one window of a guest in apps mode (appsmode.jsx), args.settings Machines settings (settingswin.jsx).
import { useEffect } from 'react';
import { useAppWindow, addListener, wm, t as tr, registerDockActions, registerDockActivate, registerWindowMenuItems, registerOpenProviders, registerBundle, SkinHeader, appName, getApp } from '@lordos/sdk';
import { Library, AppMark, openInMachine, openMachine } from './library.jsx';
import { startGuestLauncher } from './guestlaunch.js';
import { openMachines, windowsNeedsTools } from './openwith.js';
import { readMeta } from './meta.js';
import { isForeignProgram } from '../../sdk/filekinds.js';
import { MachineWindow } from './vmwindow.jsx';
import { Configure } from './configure.jsx';
import { Logs } from './logs.jsx';
import { Snapshots } from './snapshots.jsx';
import { Storage } from './storage.jsx';
import { GuestWindow } from './appsmode.jsx';
import { startAppsStatus } from './appsstatus.jsx';
import { startMachinesCli } from './cli.js';
import { startMachinesTies } from './ties.js';
import { MachinesSettings } from './settingswin.jsx';
import { useMachineDir } from './ui.jsx';
import { HeadWindow } from './alldisplays.jsx';
import { headArgs } from './alldisplays.js';
import { RecoveryKeyWindow } from './vmkey.jsx';

export function Icon({ size }) { return <AppMark size={size} />; }

// The library (the window without arguments, id "machines") is always one step away: the first item of the Dock's
// menu, the first of every Machines window's Window menu (❖0 in a machine's window), and a click on the Dock item when
// only machines are open
const LIBRARY_ID = 'machines';
export const openLibrary = () => wm.open('machines');
registerDockActions('machines', () => [{ id: 'library', label: 'machines.libraryMenu', args: {} }]);
registerDockActivate('machines', (e) => {
  if (e.windows.some((w) => w.id === LIBRARY_ID)) return false;
  openLibrary();
  return true;
});
registerWindowMenuItems('machines', () => [{ label: tr('machines.libraryMenu'), hint: '❖0', onClick: openLibrary }]);

// Files' "Open with" (docs/VIRTUALIZATION.md 7.3; sdk/openwith.js): one entry per machine whose guest tools open files,
// "‹name› (Windows)"; the natural handler of a program for another system (.exe, .msi…) when no app here opens it. An off
// or suspended machine is started (a double click asks first: needsStart); the file is copied in and opened there.
registerOpenProviders('machines', () => {
  if (typeof lord === 'undefined' || !lord.vm || typeof lord.vm.list !== 'function') return [];
  let list = [];
  try { list = lord.vm.list() || []; } catch (e) { list = []; }
  const out = openMachines(list, readMeta).map((m) => ({
    id: `machine:${m.id}`,
    label: m.opens === 'windows' ? tr('machines.win.openWith', { name: m.name }) : tr('machines.openWith.machine', { name: m.name }),
    accepts: (f) => !!f && !f.dir && typeof f.path === 'string' && f.path.startsWith('/'),
    suggests: (f) => m.opens === 'windows' && isForeignProgram(f),
    needsStart: () => !m.active,
    open: (f) => openInMachine(m, f.path),
  }));
  // a Windows machine whose tools are too old to open files (or not there yet): Files says so under Open with, for a
  // Windows program, instead of showing nothing
  if (windowsNeedsTools(list, readMeta)) out.push({ id: 'machines:tools', hint: (f) => (isForeignProgram(f) ? tr('machines.win.openWithUpdate') : '') });
  return out;
});

// Files shows a machine's folder (and a group's) as one item, a bundle (docs/VIRTUALIZATION.md 6.14): opening it opens the
// machine's window (a group's: the library); Open in New Tab still goes in
const machineAt = (path) => { try { return (lord.vm.list() || []).find((m) => m.dir === path && !m.error) || null; } catch (e) { return null; } };
registerBundle({ id: 'machines', suffix: '.lordvm', kind: () => tr('machines.bundle.machine'), open: (path) => { const m = machineAt(path); if (m) openMachine(m); else openLibrary(); } });
registerBundle({ id: 'machines', suffix: '.lordgroup', kind: () => tr('machines.bundle.group'), open: () => openLibrary() });

// the `machines` command's start and open go through this session (cli.js); a moment after the desktop is up
setTimeout(startMachinesCli, 1000);
// Search, start with the session, the battery notice and "Pause while I'm away" (ties.js)
setTimeout(startMachinesTies, 1500);
// the guests' apps of machines that are off, in the launcher and search (guestlaunch.js, 6.12)
setTimeout(() => startGuestLauncher(openMachine), 1500);
// apps mode's item by the clock, one per machine in it (appsstatus.jsx)
startAppsStatus();

export const isLibraryKey = (e) => !!e && !!e.meta && !e.ctrl && !e.alt && (e.base || e.key) === '0';

// (0.8.9.6.2) app skins (docs/THEMES-GUIDE.md, "App skins → the shared shapes"): a header a theme may add above the app
// only the main window gets the header (a machine's screen, a console or a sheet window is drawn as before)
export default function App() {
  const { args } = useAppWindow();
  if (args && Object.keys(args).some((k) => k !== 'nonce')) return <MachinesMain />;
  return <SkinHeader app="machines" title={appName(getApp('machines'))}><MachinesMain /></SkinHeader>;
}

function MachinesMain() {
  const { args, focused } = useAppWindow();
  useEffect(() => (focused && args && Object.keys(args).length ? addListener('keydown', (e) => { if (isLibraryKey(e)) { e.preventDefault?.(); openLibrary(); } }) : undefined), [focused]);
  // a machine renamed with its folder (6.26): its windows follow it there, by its id
  const dir0 = args?.machine || args?.snapshots || args?.configure || args?.logs || '';
  const dir = useMachineDir(dir0);
  // Monitor's "Show in Machines" (docs/VIRTUALIZATION.md 6.14): the library, and the machine's window opened here, so it
  // is protected from its first frame when it must be (openMachine)
  const showId = typeof args?.show === 'string' ? args.show : '';
  useEffect(() => {
    if (!showId) return;
    let m = null;
    try { m = (lord.vm.list() || []).find((x) => x.id === showId && !x.error) || null; } catch (e) { m = null; }
    if (m) openMachine(m);
  }, [showId, args?.at]);
  // All Displays (5.4): a further output of a machine's display, full screen on another display (alldisplays.jsx)
  const head = headArgs(args?.head);
  if (head) return <HeadWindow key={`${head.dir}:${head.k}`} dir={head.dir} k={head.k} name={args.name || ''} />;
  // an encrypted machine's recovery key (keyflow.js): a protected window of its own
  if (args?.recoveryKey) return <RecoveryKeyWindow id={args.recoveryKey} name={args.name || ''} />;
  if (args?.guestWin) return <GuestWindow dir={args.guestWin.dir} gid={args.guestWin.gid} />;
  if (args?.machine) return <MachineWindow key={dir} dir={dir} name={args.name || ''} loadvm={args.loadvm || ''} followed={dir !== dir0} />;
  if (args?.storage) return <Storage />;
  if (args?.settings) return <MachinesSettings section={args.settings} />;
  if (args?.snapshots) return <Snapshots key={dir} dir={dir} name={args.name || ''} />;
  if (args?.configure) return <Configure key={dir} dir={dir} section={args.section} />;
  if (args?.logs) return <Logs key={dir} dir={dir} name={args.name || ''} />;
  return <Library />;
}
