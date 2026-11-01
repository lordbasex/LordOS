// RDP — saved computers and a connection; the picture is the backend's own window (sdk/remotedesk.jsx).
import { View, RemoteDeskApp } from '@lordos/sdk';

export function Icon({ size: s }) {
  const box = (x, y, w, h, color, r = 0) => <View style={{ position: 'absolute', left: s * x, top: s * y, width: s * w, height: s * h, backgroundColor: color, borderRadius: s * r }} />;
  return (
    <View style={{ width: s, height: s, borderRadius: s * 0.225, background: 'linear-gradient(180deg, #8aa4ff, #2f3fb8)' }}>
      {box(0.44, 0.66, 0.12, 0.09, '#fff')}
      {box(0.33, 0.74, 0.34, 0.05, '#fff', 0.025)}
      {box(0.17, 0.21, 0.66, 0.46, '#fff', 0.06)}
      {box(0.22, 0.26, 0.56, 0.36, '#242f8f', 0.03)}
      {/* a window: a session of its own on the other computer */}
      {box(0.30, 0.32, 0.40, 0.25, '#fff', 0.03)}
      {box(0.30, 0.32, 0.40, 0.07, '#b8c6ff', 0.03)}
    </View>
  );
}

export default function App() { return <RemoteDeskApp proto="rdp" />; }
