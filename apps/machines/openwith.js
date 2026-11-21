// openwith.js — a machine in Files' "Open with" (docs/VIRTUALIZATION.md 7.3), the pure part; index.jsx registers the
// provider (sdk/openwith.js) and vmwindow.jsx does the work. Tested by server/tests/vm_openwith_test.mjs.
//
// A machine is listed once its guest tools said they open files (the "open" capability, AGENT_CAPS.open): the machine's
// window writes that into app.json (opens: 'windows' | 'other' | false), so an off machine is still listed. Choosing
// it opens the machine's window, which starts or resumes the machine, waits for the tools, copies the file in as a drop
// does (the transfer folder when the guest has it, else the agent's way, SHA-256 checked) and asks the tools to open
// it; then apps mode, when the guest has it, so the program comes up as its own window.
import { AGENT_CAPS, stateView, unreadableOf, guestWindows } from './model.js';

// what a connected guest's tools say of opening files: 'windows' / 'other' when they can, false when they can't,
// undefined when no helper spoke yet (nothing to write)
const HELPER = AGENT_CAPS.clipboard | AGENT_CAPS.clipNoTool | AGENT_CAPS.clipLazy | AGENT_CAPS.display | AGENT_CAPS.windows | AGENT_CAPS.open;
export function opensOf(agent) {
  if (agent?.state !== 'connected' || !(agent.caps & HELPER)) return undefined;
  if (!(agent.caps & AGENT_CAPS.open)) return false;
  return guestWindows(agent) ? 'windows' : 'other';
}

// the machines to list: readable, with tools that open files; [{ dir, id, name, opens, active }] in the list's order
export function openMachines(list, metaOf) {
  const out = [];
  for (const m of list || []) {
    if (!m || !m.dir || unreadableOf(m)) continue;
    let meta = {};
    try { meta = metaOf(m.dir) || {}; } catch (e) { meta = {}; }
    if (meta.opens !== 'windows' && meta.opens !== 'other') continue;
    out.push({ dir: m.dir, id: m.id || m.folder || m.dir, name: m.name || '', opens: meta.opens, active: stateView(m.state, m.reason).active });
  }
  return out;
}

// what a window with files to open does now (st: lord.vm.poll's state): 'start' (no session), 'resume' (paused),
// 'wait' (starting, or the tools aren't there yet), 'send' (copy and open), 'cant' (the tools connected but can't open)
export function openStep(sid, st) {
  if (!sid || !st) return 'start';
  if (st.ended) return 'start';
  if (st.state === 'paused') return 'resume';
  if (st.state !== 'running') return 'wait';
  const v = opensOf(st.agent);
  if (v === undefined) return 'wait';
  return v ? 'send' : 'cant';
}

// files asked for a machine (dir → [{ id, path, at }]): one more, each path once, at most OPEN_QUEUE kept
export const OPEN_QUEUE = 8;
export function withAsk(asks, dir, path, id, at) {
  const cur = (asks && asks[dir]) || [];
  if (cur.some((a) => a.path === path)) return asks;
  return { ...asks, [dir]: [...cur, { id, path, at }].slice(-OPEN_QUEUE) };
}
export function withoutAsks(asks, dir) {
  if (!asks || !asks[dir]) return asks;
  const out = { ...asks };
  delete out[dir];
  return out;
}
// how long a window waits for the tools after the machine runs before it says they didn't come (sign-in included)
export const OPEN_WAIT_MS = 10 * 60 * 1000;
export const askStale = (a, now) => !!a && now - a.at > OPEN_WAIT_MS;
