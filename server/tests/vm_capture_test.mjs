// vm_capture_test.mjs — apps/machines/capture.js: "Capture mouse" in a machine's window (docs/VIRTUALIZATION.md 6.25).
// node server/tests/vm_capture_test.mjs
import { captureOffer, captureStep, captureEndedByServer, captureLabelKey, CAPTURE_IDLE } from '../../apps/machines/capture.js';

let pass = 0, fail = 0;
function eq(name, got, want) {
  const g = JSON.stringify(got), w = JSON.stringify(want);
  if (g === w) { pass++; } else { fail++; console.log(`✗ ${name}: want ${w}, got ${g}`); }
}
const run = (st, evs, offered = true) => evs.reduce((acc, ev) => {
  const r = captureStep(acc.st, ev, offered);
  return { st: r.st, cmds: [...acc.cmds, r.cmd], notices: acc.notices + (r.notice ? 1 : 0) };
}, { st, cmds: [], notices: 0 });

// offered: never in apps mode (its windows share this computer's pointer), only with a running picture
eq('offer: apps mode', captureOffer({ appsOn: true, live: true }), { offered: false, why: 'apps' });
eq('offer: no picture', captureOffer({ appsOn: false, live: false }), { offered: false, why: 'off' });
eq('offer: a running picture', captureOffer({ appsOn: false, live: true }), { offered: true, why: null });
eq('label', ['apps', null].map((why) => [captureLabelKey({ on: false, why }), captureLabelKey({ on: true, why })]),
  [['machines.tb.captureApps', 'machines.tb.captureApps'], ['machines.tb.capture', 'machines.tb.captureOn']]);

// the button: on and off, the notice each time it captures
eq('toggle: captures, then releases', run(CAPTURE_IDLE, [{ type: 'toggle' }, { type: 'toggle' }]), { st: CAPTURE_IDLE, cmds: ['capture', 'release'], notices: 1 });
eq('toggle: not offered, nothing', run(CAPTURE_IDLE, [{ type: 'toggle' }], false), { st: CAPTURE_IDLE, cmds: [null], notices: 0 });
eq('a missing state is idle', captureStep(undefined, { type: 'toggle' }).st, { on: true, auto: false });
eq('an unknown event changes nothing', captureStep({ on: true, auto: true }, { type: 'what' }), { st: { on: true, auto: true }, cmd: null, notice: false });

// focus loss and unavailability always release; nothing to release when not captured
eq('blur releases', run({ on: true, auto: false }, [{ type: 'blur' }]).cmds, ['release']);
eq('blur when free: nothing', run(CAPTURE_IDLE, [{ type: 'blur' }]).cmds, [null]);
eq('apps mode / stopped releases', run({ on: true, auto: true }, [{ type: 'unavailable' }]), { st: CAPTURE_IDLE, cmds: ['release'], notices: 0 });

// lord-server ended it (❖Esc, Ctrl+Alt, the picture gone): nothing to send back
eq('ended by the keys', run({ on: true, auto: false }, [{ type: 'ended' }]), { st: CAPTURE_IDLE, cmds: [null], notices: 0 });
eq('lord.vm.capture refused', run({ on: true, auto: false }, [{ type: 'failed' }]).st, CAPTURE_IDLE);

// full screen with "Capture automatically in full screen"
const FS = { type: 'fullscreen', full: true, autoFs: true }, WIN = { type: 'fullscreen', full: false, autoFs: true };
eq('full screen captures, leaving it releases', run(CAPTURE_IDLE, [FS, WIN]), { st: CAPTURE_IDLE, cmds: ['capture', 'release'], notices: 1 });
eq('full screen without the option: nothing', run(CAPTURE_IDLE, [{ ...FS, autoFs: false }, { ...WIN, autoFs: false }]).cmds, [null, null]);
eq('a capture asked for with the button stays after full screen', run(CAPTURE_IDLE, [{ type: 'toggle' }, FS, WIN]), { st: { on: true, auto: false }, cmds: ['capture', null, null], notices: 1 });
eq('full screen while captured: not captured twice', run({ on: true, auto: false }, [FS]).cmds, [null]);
eq('full screen not offered (apps mode): nothing', run(CAPTURE_IDLE, [FS], false).cmds, [null]);
// ❖Esc in full screen, then a click in the picture captures again; outside full screen a click doesn't
const CLICK = { type: 'click', full: true, autoFs: true };
eq('❖Esc, then a click: captured again', run(CAPTURE_IDLE, [FS, { type: 'ended' }, CLICK]), { st: { on: true, auto: true }, cmds: ['capture', null, 'capture'], notices: 2 });
eq('a click while captured: nothing', run({ on: true, auto: true }, [CLICK]).cmds, [null]);
eq('a click in a window: nothing', run(CAPTURE_IDLE, [{ ...CLICK, full: false }]).cmds, [null]);
eq('a click in full screen without the option: nothing', run(CAPTURE_IDLE, [{ ...CLICK, autoFs: false }]).cmds, [null]);
eq('re-captured by a click, released by leaving full screen', run(CAPTURE_IDLE, [FS, { type: 'ended' }, CLICK, WIN]).cmds, ['capture', null, 'capture', 'release']);

// the profile's mouse 'capture' (app.json input.mouse): captured when the window comes to the front, and by a click after ❖Esc
eq('focus with mouse capture', run(CAPTURE_IDLE, [{ type: 'focus', mouse: 'capture' }]), { st: { on: true, auto: false }, cmds: ['capture'], notices: 1 });
eq('focus with mouse auto: nothing', run(CAPTURE_IDLE, [{ type: 'focus', mouse: 'auto' }]).cmds, [null]);
eq('focus not offered (apps mode): nothing', run(CAPTURE_IDLE, [{ type: 'focus', mouse: 'capture' }], false).cmds, [null]);
eq('mouse capture: ❖Esc, a click captures again, in a window too', run(CAPTURE_IDLE, [{ type: 'focus', mouse: 'capture' }, { type: 'ended' }, { type: 'click', full: false, autoFs: false, mouse: 'capture' }]).cmds, ['capture', null, 'capture']);
eq('mouse capture: blur releases, focus again captures', run(CAPTURE_IDLE, [{ type: 'focus', mouse: 'capture' }, { type: 'blur' }, { type: 'focus', mouse: 'capture' }]).cmds, ['capture', 'release', 'capture']);
eq('mouse capture survives leaving full screen (not automatic)', run(CAPTURE_IDLE, [{ type: 'focus', mouse: 'capture' }, { type: 'fullscreen', full: true, autoFs: true }, { type: 'fullscreen', full: false, autoFs: true }]).st, { on: true, auto: false });

// the poll: lord-server's count of ended captures moved while the window held one
eq('poll: ended by lord-server', captureEndedByServer({ on: true }, 2, { captured: false, captureEnds: 3 }), true);
eq('poll: still captured', captureEndedByServer({ on: true }, 2, { captured: true, captureEnds: 2 }), false);
eq('poll: not held here', captureEndedByServer({ on: false }, 2, { captured: false, captureEnds: 3 }), false);
eq('poll: same count (not yet captured there)', captureEndedByServer({ on: true }, 3, { captured: false, captureEnds: 3 }), false);
eq('poll: an older lord-server without the field', captureEndedByServer({ on: true }, 0, { captured: false }), false);
eq('poll: no answer', captureEndedByServer({ on: true }, 0, null), false);

console.log(`vm-capture-test: ${pass} passed, ${fail} failed`);
process.exit(fail ? 1 : 0);
