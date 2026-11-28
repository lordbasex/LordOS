// adminsyncmodel.js — when the shell applies a setting through a closed administrator action that only mirrors it
// (browser-restore, video-optimize: "make the machine match this choice"), whether to run it now. Pure, node-tested by
// server/tests/admin_sync_test.mjs.
//
// The choice already applied: nothing. A choice whose last attempt failed: not again for RETRY_MS, whatever else in the
// settings changed meanwhile. Before (BUG-10b of the ws170 QA) a failure forgot what was applied, so every later change
// of any setting ran the action again: an AI agent's clicks in Settings each brought refused administrator requests
// ("lord.admin.request refused (an AI agent's input)") for actions it never asked for.
//   state: { applied, failed, failedAt }   step(state, arg, now) → { run, state }   done(state, arg, ok, now) → state
export const RETRY_MS = 60000;
export const SYNC_START = { applied: '', failed: '', failedAt: 0 };
export function syncStep(state, arg, now) {
  const s = state || SYNC_START;
  if (arg === s.applied) return { run: false, state: s };
  if (arg === s.failed && now - s.failedAt >= 0 && now - s.failedAt < RETRY_MS) return { run: false, state: s };
  return { run: true, state: { ...s, applied: arg } };
}
export function syncDone(state, arg, ok, now) {
  const s = state || SYNC_START;
  if (ok) return { ...s, failed: '', failedAt: 0 };
  return { applied: s.applied === arg ? '' : s.applied, failed: arg, failedAt: now };
}
// the input being handled is an AI agent's (lord.hands.injecting; false without Hands)
// (refused, as while a community theme's code is on the stack, counts as not injecting)
export const agentInput = () => {
  try { return typeof lord !== 'undefined' && typeof lord.hands?.injecting === 'function' && !!lord.hands.injecting(); } catch (e) { return false; }
};
