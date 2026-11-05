// Logs (the approved mock "Logs"): the machine's log (lord-vm, QEMU and passt) or the guest's serial console, with a
// filter, Pause, Copy and Save. Read again every second while it isn't paused. Guest journal levels and units need a
// guest agent: later.
import { View, Text, TextInput, ScrollView, Segmented, useAppWindow, wm, fs, clipboard, revealPath, t as tr } from '@lordos/sdk';
import { useEffect, useState } from 'react';
import { usePal, Btn } from './ui.jsx';

const levelOf = (l) => (/(fail|error|refus|crash|killed|panic|cannot|can't)/i.test(l) ? 'error' : /(warn|note:|lost|retry)/i.test(l) ? 'warn' : 'info');

export function Logs({ dir, name }) {
  const p = usePal();
  const { id: winId } = useAppWindow();
  const [which, setWhich] = useState('vm');
  const [text, setText] = useState('');
  const [filter, setFilter] = useState('');
  const [paused, setPaused] = useState(false);
  const [saved, setSaved] = useState('');
  useEffect(() => { wm.setTitle(winId, tr('machines.logs.title', { name })); }, [name]);
  useEffect(() => {
    const read = () => { if (!paused) setText(lord.vm.log(dir, which, 262144)); };
    read();
    const timer = setInterval(read, 1000);
    return () => clearInterval(timer);
  }, [which, paused]);
  const all = text.split('\n').filter((l) => l.trim());
  const f = filter.trim().toLowerCase();
  const lines = f ? all.filter((l) => l.toLowerCase().includes(f)) : all;
  const tone = { info: '#9fd0ff', warn: '#ffc56e', error: '#ff9a9a' };
  const save = () => {
    const out = `${fs.home()}/Documents/${name} ${which === 'vm' ? 'machine' : 'console'} log.txt`;
    try { fs.write(out, lines.join('\n') + '\n'); setSaved(out); } catch (e) { setSaved(''); }
  };
  return (
    <View style={{ flex: 1, backgroundColor: p.bg }}>
      <View style={{ height: 52, flexDirection: 'row', alignItems: 'center', gap: 8, paddingHorizontal: 16, borderBottomWidth: 1, borderColor: p.line }}>
        <Text style={{ flex: 1, fontSize: 15, fontWeight: 600, color: p.text, textAlign: 'start' }}>{tr('machines.logs.title', { name })}</Text>
        <Segmented small value={which} onChange={setWhich} options={[['vm', tr('machines.logs.machine')], ['serial', tr('machines.logs.console')]]} />
        <Text style={{ fontSize: 12, color: p.dim }}>{tr('machines.logs.filter')}</Text>
        <View style={{ width: 180 }}><TextInput value={filter} onChange={setFilter} /></View>
        <Btn label={paused ? tr('machines.logs.follow') : tr('machines.logs.pause')} onClick={() => setPaused(!paused)} style={{ height: 28 }} />
        <Btn label={tr('machines.logs.copy')} onClick={() => clipboard.set(lines.join('\n'))} style={{ height: 28 }} />
        <Btn label={tr('machines.logs.save')} onClick={save} style={{ height: 28 }} />
      </View>
      <ScrollView style={{ flex: 1, backgroundColor: p.dark ? '#16171b' : 'rgba(0,0,0,0.03)' }} contentStyle={{ paddingHorizontal: 16, paddingVertical: 10 }}>
        {lines.slice(-800).map((l, i) => {
          const m = l.match(/^(\d{4}-\d\d-\d\dT(\d\d:\d\d:\d\d)[.\d]*Z)\s+(.*)$/);
          const lv = levelOf(l);
          return (
            <View key={i} style={{ flexDirection: 'row', gap: 10 }}>
              {m ? <Text style={{ width: 64, fontFamily: 'monospace', fontSize: 12, lineHeight: 1.7, color: p.dim }}>{m[2]}</Text> : null}
              <Text style={{ width: 44, fontFamily: 'monospace', fontSize: 12, lineHeight: 1.7, color: tone[lv] }}>{lv}</Text>
              <Text style={{ flex: 1, fontFamily: 'monospace', fontSize: 12, lineHeight: 1.7, color: p.text, textAlign: 'start', direction: 'ltr', whiteSpace: 'nowrap', overflow: 'hidden' }}>{m ? m[3] : l}</Text>
            </View>
          );
        })}
      </ScrollView>
      <View style={{ height: 32, flexDirection: 'row', alignItems: 'center', gap: 16, paddingHorizontal: 16, borderTopWidth: 1, borderColor: p.line }}>
        <Text style={{ fontSize: 12, color: p.dim }}>{paused ? tr('machines.logs.paused') : tr('machines.logs.following')}</Text>
        <Text style={{ fontSize: 12, color: p.dim }}>{tr('machines.logs.count', { a: lines.length, b: all.length })}</Text>
        <View style={{ flex: 1 }} />
        {saved ? <Text onClick={() => revealPath(saved)} style={{ fontSize: 12, color: '#6cb6ff' }}>{tr('machines.logs.savedAs', { name: saved.split('/').pop() })}</Text> : null}
        <Text style={{ fontSize: 12, color: p.dim }}>{which === 'vm' ? tr('machines.logs.machine') : tr('machines.logs.console')}</Text>
      </View>
    </View>
  );
}
