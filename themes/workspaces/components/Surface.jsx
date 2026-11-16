// <Surface>: one of the theme's materials (tokens.js): Opaque, Translucent or Crystal. Crystal adds three thin children
// under the content: the sheen (a two-stop gradient from the top), the bright top edge and the darker lower rim. They
// never take clicks. Keep blurred surfaces few and small (docs/WORKSPACES.md, "Effects styles").
import { View } from '@lordos/sdk';
import { forwardRef, memo } from 'react';
import { surfaceStyle } from '../tokens.js';

const none = { pointerEvents: 'none' };

export const Crystal = memo(function Crystal({ t, radius }) {
  const m = t.mat;
  if (!m.sheen) return null;
  return (
    <>
      <View style={{ ...none, position: 'absolute', left: 0, right: 0, top: 0, height: '62%', background: m.sheen, borderTopLeftRadius: radius, borderTopRightRadius: radius }} />
      <View style={{ ...none, position: 'absolute', left: radius * 0.6, right: radius * 0.6, top: 0, height: 1, backgroundColor: m.edge }} />
      <View style={{ ...none, position: 'absolute', left: radius * 0.8, right: radius * 0.8, bottom: 0, height: 1, backgroundColor: m.rim }} />
    </>
  );
});

export const Surface = memo(forwardRef(function Surface({ t, radius = 16, style, children, ...rest }, ref) {
  return (
    <View ref={ref} {...rest} style={{ ...surfaceStyle(t, radius), ...style }}>
      <Crystal t={t} radius={radius} />
      {children}
    </View>
  );
}));
