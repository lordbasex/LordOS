// Settings → Appearance → "Browsers: tabs in the title bar" (docs/WAYLAND.md, "The switch in
// Settings"). On: Chromium, Brave and Firefox draw one bar with their tabs and LordOS's window buttons;
// off: they use LordOS's frame. The SDK (sdk/browserframe.js) writes that one preference in the
// user's own browser profiles through lord.browserFrame.
import { View, Button, Toggle, useLocale, useSettings, useStore, browserFrameResult, browserFrameState, browserTabsInTitlebar, setBrowserTabsInTitlebar } from '@lordos/sdk';
import { useEffect, useState } from 'react';
import { Section, SettingRow, Note, Actions } from './ui.jsx';

// What the last change did, as notes under the switch.
function ResultNotes({ r, pending }) {
  const { t: tr } = useLocale();
  if (!r) return null;
  const failed = (r.chromium?.failed | 0) + (r.firefox?.failed | 0);
  const out = [];
  if (pending) out.push(<Note key="open" tone="warn">{tr('settings.browserTabs.chromiumOpen')}</Note>);
  if (failed) out.push(<Note key="failed" tone="error">{tr('settings.browserTabs.failed', { count: failed })}</Note>);
  if (r.firefox?.kept) out.push(<Note key="kept">{tr('settings.browserTabs.firefoxKept')}</Note>);
  if (!pending && !failed && (r.chromium?.changed || r.firefox?.changed)) out.push(<Note key="ok" tone="ok">{tr('settings.browserTabs.done')}</Note>);
  return out.length ? out : null;
}

export function BrowserFrameSettings() {
  const { t: tr } = useLocale();
  const [s] = useSettings();
  const on = browserTabsInTitlebar(s);
  const result = useStore(browserFrameResult);
  // Chromium and Brave profiles that don't follow the switch (made before 0.8.4, a new Brave profile, or changed in the browser)
  const [state, setState] = useState(null);
  useEffect(() => { setState(browserFrameState()); }, [on, result]);
  const other = state ? (on ? state.off : state.on) : 0;
  const pick = (v) => setBrowserTabsInTitlebar(v);
  return (
    <View style={{ gap: 6 }}>
      <Section footer={tr('settings.browserTabs.hint')}>
        <SettingRow label={tr('settings.browserTabs.label')}>
          <Toggle small on={on} onChange={pick} />
        </SettingRow>
        {other > 0 && !s.browserFramePending ? (
          <SettingRow label={tr(on ? 'settings.browserTabs.othersOff' : 'settings.browserTabs.othersOn', { count: other })}>
            <Actions><Button label={tr('settings.browserTabs.applyAll')} onClick={() => pick(on)} /></Actions>
          </SettingRow>
        ) : null}
      </Section>
      <ResultNotes r={result} pending={!!s.browserFramePending} />
    </View>
  );
}
