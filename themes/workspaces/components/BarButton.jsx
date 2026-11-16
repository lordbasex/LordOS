// <BarButton>: one control of the theme's bars, in the way of a video player's controls: each sits on its own rounded
// pill that shows only under the pointer (t.itemHover), its glyph in the bars' icon colour (t.iconFg, the Context
// Spine's), brighter under the pointer. glyph(color) draws it; children (a label, the clock) follow the glyph.
// <BarSeparator>: the thin line between a bar's groups.
import { View } from '@lordos/sdk';
import { forwardRef, memo, useState } from 'react';

export const BAR_ITEM = 32;

export const BarButton = memo(forwardRef(function BarButton({ t, glyph, title, onClick, active, disabled, style, children }, ref) {
  const [hover, setHover] = useState(false);
  const fg = active ? t.onAccent : hover && !disabled ? t.iconFgHover : t.iconFg;
  return (
    <View ref={ref} title={title} onClick={disabled ? undefined : onClick}
      onMouseEnter={() => setHover(true)} onMouseLeave={() => setHover(false)}
      style={{ height: BAR_ITEM, minWidth: BAR_ITEM, borderRadius: BAR_ITEM / 2, flexDirection: 'row', alignItems: 'center', justifyContent: 'center',
        gap: 6, paddingHorizontal: children ? 10 : 0, opacity: disabled ? 0.45 : 1,
        backgroundColor: active ? t.accent : hover && !disabled ? t.itemHover : 'transparent', ...style }}>
      {glyph ? glyph(fg) : null}
      {typeof children === 'function' ? children(fg) : children}
    </View>
  );
}));

export const BarSeparator = ({ t }) => <View style={{ width: 1, height: 18, marginHorizontal: 4, backgroundColor: t.barSeparator, pointerEvents: 'none' }} />;
