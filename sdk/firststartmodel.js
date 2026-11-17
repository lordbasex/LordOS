// firststartmodel.js — the first-start desktop chooser's logic (0.8.9, docs/SETTINGS.md "First start"), as pure functions
// with no imports and no lord.* calls (tested by server/tests/firststart_model_test.mjs). sdk/firststart.jsx draws it.
//
// Who sees it: the first session of an account on an installed system. Not the live ISO, not the OVA's first boot (the
// installer runs there), not the login screen (it never calls startShell) and not anyone who has already used LordOS:
// settings.desktopChosen is true for them, set by the per-user migration (markExistingUser) on a settings file that
// shows an earlier session, or by finishing the chooser.
//
// Its state lives in settings.firstStart while it is open, because step 2 switches the theme for real (themes.use
// discards the JS context): { step: 1 | 2 | 3, pick: theme id | null, order: [theme ids], pos: { x, y } | null }.

// The desktops it offers: the bundled ones that are installed. A third-party theme is chosen later in Settings.
export const DESKTOPS = ['default', 'classic', 'workspaces'];
export const FLOAT_W = 330;                 // the floating box of step 2
const FLOAT_MARGIN = 16;

// The per-user migration: a settings file written by an earlier session (the welcome tour was shown, a session saved
// what was open, or a desktop style was chosen) belongs to someone who already uses LordOS, who never sees the chooser.
// A file the installer, the OVA's first boot or Settings → Users wrote for a new account has none of these.
export function markExistingUser(s) {
  if (!s || typeof s !== 'object') return s;
  const used = s.welcomeShown === true || (s.sessionSaved != null && typeof s.sessionSaved === 'object') || (typeof s.theme === 'string' && s.theme !== '');
  return used ? { ...s, desktopChosen: true } : s;
}

// Whether this session shows it: info is lord.system.info() (live, firstAccount), available the desktops installed.
export function shouldShow(s, info, available) {
  if (!s || s.desktopChosen === true) return false;
  if (!info || info.live || info.firstAccount) return false;
  return Array.isArray(available) && available.length >= 2;
}

// A small seeded generator (mulberry32), so a test can repeat an order; the chooser seeds it from the clock.
export function seeded(seed) {
  let a = (seed >>> 0) || 1;
  return () => {
    a = (a + 0x6D2B79F5) >>> 0;
    let x = a;
    x = Math.imul(x ^ (x >>> 15), x | 1);
    x ^= x + Math.imul(x ^ (x >>> 7), x | 61);
    return ((x ^ (x >>> 14)) >>> 0) / 4294967296;
  };
}

// The cards' order: a random permutation every time it starts, so none comes first by design.
export function shuffle(list, rand = Math.random) {
  const out = [...list];
  for (let i = out.length - 1; i > 0; i--) {
    const j = Math.floor(rand() * (i + 1));
    [out[i], out[j]] = [out[j], out[i]];
  }
  return out;
}

export function startState(available, rand = Math.random) {
  return { step: 1, pick: null, order: shuffle(DESKTOPS.filter((id) => available.includes(id)), rand), pos: null };
}

const isPos = (p) => p && typeof p === 'object' && Number.isFinite(p.x) && Number.isFinite(p.y);

// The saved state, checked (settings.json is the user's file): null when it can't be used, and then the chooser
// starts over. The order must be the installed desktops, each once; a step past 1 needs a pick.
export function cleanState(saved, available) {
  if (!saved || typeof saved !== 'object' || Array.isArray(saved)) return null;
  const want = DESKTOPS.filter((id) => available.includes(id));
  if (!Array.isArray(saved.order) || saved.order.length !== want.length) return null;
  const order = saved.order.filter((id) => want.includes(id));
  if (order.length !== want.length || new Set(order).size !== order.length) return null;
  const pick = typeof saved.pick === 'string' && want.includes(saved.pick) ? saved.pick : null;
  const step = saved.step === 2 || saved.step === 3 ? saved.step : 1;
  return { step: pick ? step : 1, pick, order, pos: isPos(saved.pos) ? { x: Math.round(saved.pos.x), y: Math.round(saved.pos.y) } : null };
}

// ---- moving between the steps ----

export const choose = (st, id) => (st.order.includes(id) ? { ...st, pick: id } : st);
// forward: 1 → 2 needs a pick; 3 is the last step (finishing is done(), not next)
export const next = (st) => (st.step === 1 ? (st.pick ? { ...st, step: 2 } : st) : st.step === 2 ? { ...st, step: 3 } : st);
export const back = (st) => (st.step > 1 ? { ...st, step: st.step - 1 } : st);
export const canNext = (st) => (st.step === 1 ? !!st.pick : true);

// The theme to switch to so the screen shows the pick: on steps 2 and 3 the chosen desktop is the real one. null when
// the current one is already it (or on step 1, where nothing is switched).
export function themeToApply(st, current) {
  if (!st || st.step < 2 || !st.pick) return null;
  return st.pick === current ? null : st.pick;
}

// ---- the floating box of step 2 ----

// Inside the work area (a Dock, a taskbar or the Workspaces spine is outside it), with a margin.
export function clampFloat(pos, area, size) {
  const m = Math.min(FLOAT_MARGIN, Math.max(0, (area.w - size.w) / 2), Math.max(0, (area.h - size.h) / 2));
  const x = Math.min(Math.max(pos.x, area.x + m), area.x + Math.max(m, area.w - size.w - m));
  const y = Math.min(Math.max(pos.y, area.y + m), area.y + Math.max(m, area.h - size.h - m));
  return { x: Math.round(x), y: Math.round(y) };
}
// Where it opens: the end of the work area, a little below the middle (out of the way of the windows that open in
// the middle); where the person left it, if they moved it.
export function floatPlace(saved, area, size) {
  const start = isPos(saved) ? saved : { x: area.x + area.w - size.w - area.w * 0.04, y: area.y + area.h * 0.55 - size.h / 2 };
  return clampFloat(start, area, size);
}
