// "Ask before each paste" (docs/VIRTUALIZATION.md 6.8, machine.json clipboard_ask): the small bar over a machine's
// picture that asks before this computer's clipboard goes into the machine, or before what the machine copied comes to
// this computer, with a one-line preview. The machine's window shows it, and in apps mode the guest window in front.
// Keys: Esc says no; Enter says yes only on the focused button (Tab reaches them); nothing goes to the guest meanwhile.
import { View, Text, addListener, t as tr } from '@lordos/sdk';
import { useEffect, useState } from 'react';
import { clipPreviewLine } from './model.js';

function BarBtn({ label, primary, onPress }) {
  const [focus, setFocus] = useState(false);
  const [hover, setHover] = useState(false);
  return (
    <View focusable onClick={onPress} onFocus={() => setFocus(true)} onBlur={() => setFocus(false)}
      onKeyDown={(e) => { if (e.key === 'Enter' || e.key === ' ') { e.preventDefault?.(); onPress(); } }}
      onMouseEnter={() => setHover(true)} onMouseLeave={() => setHover(false)}
      style={{ height: 28, paddingHorizontal: 14, borderRadius: 14, alignItems: 'center', justifyContent: 'center',
        backgroundColor: primary ? '#0a84ff' : 'rgba(255,255,255,0.12)', opacity: hover ? 0.86 : 1,
        border: focus ? '2px solid #9ccaff' : '2px solid transparent' }}>
      <Text style={{ fontSize: 13, fontWeight: 600, color: '#fff', whiteSpace: 'nowrap' }}>{label}</Text>
    </View>
  );
}

// ask: { kind: 'in' | 'push' | 'out', name, preview } (preview: lord.vm.clipboardPreview's), onAnswer(yes)
export function ClipAskBar({ ask, onAnswer, top = 12 }) {
  useEffect(() => (ask ? addListener('keydown', (e) => { if (e.key === 'Escape') { e.preventDefault?.(); onAnswer(false); } }) : undefined), [ask?.kind, ask?.preview?.hash]);
  if (!ask) return null;
  const out = ask.kind === 'out';
  const pv = clipPreviewLine(ask.preview);
  const line = pv.kind === 'image' ? tr('machines.clip.picture', { w: pv.w, h: pv.h }) : pv.text;
  const yes = tr(out ? 'machines.clip.bring' : 'machines.clip.paste'), no = tr(out ? 'machines.clip.ignore' : 'machines.clip.dontPaste');
  return (
    <View style={{ position: 'absolute', top, left: 0, right: 0, alignItems: 'center' }}>
      <View style={{ maxWidth: 620, minWidth: 320, flexDirection: 'row', alignItems: 'center', gap: 12, paddingHorizontal: 14, paddingVertical: 10,
        borderRadius: 12, backgroundColor: 'rgba(28,28,33,0.96)', border: '1px solid rgba(255,255,255,0.12)' }}>
        <View style={{ flex: 1, gap: 2 }}>
          <Text style={{ fontSize: 13, fontWeight: 700, color: '#f2f2f5', textAlign: 'start' }}>
            {tr(out ? 'machines.clip.askOut' : 'machines.clip.askIn', { name: ask.name })}
          </Text>
          {line ? <Text style={{ fontSize: 12, color: '#c9c9d1', textAlign: 'start' }}>{line}</Text> : null}
          <Text style={{ fontSize: 11, color: '#8a8a94', textAlign: 'start' }}>{tr('machines.clip.keys', { yes, no })}</Text>
        </View>
        <BarBtn label={no} onPress={() => onAnswer(false)} />
        <BarBtn primary label={yes} onPress={() => onAnswer(true)} />
      </View>
    </View>
  );
}
