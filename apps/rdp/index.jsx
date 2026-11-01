// RDP — saved computers and a connection; the picture is the backend's own window (sdk/remotedesk.jsx).
import { View, Text, RemoteDeskApp } from '@lordos/sdk';

export function Icon({ size: s }) {
  return (
    <View style={{ width: s, height: s, borderRadius: s * 0.225, background: 'linear-gradient(180deg, #5fd0c5, #127a8e)', alignItems: 'center', justifyContent: 'center' }}>
      <Text style={{ fontSize: s * 0.28, fontWeight: 700, color: '#fff' }}>RDP</Text>
    </View>
  );
}

export default function App() { return <RemoteDeskApp proto="rdp" />; }
