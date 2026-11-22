// fhlook.js — a Flathub app's page: how it shows its windows (Wayland, X11 through Xwayland) and the toolkit it is
// built with, before it is installed. Both come from the metadata the page already fetched (F.info: `flatpak
// remote-info --show-metadata`), read by sdk/flathubmodel.js displayOf and toolkitOf (docs/FLATPAK.md, "Showing them").
import { createElement as h } from 'react';
import { View, Text, GroupBox, useTheme, flathubModel as M, typeScale, spacing, t } from '@lordos/sdk';

function Row({ th, label, value, hint }) {
  return h(View, { style: { flexDirection: 'row', gap: spacing.m, paddingVertical: spacing.s + 2, alignItems: 'flex-start' } },
    h(Text, { style: { width: 110, flexShrink: 0, fontSize: typeScale.callout, color: th.textDim, lineHeight: 1.4, textAlign: 'start' } }, label),
    h(View, { style: { flex: 1, minWidth: 0, gap: 2 } },
      h(Text, { style: { fontSize: typeScale.body, fontWeight: 600, color: th.text, lineHeight: 1.4, textAlign: 'start' } }, value),
      hint ? h(Text, { style: { fontSize: typeScale.caption, color: th.textDim, lineHeight: 1.4, textAlign: 'start' } }, hint) : null));
}

// Nothing until the metadata is there (the permissions say "loading" or why it failed).
export function DisplayToolkit({ meta }) {
  const th = useTheme();
  if (!meta || meta.error) return null;
  const d = M.displayOf(meta), tk = M.toolkitOf(meta);
  if (!d || !tk) return null;
  return h(View, null,
    h(Text, { style: { fontSize: typeScale.body, fontWeight: 600, color: th.text, paddingHorizontal: spacing.xs, marginBottom: 6, textAlign: 'start' } }, t('flathub.look.title')),
    h(GroupBox, null,
      h(Row, { key: 'd', th, label: t('flathub.look.display'), value: t(`flathub.display.${d}`), hint: t(`flathub.display.${d}Hint`) }),
      h(Row, { key: 't', th, label: t('flathub.look.toolkit'), value: t(`flathub.toolkit.${tk.kind}`),
        hint: tk.runtime ? t('flathub.look.runtime', { runtime: [tk.runtime, tk.version].filter(Boolean).join(' ') }) : '' })));
}
