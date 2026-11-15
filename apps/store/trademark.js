// trademark.js — the trademark notice under the Store's Web apps section (webapps.js) and at the end of each web app's
// page (webdetail.js), 0.8.8.9: the names, logos and icons shown belong to their owners and only identify the services,
// LordOS is not affiliated with, endorsed or sponsored by them, and it runs the sites unmodified. The same notice as
// the website's (the LordOS Web Runtime page, the paper's §5.4 and the trademark policy).
import { createElement as h } from 'react';
import { Text, useTheme, typeScale, spacing, t } from '@lordos/sdk';

export function TrademarkNote({ style }) {
  const th = useTheme();
  return h(Text, { style: { fontSize: typeScale.caption, color: th.textDim, lineHeight: 1.4, maxWidth: 680, paddingHorizontal: spacing.xs, textAlign: 'start', ...style } },
    t('webapps.store.trademark'));
}
