// Machines (alpha) — other systems in windows, each machine supervised by lord-vm in its own sandbox and its picture
// drawn by lord-vmdisplay into a media node (docs/VIRTUALIZATION.md, "Phase 1 implementation", The app). One window
// is the Control Center (library.jsx, with the New Machine assistant, Requirements and the Pi page); the same app
// opened with args.machine is a machine's window, args.configure its Configure window, args.logs its logs.
import { useAppWindow } from '@lordos/sdk';
import { Library, AppMark } from './library.jsx';
import { MachineWindow } from './vmwindow.jsx';
import { Configure } from './configure.jsx';
import { Logs } from './logs.jsx';
import { Snapshots } from './snapshots.jsx';

export function Icon({ size }) { return <AppMark size={size} />; }

export default function App() {
  const { args } = useAppWindow();
  if (args?.machine) return <MachineWindow dir={args.machine} name={args.name || ''} loadvm={args.loadvm || ''} />;
  if (args?.snapshots) return <Snapshots dir={args.snapshots} name={args.name || ''} />;
  if (args?.configure) return <Configure dir={args.configure} section={args.section} />;
  if (args?.logs) return <Logs dir={args.logs} name={args.name || ''} />;
  return <Library />;
}
