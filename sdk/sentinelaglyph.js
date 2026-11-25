// sentinelaglyph.js — Sentinela's icon for its notifications and its Settings page: a shield on a violet tile.
import { createElement as h } from 'react';
import { View } from '@lordos/runtime';

export function ShieldGlyph({ size = 34 }) {
  const s = size / 34;
  return h(View, { style: { width: size, height: size, borderRadius: 8 * s, backgroundColor: '#5a3a9a', alignItems: 'center', justifyContent: 'center', direction: 'ltr' } },
    h(View, { style: { width: 18 * s, height: 21 * s, alignItems: 'center' } },
      h(View, { style: { width: 18 * s, height: 12 * s, borderTopLeftRadius: 3 * s, borderTopRightRadius: 3 * s, backgroundColor: '#ffffff' } }),
      h(View, { style: { width: 13 * s, height: 6 * s, borderBottomLeftRadius: 7 * s, borderBottomRightRadius: 7 * s, backgroundColor: '#ffffff' } }),
      h(View, { style: { position: 'absolute', top: 4 * s, width: 8 * s, height: 8 * s, borderRadius: 4 * s, backgroundColor: '#e8457f' } })));
}
