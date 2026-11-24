// Machines (alpha) — other systems in windows, each machine supervised by lord-vm in its own sandbox and its picture
// drawn by lord-vmdisplay into a media node (docs/VIRTUALIZATION.md, "Phase 1 implementation", The app). One window
// is the Control Center (library.jsx, with the New Machine assistant, Requirements and the Pi page); the same app
// opened with args.machine is a machine's window, args.configure its Configure window, args.logs its logs, args.guestWin
// one window of a guest in apps mode (appsmode.jsx), args.settings Machines settings (settingswin.jsx).
import { useEffect } from 'react';
import { useAppWindow, addListener, wm, t as tr, registerDockActions, registerDockActivate, registerWindowMenuItems, registerOpenProviders } from '@lordos/sdk';
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
import { MachinesSettings } from './settingswin.jsx';
import { useMachineDir } from './ui.jsx';

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

// the `machines` command's start and open go through this session (cli.js); a moment after the desktop is up
setTimeout(startMachinesCli, 1000);
// the guests' apps of machines that are off, in the launcher and search (guestlaunch.js, 6.12)
setTimeout(() => startGuestLauncher(openMachine), 1500);
// apps mode's item by the clock, one per machine in it (appsstatus.jsx)
startAppsStatus();

export const isLibraryKey = (e) => !!e && !!e.meta && !e.ctrl && !e.alt && (e.base || e.key) === '0';

export default function App() {
  const { args, focused } = useAppWindow();
  useEffect(() => (focused && args && Object.keys(args).length ? addListener('keydown', (e) => { if (isLibraryKey(e)) { e.preventDefault?.(); openLibrary(); } }) : undefined), [focused]);
  // a machine renamed with its folder (6.26): its windows follow it there, by its id
  const dir0 = args?.machine || args?.snapshots || args?.configure || args?.logs || '';
  const dir = useMachineDir(dir0);
  if (args?.guestWin) return <GuestWindow dir={args.guestWin.dir} gid={args.guestWin.gid} />;
  if (args?.machine) return <MachineWindow key={dir} dir={dir} name={args.name || ''} loadvm={args.loadvm || ''} followed={dir !== dir0} />;
  if (args?.storage) return <Storage />;
  if (args?.settings) return <MachinesSettings section={args.settings} />;
  if (args?.snapshots) return <Snapshots key={dir} dir={dir} name={args.name || ''} />;
  if (args?.configure) return <Configure key={dir} dir={dir} section={args.section} />;
  if (args?.logs) return <Logs key={dir} dir={dir} name={args.name || ''} />;
  return <Library />;
}
