// cli.js — the Machines app's side of the `machines` command's session channel (server/src/vm/vmcli.h,
// server/src/ui/js_vmcli.c; docs/VIRTUALIZATION.md "From the terminal"). `machines start NAME` and `machines open NAME`
// ask the person's running session, so a start from the terminal is the same as one from this app: its window (Play:
// begin(), with the bridged network and the vGPU first), or "Start Without Window" (startPrep, then
// lord.vm.startDetached, with lord.vm.list() read meanwhile so vm-memlock is asked for a vGPU machine). What a request
// does is cliPlan's (model.js): only a machine of this library, found by its folder. The answer is a code (the command's
// exit status) and words in the person's language.
//
// A machine group (docs/VIRTUALIZATION.md 19): a request may name a group's folder (<name>.lordgroup); groupCliPlan says
// what it does, and groupPlan starts the members in order (the gateway first, the workstation once the gateway runs).
// The group's internal link (lord.vm.link: a socketpair lord-server hands both members) is made whenever both members
// run under lord-vms it hasn't linked yet: after a start from here, from the app or from the terminal, and after either
// member started anew (its old pair died with its QEMU).
import { t as tr, isAppOff } from '@lordos/sdk';
import { openMachine } from './library.jsx';
import { startPrep, vmRefusal } from './ui.jsx';
import { CLI, cliPlan, lordVmWhy, groupCliPlan, groupPlan, groupLinkKey, groupStartFailed } from './model.js';

const TICK_MS = 300;
const FOLLOW_MS = 500;
const LINK_MS = 2000;
const GROUP_START_MS = 10 * 60000;
const words = (key, vars) => (key ? tr(`machinescli.${key}`, vars) : '');

// a start without a window: the admin steps, then lord-vm --start --detach, followed until it ends; done(ok, why)
function detachedStart(m, done) {
  startPrep(m).then(() => {
    const job = lord.vm.startDetached(m.dir);
    if (!job) { done(false, vmRefusal(tr('machines.cfg.refused'))); return; }
    const timer = setInterval(() => {
      try { lord.vm.list(); } catch (e) { /* the list only drives vm-memlock and the clean-up here */ }
      const r = lord.vm.job(job);
      if (r && !r.done) return;
      clearInterval(timer);
      done(!!r?.ok, r?.ok ? '' : lordVmWhy(r?.out));
    }, FOLLOW_MS);
  }, (e) => done(false, String(e?.message || e)));
}

function startDetached(id, m) {
  detachedStart(m, (ok, why) => lord.vmcli.answer(id, ok ? CLI.OK : CLI.FAILED, ok ? words('startedNoWindow', { name: m.name }) : words('failed', { name: m.name, why })));
}

// the group's link, made once for each pair of running lord-vms (groupLinkKey)
const linked = new Map();
function linkGroups(list, groups) {
  for (const g of groups || []) {
    if (!g || g.error) continue;
    const key = groupLinkKey(g, list);
    if (!key) { linked.delete(g.dir); continue; }
    if (linked.get(g.dir) === key) continue;
    let why = 'failed';
    try { why = lord.vm.link(g.dir, true); } catch (e) { why = String(e?.message || e); }
    if (why === '') linked.set(g.dir, key);
  }
}

// a group's start (groupPlan, 19.9): each member in order, detached, or the primary one in its window when asked
function startGroup(id, g, window) {
  const answer = (code, key, vars) => lord.vmcli.answer(id, code, words(key, { name: g.name, ...vars }));
  const until = Date.now() + GROUP_START_MS;
  const asked = {};          // role → the start was asked and is under way (true) or ended (false: failed, why in failed)
  const failed = {};
  const tick = () => {
    let list = [];
    try { list = lord.vm.list() || []; } catch (e) { list = []; }
    const step = groupPlan(g, list, 'start');
    if (step.fail) { answer(CLI.FAILED, 'groupDamaged'); return; }
    if (step.done) {
      let groups = [];
      try { groups = lord.vm.groups() || []; } catch (e) { groups = []; }
      linkGroups(list, groups);
      answer(CLI.OK, 'groupStarted');
      return;
    }
    if (Date.now() > until) { answer(CLI.FAILED, 'failed', { why: '' }); return; }
    if (step.member && asked[step.role] === false) {
      answer(CLI.FAILED, groupStartFailed(step.role), { why: failed[step.role] || '' });
      return;
    }
    if (step.member && !asked[step.role]) {
      asked[step.role] = true;
      const m = step.member;
      if (window && m.primary) {
        openMachine(m);          // Play's path: its window starts it
      } else {
        detachedStart(m, (ok, why) => { if (!ok) { asked[step.role] = false; failed[step.role] = why; } });
      }
    }
    setTimeout(tick, FOLLOW_MS);
  };
  tick();
}

let started = false;
export function startMachinesCli() {
  if (started || typeof lord === 'undefined' || !lord.vmcli || !lord.vm || !lord.vmcli.active()) return;
  started = true;
  setInterval(() => {
    let reqs = [];
    try { reqs = lord.vmcli.poll() || []; } catch (e) { reqs = []; }
    if (!reqs.length) return;
    let list = [];
    try { list = lord.vm.list() || []; } catch (e) { list = []; }
    for (const req of reqs) {
      try {
        // Machines deactivated (Store → Installed) opens and starts nothing, as from the Dock; a release still cleans up
        if (req.verb !== 'release' && isAppOff('machines')) { lord.vmcli.answer(req.id, CLI.FAILED, words('appOff', {})); continue; }
        if (typeof req.dir === 'string' && req.dir.endsWith('.lordgroup')) {
          let groups = [];
          try { groups = lord.vm.groups() || []; } catch (e) { groups = []; }
          const gp = groupCliPlan(req, groups, list);
          if (gp.group) { startGroup(req.id, gp.group, gp.window); continue; }
          if (gp.open) openMachine(gp.open);
          lord.vmcli.answer(req.id, gp.code, words(gp.key, gp.vars));
          continue;
        }
        const plan = cliPlan(req, list);
        if (plan.detached) { startDetached(req.id, plan.detached); continue; }
        if (plan.open) openMachine(plan.open);
        lord.vmcli.answer(req.id, plan.code, words(plan.key, plan.vars));
      } catch (e) {
        lord.vmcli.answer(req.id, CLI.FAILED, String(e?.message || e));
      }
    }
  }, TICK_MS);
  // groups whose members were started from anywhere get their link
  setInterval(() => {
    try {
      const groups = lord.vm.groups ? lord.vm.groups() : [];
      if (groups.length) linkGroups(lord.vm.list() || [], groups);
    } catch (e) { /* the next turn tries again */ }
  }, LINK_MS);
}
