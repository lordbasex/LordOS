// A test package for tests/companion_e2e.sh: it passes what the companion says to a file through lord.fs, and sets a status item.
import { useEffect, useState } from 'react';
import { View, Text, companion, statusItem } from '@lordos/sdk';

const say = (name, v) => { try { lord.fs.write(name, JSON.stringify(v)); } catch (e) { /* */ } };

export default function App() {
  const [line, setLine] = useState('waiting');
  useEffect(() => {
    let n = 0;
    const off = companion.on((m) => {
      setLine(JSON.stringify(m));
      say(`msg${n++}.json`, m);
      if (m && m.companion === 'started') companion.send({ ping: 1 });
    });
    const ok = statusItem.set({ icon: 'icon.png', title: 'Test', alert: true, menu: [{ id: 'open', label: 'Open' }, { line: true }, { id: 'quit', label: 'Quit' }] });
    say('status.json', { ok });
    return off;
  }, []);
  return <View style={{ flex: 1, padding: 12 }}><Text>{line}</Text></View>;
}
