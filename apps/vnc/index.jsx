// VNC — saved computers and a connection; the picture is the backend's own window (sdk/remotedesk.jsx).
import { View, RemoteDeskApp } from '@lordos/sdk';

export function Icon({ size: s }) {
  const box = (x, y, w, h, color, r = 0) => <View style={{ position: 'absolute', left: s * x, top: s * y, width: s * w, height: s * h, backgroundColor: color, borderRadius: s * r }} />;
  return (
    <View style={{ width: s, height: s, borderRadius: s * 0.225, background: 'linear-gradient(180deg, #5fd0c5, #127a8e)' }}>
      {box(0.44, 0.66, 0.12, 0.09, '#fff')}
      {box(0.33, 0.74, 0.34, 0.05, '#fff', 0.025)}
      {box(0.17, 0.21, 0.66, 0.46, '#fff', 0.06)}
      {box(0.22, 0.26, 0.56, 0.36, '#0e5f70', 0.03)}
      {/* a pointer: what you do is move the other computer's own */}
      <View style={{ position: 'absolute', left: s * 0.41, top: s * 0.30, width: s * 0.2, height: s * 0.2, overflow: 'hidden' }}>
        <View style={{ position: 'absolute', left: -s * 0.2, top: -s * 0.2, width: s * 0.4, height: s * 0.4, backgroundColor: '#fff', transform: 'rotate(45deg)' }} />
      </View>
      <View style={{ position: 'absolute', left: s * 0.475, top: s * 0.43, width: s * 0.2, height: s * 0.055, backgroundColor: '#fff', transform: 'rotate(45deg)' }} />
    </View>
  );
}

export default function App() { return <RemoteDeskApp proto="vnc" />; }
