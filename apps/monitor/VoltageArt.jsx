// Monitor → Voltage: the machine's board with its power path in the color of the power state, as Cooling draws the
// case with its fan. Drawn with views; nothing here moves, so it costs nothing while the view is open.
import { View, useTheme } from '@lordos/sdk';
import { BOARD_ART } from './voltart.js';

const Box = ([x, y, w, h, color, r], tone, key) => (
  <View key={key} style={{ position: 'absolute', left: x, top: y, width: w, height: h, borderRadius: r, backgroundColor: color || tone }} />
);

export function VoltageArt({ variant, tone }) {
  const t = useTheme();
  const art = BOARD_ART[variant];
  return (
    <View style={{ width: 280, height: 156, borderRadius: 14, overflow: 'hidden', direction: 'ltr', backgroundColor: t.mode === 'dark' ? 'rgba(255,255,255,0.05)' : 'rgba(0,0,0,0.05)' }}>
      {art.boxes.map((b, i) => Box(b, tone, i))}
      {art.power.map((b, i) => Box(b, tone, `p${i}`))}
    </View>
  );
}
