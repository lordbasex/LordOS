// Machines (alpha) — other systems in windows, each machine supervised by lord-vm in its own sandbox and its picture
// drawn by lord-vmdisplay into a media node (docs/VIRTUALIZATION.md, "Phase 1 implementation", The app). One window
// is the library (the list, New, Start, Stop, Delete); each running machine has a window of its own, the same app
// opened with args.machine. Phase 2 replaces the library with the full Control Center (5.1).
import { View, useAppWindow } from '@lordos/sdk';
import { Library } from './library.jsx';
import { MachineWindow } from './vmwindow.jsx';

// A screen holding a smaller rounded tile, on the LordOS shape (5, the icon)
export function Icon({ size: s }) {
  return (
    <View style={{ width: s, height: s, borderRadius: s * 0.225, overflow: 'hidden', direction: 'ltr', background: 'linear-gradient(160deg, #3a4a63 0%, #141b29 100%)' }}>
      <View style={{ position: 'absolute', left: s * 0.13, top: s * 0.18, width: s * 0.74, height: s * 0.52, borderRadius: s * 0.07, backgroundColor: '#e9eef7' }} />
      <View style={{ position: 'absolute', left: s * 0.17, top: s * 0.22, width: s * 0.66, height: s * 0.44, borderRadius: s * 0.045, background: 'linear-gradient(160deg, #1d2a40 0%, #0c1220 100%)' }} />
      <View style={{ position: 'absolute', left: s * 0.33, top: s * 0.31, width: s * 0.34, height: s * 0.26, borderRadius: s * 0.06, background: 'linear-gradient(160deg, #5ad1a8 0%, #2a8f7a 100%)' }} />
      <View style={{ position: 'absolute', left: s * 0.39, top: s * 0.36, width: s * 0.09, height: s * 0.035, borderRadius: s * 0.02, backgroundColor: 'rgba(255,255,255,0.85)' }} />
      <View style={{ position: 'absolute', left: s * 0.42, top: s * 0.74, width: s * 0.16, height: s * 0.05, backgroundColor: '#c9d2e3' }} />
      <View style={{ position: 'absolute', left: s * 0.3, top: s * 0.79, width: s * 0.4, height: s * 0.04, borderRadius: s * 0.02, backgroundColor: '#e9eef7' }} />
    </View>
  );
}

export default function App() {
  const { args } = useAppWindow();
  if (args?.machine) return <MachineWindow dir={args.machine} name={args.name || ''} />;
  return <Library />;
}
