// alldisplays.js — All Displays (docs/VIRTUALIZATION.md 5.4): a machine across this computer's displays, one output of its
// virtio-gpu per display, each output's picture full screen on its display. Pure functions (tests/vm_alldisplays_test.mjs);
// the window side is alldisplays.jsx.
//
//   allDisplaysPlan(m, displays, { first, running, prot, appsOn, family }) →
//     { ok, why, n, order: [display…], sizes: [[w, h]…], restart }
//   ok false: why says why not (a key of machines.alld.why.*). restart: the machine has fewer outputs than displays and
//   must start again with more (its virtio-gpu's outputs are hardware the guest sees: set while it is off).

export const HEADS_MAX = 4;        // machine.h VM_HEADS_MAX
const SIDE_MIN = 320, SIDE_MAX = 8192;

const okDisplay = (d) => !!d && Number.isFinite(d.x) && Number.isFinite(d.y) && d.w > 0 && d.h > 0;

// The displays in the order the outputs take them: the machine's window's display first (output 0, the one with the
// machine's own window), then the others left to right, top to bottom; at most HEADS_MAX
export function displayOrder(displays, firstId) {
  const list = (displays || []).filter(okDisplay);
  const first = list.find((d) => d.id === firstId) || list.find((d) => d.main) || list[0];
  if (!first) return [];
  const rest = list.filter((d) => d !== first).sort((a, b) => a.x - b.x || a.y - b.y);
  return [first, ...rest].slice(0, HEADS_MAX);
}

// One output's size for a display: its own, kept within what the guest's display may be, even numbers
export function headSize(d) {
  const c = (v) => Math.max(SIDE_MIN, Math.min(SIDE_MAX, Math.round(v) & ~1));
  return [c(d.w), c(d.h)];
}

// The desktop the outputs make side by side never passes 32767 pixels (vmproto.h vm_heads_layout): the outputs that fit
export function fitSizes(sizes) {
  const out = [];
  let w = 0;
  for (const s of sizes) { if (w + s[0] > 32767) break; out.push(s); w += s[0]; }
  return out;
}

export function allDisplaysPlan(m, displays, { first, running = true, prot = false, appsOn = false, family = '' } = {}) {
  const no = (why) => ({ ok: false, why, n: 1, order: [], sizes: [], restart: false });
  if (!m) return no('noMachine');
  if (m.display && m.display !== 'window') return no('noWindow');
  if (m.gpu && m.gpu !== 'none') return no('vgpu');                    // an Intel vGPU's picture is one display
  if (prot || m.profileLock === 'anonymity') return no('anonymity');    // the guest would learn this computer's displays
  if (family === 'windows') return no('windows');                       // viogpudo drives one output
  if (appsOn) return no('apps');
  const order = displayOrder(displays, first);
  if (order.length < 2) return no('oneDisplay');
  const sizes = fitSizes(order.map(headSize));
  if (sizes.length < 2) return no('oneDisplay');
  const n = sizes.length;
  const heads = m.heads > 1 ? m.heads : 1;
  if (heads < n) return { ok: true, why: running ? 'restart' : '', n, order: order.slice(0, n), sizes, restart: true };
  if (!running) return { ok: false, why: 'notRunning', n, order: order.slice(0, n), sizes, restart: false };
  return { ok: true, why: '', n, order: order.slice(0, n), sizes, restart: false };
}

// The machine's outputs as its windows hold them: the instance of output k's window (k ≥ 1)
export const headInstance = (id, k) => `vmhead-${id}-${k}`;
export const headWindowId = (id, k) => `machines#${headInstance(id, k)}`;
// Output k's window from its args (index.jsx): { dir, k } with k 1..HEADS_MAX-1, else null
export function headArgs(a) {
  if (!a || typeof a.dir !== 'string' || !a.dir.startsWith('/')) return null;
  const k = Number(a.k);
  return Number.isInteger(k) && k >= 1 && k < HEADS_MAX ? { dir: a.dir, k } : null;
}
