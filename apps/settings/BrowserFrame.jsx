// Settings → Appearance → "Browsers: tabs in the title bar" (docs/WAYLAND.md, "The switch in
// Settings"). On: Chromium, Brave and Firefox draw one bar with their tabs and LordOS's window buttons;
// off: they use LordOS's frame. The SDK (sdk/browserframe.js) writes that one preference in the
// user's own browser profiles through lord.browserFrame.
import { View, Button, Toggle, useLocale, useSettings, useStore, browserFrameResult, browserFrameNotes, browserFrameState, browserTabsInTitlebar, setBrowserTabsInTitlebar } from '@lordos/sdk';
import { useEffect, useState } from 'react';
import { Section, SettingRow, Note, Actions } from './ui.jsx';

// What the last change did, as notes under the switch: only what needs the person (a browser open, a profile
// that failed or that Firefox's user.js keeps). A change that simply worked says nothing (#222): the switch
// shows it, and the footer already says it applies the next time each browser opens.
function ResultNotes({ r, pending }) {
  const { t: tr } = useLocale();
  const notes = browserFrameNotes(r, pending);
  return notes.length ? notes.map((n) => <Note key={n.key} tone={n.tone}>{tr(n.text, n.vars)}</Note>) : null;
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
