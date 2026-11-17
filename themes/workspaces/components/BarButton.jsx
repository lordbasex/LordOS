// <BarButton>: one control of the theme's bars, in the way of a video player's controls: each sits on its own rounded
// pill that shows only under the pointer (t.itemHover), its glyph in the bars' icon colour (t.iconFg, the Context
// Spine's), brighter under the pointer. glyph(color) draws it; children (a label, the clock) follow the glyph.
// <BarSeparator>: the thin line between a bar's groups.
// <FocusRing fid>: the ring a control draws while the bars have the keyboard and it is the one (barfocus.js); only the
// ring re-renders when the keyboard moves, outside the control's box so nothing is laid out again.
import { View, useStore } from '@lordos/sdk';
import { forwardRef, memo, useState } from 'react';
import { barFocus } from '../barfocus.js';

export const FocusRing = memo(function FocusRing({ t, fid, radius = 12 }) {
  const on = useStore(barFocus, (f) => f === fid);
  if (!on) return null;
  return <View style={{ position: 'absolute', left: -3, top: -3, right: -3, bottom: -3, borderRadius: radius + 3, border: `2px solid ${t.focusRing || t.accent}`, pointerEvents: 'none' }} />;
});

// a video player's proportions (a 48 px pill, 40 px buttons, 4 px inside it) at the bars' 44 px
export const BAR_ITEM = 36;

export const BarButton = memo(forwardRef(function BarButton({ t, glyph, title, onClick, active, disabled, style, children, fid }, ref) {
  const [hover, setHover] = useState(false);
  const fg = active ? t.onAccent : hover && !disabled ? t.iconFgHover : t.iconFg;
  return (
    <View ref={ref} title={title} onClick={disabled ? undefined : onClick}
      onMouseEnter={() => setHover(true)} onMouseLeave={() => setHover(false)}
      style={{ height: BAR_ITEM, minWidth: BAR_ITEM, borderRadius: BAR_ITEM / 2, flexDirection: 'row', alignItems: 'center', justifyContent: 'center',
        gap: 6, paddingHorizontal: children ? 12 : 0, opacity: disabled ? 0.45 : 1,
        backgroundColor: active ? t.accent : hover && !disabled ? t.itemHover : 'transparent', ...style }}>
      {glyph ? glyph(fg) : null}
      {typeof children === 'function' ? children(fg) : children}
      {fid ? <FocusRing t={t} fid={fid} radius={BAR_ITEM / 2} /> : null}
    </View>
  );
}));

export const BarSeparator = ({ t }) => <View style={{ width: 1, height: 18, marginHorizontal: 4, backgroundColor: t.barSeparator, pointerEvents: 'none' }} />;
