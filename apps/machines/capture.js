// capture.js — "Capture mouse" in a machine's window (docs/VIRTUALIZATION.md 6.25): pure decisions, tested by
// server/tests/vm_capture_test.mjs.
//
// Automatic (the default): the guest gets an absolute tablet and the pointer moves freely in and out of the picture.
// Captured: lord-server holds a pointer lock on the picture (server/src/pointerlock.h), the host's pointer is hidden and the
// guest gets relative motion (lord.vm.capture). It ends with ❖Esc or Ctrl+Alt (taken by lord-server before any app), with
// the toolbar's button or the Actions menu, and always when the window loses the keyboard, leaves full screen after an
// automatic capture, the machine stops or apps mode starts. "Capture automatically in full screen" (machine.json
// capture_fullscreen) captures on entering full screen; after ❖Esc there, a click in the picture captures again.

export const CAPTURE_NOTICE_MS = 4000;

// whether the window offers it now: { offered, why } — why names the tooltip's reason ('apps': apps mode shares this
// computer's pointer with the guest's windows; 'off': no running machine with a picture)
export function captureOffer({ appsOn, live }) {
  if (appsOn) return { offered: false, why: 'apps' };
  if (!live) return { offered: false, why: 'off' };
  return { offered: true, why: null };
}

export const CAPTURE_IDLE = Object.freeze({ on: false, auto: false });

// One step: st { on, auto (this capture came from full screen, so it ends with it) } and an event:
//   { type: 'toggle' }                       the button or the menu
//   { type: 'fullscreen', full, autoFs }     the window entered or left full screen
//   { type: 'click', full, autoFs, mouse }   a press in the picture: captures again in full screen with the option, or
//                                            always with the profile's mouse 'capture' (app.json input.mouse)
//   { type: 'focus', mouse }                 the window came to the front (or the machine's picture came up while it was):
//                                            the profile's mouse 'capture' captures
//   { type: 'blur' }                         the window lost the keyboard
//   { type: 'unavailable' }                  not offered any more (apps mode, the machine stopped, no picture)
//   { type: 'ended' }                        lord-server ended it (❖Esc, Ctrl+Alt, the picture went away)
//   { type: 'failed' }                       lord.vm.capture refused
// offered: captureOffer(...).offered now. → { st, cmd: 'capture' | 'release' | null, notice: show the notice }
export function captureStep(st0, ev, offered = true) {
  const st = { ...CAPTURE_IDLE, ...(st0 || {}) };
  const same = { st, cmd: null, notice: false };
  const take = (auto) => (offered && !st.on ? { st: { on: true, auto }, cmd: 'capture', notice: true } : same);
  const free = () => ({ st: { ...CAPTURE_IDLE }, cmd: st.on ? 'release' : null, notice: false });
  switch (ev?.type) {
    case 'toggle': return st.on ? free() : take(false);
    // entering full screen captures when the machine asks for it; leaving ends an automatic capture (one asked for with
    // the button stays)
    case 'fullscreen': return ev.full ? (ev.autoFs ? take(true) : same) : st.on && st.auto ? free() : same;
    case 'click': return (ev.full && ev.autoFs) || ev.mouse === 'capture' ? take(ev.mouse !== 'capture') : same;
    case 'focus': return ev.mouse === 'capture' ? take(false) : same;
    case 'blur': case 'unavailable': return free();
    case 'ended': case 'failed': return { st: { ...CAPTURE_IDLE }, cmd: null, notice: false };
    default: return same;
  }
}

// lord.vm.poll's counter of ended captures moved on while the window thought it held one: lord-server ended it
export function captureEndedByServer(st, prevEnds, poll) {
  return !!st?.on && !!poll && typeof poll.captureEnds === 'number' && poll.captureEnds !== prevEnds && !poll.captured;
}

// the toolbar button's tooltip key
export function captureLabelKey({ on, why }) {
  if (why === 'apps') return 'machines.tb.captureApps';
  return on ? 'machines.tb.captureOn' : 'machines.tb.capture';
}
