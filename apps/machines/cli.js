// cli.js — the Machines app's side of the `machines` command's session channel (server/src/vm/vmcli.h,
// server/src/ui/js_vmcli.c; docs/VIRTUALIZATION.md "From the terminal"). `machines start NAME` and `machines open NAME`
// ask the person's running session, so a start from the terminal is the same as one from this app: its window (Play:
// begin(), with the bridged network and the vGPU first), or "Start Without Window" (startPrep, then
// lord.vm.startDetached, with lord.vm.list() read meanwhile so vm-memlock is asked for a vGPU machine). What a request
// does is cliPlan's (model.js): only a machine of this library, found by its folder. The answer is a code (the command's
// exit status) and words in the person's language.
import { t as tr, isAppOff } from '@lordos/sdk';
import { openMachine } from './library.jsx';
import { startPrep, vmRefusal } from './ui.jsx';
import { CLI, cliPlan, lordVmWhy } from './model.js';

const TICK_MS = 300;
const FOLLOW_MS = 500;
const words = (key, vars) => (key ? tr(`machinescli.${key}`, vars) : '');

// a start without a window: the admin steps, then lord-vm --start --detach, followed until it ends
function startDetached(id, m) {
  const answer = (code, text) => lord.vmcli.answer(id, code, text);
  const failed = (why) => answer(CLI.FAILED, words('failed', { name: m.name, why }));
  startPrep(m).then(() => {
    const job = lord.vm.startDetached(m.dir);
    if (!job) { failed(vmRefusal(tr('machines.cfg.refused'))); return; }
    const timer = setInterval(() => {
      try { lord.vm.list(); } catch (e) { /* the list only drives vm-memlock and the clean-up here */ }
      const r = lord.vm.job(job);
      if (r && !r.done) return;
      clearInterval(timer);
      if (r?.ok) answer(CLI.OK, words('startedNoWindow', { name: m.name }));
      else failed(lordVmWhy(r?.out));
    }, FOLLOW_MS);
  }, (e) => failed(String(e?.message || e)));
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
        const plan = req.verb !== 'release' && isAppOff('machines') ? { code: CLI.FAILED, key: 'appOff', vars: {} } : cliPlan(req, list);
        if (plan.detached) { startDetached(req.id, plan.detached); continue; }
        if (plan.open) openMachine(plan.open);
        lord.vmcli.answer(req.id, plan.code, words(plan.key, plan.vars));
      } catch (e) {
        lord.vmcli.answer(req.id, CLI.FAILED, String(e?.message || e));
      }
    }
  }, TICK_MS);
}
