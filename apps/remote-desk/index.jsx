// Remote Desk — saved computers and a connection over RDP or VNC; the picture is the backend's own window (sdk/remotedesk.jsx).
import { View, RemoteDeskApp } from '@lordos/sdk';

export function Icon({ size: s }) {
  const box = (x, y, w, h, color, r = 0, opacity = 1) => <View style={{ position: 'absolute', left: s * x, top: s * y, width: s * w, height: s * h, backgroundColor: color, borderRadius: s * r, opacity }} />;
  return (
    <View style={{ width: s, height: s, borderRadius: s * 0.225, background: 'linear-gradient(180deg, #8aa4ff, #2f3fb8)' }}>
      {/* a computer far away, and the screen that shows it here */}
      {box(0.14, 0.17, 0.27, 0.66, '#fff', 0.05, 0.5)}
      {box(0.20, 0.25, 0.15, 0.04, '#242f8f', 0.02)}
      {box(0.20, 0.33, 0.15, 0.04, '#242f8f', 0.02)}
      {box(0.34, 0.38, 0.53, 0.37, '#fff', 0.05)}
      {box(0.38, 0.42, 0.45, 0.29, '#242f8f', 0.025)}
      {box(0.44, 0.47, 0.33, 0.19, '#fff', 0.025)}
      {box(0.44, 0.47, 0.33, 0.06, '#b8c6ff', 0.025)}
    </View>
  );
}

export default function App() { return <RemoteDeskApp />; }
