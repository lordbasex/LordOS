// browserframe.js — Settings → Appearance → "Browsers: tabs in the title bar" (docs/WAYLAND.md,
// "The switch in Settings"). On (the default): Chromium, Brave (0.8.8.4, #39) and Firefox draw one bar
// with their tabs and LordOS's window buttons. Off: they use LordOS's frame.
//
// lord.browserFrame (server/src/hw/brframe.c) writes that one preference in the user's own
// profiles, and only when the switch is turned (never at login on its own):
//   - Chromium and Brave ('chromium' below means both): Preferences, only while that browser is
//     closed. If one is open, the change waits in settings.browserFramePending and is applied as soon
//     as it quits (checked every 5 s while pending, and at login);
//   - Firefox: a line in user.js, read the next time Firefox opens; at login lord.browserFrame.tidy()
//     removes it again once Firefox has read it, so a later choice in Firefox's own settings sticks.
import { settings } from './settings.js';
import { createStore } from './store.js';

// The last result of lord.browserFrame.apply: { chromium: {...}, firefox: {...} } counts, or null.
export const browserFrameResult = createStore(null);

// The notes under the switch for the last result (pure, tested in server/tests/browserframe_notes_test.mjs):
// [{ key, tone, text, vars }]. Nothing for a change that simply worked (#222: a green "Done" stayed after every
// toggle, and the footer already says when it applies).
export function browserFrameNotes(r, pending) {
  if (!r) return [];
  const failed = (r.chromium?.failed | 0) + (r.firefox?.failed | 0);
  const out = [];
  if (pending) out.push({ key: 'open', tone: 'warn', text: 'settings.browserTabs.chromiumOpen' });
  if (failed) out.push({ key: 'failed', tone: 'error', text: 'settings.browserTabs.failed', vars: { count: failed } });
  if (r.firefox?.kept) out.push({ key: 'kept', text: 'settings.browserTabs.firefoxKept' });
  return out;
}

const api = () => typeof lord !== 'undefined' && lord.browserFrame;

export const browserTabsInTitlebar = (s = settings.get()) => s.browserTabsInTitlebar !== false;

let timer = 0;
function retryChromium() {
  clearTimeout(timer);
  if (!api() || !settings.get().browserFramePending) return;
  if (lord.browserFrame.chromiumRunning()) { timer = setTimeout(retryChromium, 5000); return; }
  const r = lord.browserFrame.apply(browserTabsInTitlebar(), 'chromium');
  if (r) browserFrameResult.set((cur) => ({ ...(cur || {}), chromium: r.chromium }));
  if (r && r.chromium.running) timer = setTimeout(retryChromium, 5000);   // reopened meanwhile
  else settings.set({ browserFramePending: false });
}

export function setBrowserTabsInTitlebar(on) {
  settings.set({ browserTabsInTitlebar: !!on });
  if (!api()) return;
  const r = lord.browserFrame.apply(!!on, 'all');
  browserFrameResult.set(r);
  settings.set({ browserFramePending: !!r?.chromium?.running });
  retryChromium();
}

// Chromium and Brave profiles whose own settings don't match the switch (made before 0.8.4, a new
// Brave profile with its Classic theme, or changed in the browser): { on, off } or null.
export function browserFrameState() {
  if (!api()) return null;
  const s = lord.browserFrame.state();
  return s ? { on: s.chromiumOn | 0, off: s.chromiumOff | 0 } : null;
}

export function startBrowserFrame() {
  if (!api()) return;
  setTimeout(() => {
    lord.browserFrame.tidy();
    retryChromium();
  }, 2000);
}
