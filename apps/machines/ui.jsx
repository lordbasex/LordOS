// Pieces both windows use: the Alpha badge, the state dot.
import { View, Text, t as tr } from '@lordos/sdk';

export const TONE = { run: '#30d158', pause: '#ff9f0a', off: '#8e8e93', busy: '#0a84ff', bad: '#ff453a' };
export const has = () => typeof lord !== 'undefined' && !!lord.vm;
export const gb = (mb) => (mb >= 1024 ? `${Math.round((mb / 1024) * 10) / 10} GB` : `${mb} MB`);

export function AlphaBadge() {
  return (
    <View style={{ paddingHorizontal: 6, height: 18, borderRadius: 9, justifyContent: 'center', backgroundColor: 'rgba(255,159,10,0.18)' }}>
      <Text style={{ fontSize: 10, fontWeight: 700, color: '#ff9f0a' }}>{tr('machines.alpha')}</Text>
    </View>
  );
}

export function StateDot({ tone }) { return <View style={{ width: 8, height: 8, borderRadius: 4, backgroundColor: TONE[tone] || TONE.off }} />; }
