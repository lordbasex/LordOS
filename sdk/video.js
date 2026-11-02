// video.js — "Optimize video for this computer" (Settings → Displays; docs/VIDEO.md).
//
// When the computer can't decode VP9 or AV1 in hardware (most virtual machines, older PCs, the
// Raspberry Pi 5), video sites that send those codecs make the browser decode them on the CPU and
// the video stutters. LordOS then asks the browsers for H.264 instead:
//   - lord.media.hwDecode() says what the hardware decodes (src/media/hwdec.c, once per boot);
//   - the closed admin action video-optimize writes the browsers' settings from fixed text
//     (Chromium: the managed policy of LordOS's own extension, assets/browser/video; Firefox ESR:
//     a default pref), at login and whenever the setting changes.
// settings.video = { mode: 'auto' | 'always' | 'never', autoLower: false }. Nothing leaves the computer.
import { settings } from './settings.js';
import { admin } from './admin.js';
import { createStore } from './store.js';

export const VIDEO_DEFAULTS = { mode: 'auto', autoLower: false };
export const VIDEO_MODES = ['auto', 'always', 'never'];

// What lord.media.hwDecode() answered (null while it is being found out).
export const videoHardware = createStore(null);
// The last error of video-optimize (a translated message) or ''.
export const videoError = createStore('');

export function videoPrefs(s = settings.get()) {
  const v = s.video || {};
  return {
    mode: VIDEO_MODES.includes(v.mode) ? v.mode : VIDEO_DEFAULTS.mode,
    autoLower: typeof v.autoLower === 'boolean' ? v.autoLower : VIDEO_DEFAULTS.autoLower,
  };
}

export function setVideoPrefs(patch) {
  settings.set((s) => ({ ...s, video: { ...videoPrefs(s), ...patch } }));
}

// The admin action's argument: "<mode> <on|off> <vp9 yes|no> <av1 yes|no>".
export function videoArg(prefs, hw) {
  return `${prefs.mode} ${prefs.autoLower ? 'on' : 'off'} ${hw?.vp9 ? 'yes' : 'no'} ${hw?.av1 ? 'yes' : 'no'}`;
}

// Is the optimization on right now? With Automatic, only when VP9 or AV1 lacks hardware decoding.
export function videoOptimizing(prefs, hw) {
  if (prefs.mode === 'always') return true;
  if (prefs.mode === 'never') return false;
  return !!hw && !(hw.vp9 && hw.av1);
}

let applied = '';
function apply() {
  const hw = videoHardware.get();
  if (!hw) return;
  const arg = videoArg(videoPrefs(), hw);
  if (arg === applied) return;
  applied = arg;
  admin.run('video-optimize', arg, undefined, { quiet: true }).then(() => videoError.set(''), (e) => {
    applied = '';
    videoError.set(e.message);
  });
}

export function startVideo() {
  if (!lord.media?.hwDecode || !lord.admin?.available()) return;
  let tries = 0;
  const poll = () => {
    const hw = lord.media.hwDecode();
    if (hw) { videoHardware.set(hw); apply(); return; }
    if (++tries < 40) setTimeout(poll, 250);       // axia-media answers in well under a second
  };
  poll();
  settings.subscribe(apply);
}
