// Malicious test app (tests/lpk-evil). Declares ZERO permissions and, when opened, tries everything
// an isolated app should not be able to do. Shows the result of each attempt: with the LordOS
// sandbox (server/src/sec/sandbox.c) all of them must fail. See docs/PRIVACY.md.
//
// When apps run as their own process (axia-server --app <id>) these attempts hit
// Landlock/seccomp. Meanwhile, the ones going through the SDK (lord.fs, clipboard) already
// run into the server's restrictions; the native ones (socket, exec, ptrace) are covered by
// sandbox-test. The app serves as a visible witness of the isolation.
import { View, Text, ScrollView, useTheme, clipboard, fs } from '@lordos/sdk';
import { useEffect, useState } from 'react';

const attempts = [
  ['Leer ~/.ssh/id_rsa', () => fs.read('.ssh/id_rsa')],
  ['Leer settings.json', () => fs.read('.config/axia/settings.json')],
  ['Escribir fuera de mi carpeta', () => fs.write('.config/axia/robado.txt', 'x')],
  ['Leer el portapapeles en silencio', () => clipboard.get()],
  ['Abrir /etc/shadow', () => fs.read('/etc/shadow')],
];

export function Icon({ size: s }) {
  return <View style={{ width: s, height: s, borderRadius: s * 0.22, backgroundColor: '#ff453a', alignItems: 'center', justifyContent: 'center' }}>
    <Text style={{ fontSize: s * 0.6, lineHeight: s }}>☠</Text></View>;
}

export default function Evil() {
  const t = useTheme();
  const [rows, setRows] = useState([]);
  useEffect(() => {
    const out = [];
    for (const [label, fn] of attempts) {
      let r;
      try { const v = fn(); r = { ok: false, note: `DEVOLVIÓ ${String(v).slice(0, 40)}` }; }
      catch (e) { r = { ok: true, note: `bloqueado: ${String(e.message || e).slice(0, 60)}` }; }
      out.push({ label, ...r });
    }
    setRows(out);
  }, []);
  return (
    <View style={{ flex: 1, padding: 16, gap: 8 }}>
      <Text style={{ fontSize: 16, fontWeight: 700, color: t.text }}>Intentos de la app maliciosa</Text>
      <ScrollView style={{ flex: 1 }}>
        {rows.map((r, i) => (
          <View key={i} style={{ flexDirection: 'row', gap: 8, paddingVertical: 6 }}>
            <Text style={{ fontSize: 13, color: r.ok ? '#30d158' : '#ff453a' }}>{r.ok ? '✓' : '✗'}</Text>
            <View style={{ flex: 1 }}>
              <Text style={{ fontSize: 13, color: t.text }}>{r.label}</Text>
              <Text style={{ fontSize: 11, color: t.textDim }}>{r.note}</Text>
            </View>
          </View>
        ))}
      </ScrollView>
    </View>
  );
}
