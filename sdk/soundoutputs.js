// soundoutputs.js — the outputs to choose from, next to the volume of a theme's Control Center or quick settings
// (issue #58). The theme shows a button by its volume slider when there is more than one output and, when it is
// pressed, this list:
//
//   {outputs.length > 1 && <View onClick={() => setOpen(!open)}>…</View>}
//   {open && <SoundOutputList onPick={() => setOpen(false)} />}
//
// A click makes that output the default one (the same call as Settings → Sound): the slider then moves that output,
// and PipeWire keeps each output's own volume, so a headset and the speakers each come back at the level they had.
import { createElement as h, useState } from 'react';
import { View, Text } from '@lordos/runtime';
import { useTheme } from './theme.js';
import { t } from './i18n.js';
import { sound, useSound } from './sound.js';
import { outputChoices } from './menubarmodel.js';

export const useSoundOutputs = () => outputChoices(useSound());

function Row({ th, o, onPick }) {
  const [hover, setHover] = useState(false);
  return h(View, {
    onClick: () => { if (!o.default) sound.setDefault('sink', o.name); onPick?.(o); },
    onMouseEnter: () => setHover(true), onMouseLeave: () => setHover(false),
    style: { minHeight: 28, paddingHorizontal: 8, borderRadius: 6, flexDirection: 'row', alignItems: 'center', gap: 8, backgroundColor: hover ? th.hover : 'transparent' },
  },
  h(Text, { style: { width: 14, fontSize: 12, color: th.accent } }, o.default ? '✓' : ''),
  h(Text, { style: { flex: 1, fontSize: 12, color: th.text, textAlign: 'start', whiteSpace: 'nowrap', textOverflow: 'ellipsis', overflow: 'hidden' } }, o.label));
}

export function SoundOutputList({ onPick, style }) {
  const th = useTheme();
  const list = useSoundOutputs();
  if (list.length < 2) return null;
  return h(View, { style: [{ gap: 2 }, style] },
    h(Text, { style: { fontSize: 11, fontWeight: 600, color: th.textDim, textAlign: 'start', paddingHorizontal: 8, paddingBottom: 2 } }, t('shell.cc.output')),
    ...list.map((o) => h(Row, { key: o.name, th, o, onPick })));
}
