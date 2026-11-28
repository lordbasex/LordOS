// confirmremove.js — the Store's Remove asks first (in its own module: ui.js stays free of controls, its scenes' SDK
// stand-ins don't have them).
import { createElement as h } from 'react';
import { View, Text, Button, useTheme, typeScale, spacing, t } from '@lordos/sdk';

// Remove asks first, as every destructive action does (QA 0.8.9.6.2: a Flatpak app or a LordOS package went at one
// click): "Remove {name}?" with Cancel and a red Remove. Shown under the app's row or button.
export function ConfirmRemove({ name, onYes, onNo }) {
  const th = useTheme();
  return h(View, { style: { flexDirection: 'row', alignItems: 'center', gap: spacing.s, paddingVertical: spacing.xs } },
    h(Text, { style: { flex: 1, minWidth: 0, fontSize: typeScale.callout, fontWeight: 600, color: th.text, textAlign: 'start' } }, t('store.removeAsk', { name })),
    h(Button, { small: true, label: t('store.cancel'), onClick: onNo }),
    h(Button, { small: true, destructive: true, primary: true, label: t('store.remove'), onClick: onYes }));
}
