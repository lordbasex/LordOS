// codeblock.jsx — <CommandBlock text />: a command shown as code (monospace, left to right in any language) with a Copy
// button that puts exactly that text on the clipboard and says "Copied" for a moment. Used where a page tells the person
// to run something themselves (Settings → AI → Hands' `claude mcp add …`). It only copies: nothing is typed or run.
import { createElement as h, useEffect, useRef, useState } from 'react';
import { View, Text } from '@lordos/runtime';
import { useTheme } from './theme.js';
import { t } from './i18n.js';
import { clipboard } from './clipboard.js';
import { Button } from './components.jsx';

export function CommandBlock({ text, style }) {
  const th = useTheme();
  const [copied, setCopied] = useState(false);
  const timer = useRef(null);
  useEffect(() => () => clearTimeout(timer.current), []);
  const copy = () => {
    try { clipboard.set(String(text)); } catch (e) { return; }
    setCopied(true);
    clearTimeout(timer.current);
    timer.current = setTimeout(() => setCopied(false), 2000);
  };
  return h(View, { style: [{ flexDirection: 'row', alignItems: 'center', gap: 8, paddingVertical: 6, paddingHorizontal: 8, borderRadius: 8, backgroundColor: th.control }, style] },
    h(View, { style: { flex: 1, minWidth: 0, direction: 'ltr' } },
      h(Text, { style: { fontFamily: 'monospace', fontSize: 12, color: th.text, textAlign: 'left' } }, String(text))),
    h(Button, { small: true, label: copied ? t('shell.code.copied') : t('shell.code.copy'), onClick: copy }));
}
