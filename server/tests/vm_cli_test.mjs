// vm_cli_test.mjs — apps/machines/model.js cliPlan: what the Machines app does with a request of the `machines` command's
// session channel (server/src/vm/vmcli.h; apps/machines/cli.js). node server/tests/vm_cli_test.mjs
import { cliPlan, lordVmWhy, CLI } from '../../apps/machines/model.js';

let pass = 0, fail = 0;
function eq(name, got, want) {
  const g = JSON.stringify(got), w = JSON.stringify(want);
  if (g === w) pass++; else { fail++; console.log(`✗ ${name}: want ${w}, got ${g}`); }
}
const L = '/home/a/Machines';
const off = { dir: `${L}/Debian.lordvm`, name: 'Debian', state: 'stopped', error: '' };
const on = { dir: `${L}/Win.lordvm`, name: 'Win', state: 'running', error: '' };
const busy = { dir: `${L}/Busy.lordvm`, name: 'Busy', state: 'stopping', error: '' };
const crashed = { dir: `${L}/C.lordvm`, name: 'C', state: 'crashed', error: '' };
const broken = { dir: `${L}/Bad.lordvm`, name: 'Bad', state: 'stopped', error: 'invalid' };
const list = [off, on, busy, crashed, broken];
const plan = (verb, dir) => { const p = cliPlan({ verb, dir }, list); return { code: p.code, key: p.key, open: p.open?.name, detached: p.detached?.name }; };

// only this library's machines, by their folder exactly
eq('a folder not in the list', plan('start', '/home/b/Machines/Debian.lordvm'), { code: CLI.NO_MACHINE, key: 'notInLibrary' });
eq('a folder that differs in case', plan('start', `${L}/debian.lordvm`), { code: CLI.NO_MACHINE, key: 'notInLibrary' });
eq('a machine.json lord-vm refuses', plan('start', broken.dir), { code: CLI.NO_MACHINE, key: 'notInLibrary' });
eq('no list at all', cliPlan({ verb: 'open', dir: off.dir }, null).code, CLI.NO_MACHINE);
eq('no request', cliPlan(null, list).code, CLI.NO_MACHINE);

// start: Play's path (its window), or no window (startPrep + startDetached)
eq('start an off machine: its window', plan('start', off.dir), { code: CLI.OK, key: 'startingWindow', open: 'Debian' });
eq('start one that crashed', plan('start', crashed.dir), { code: CLI.OK, key: 'startingWindow', open: 'C' });
eq('start a running one: nothing', plan('start', on.dir), { code: CLI.OK, key: 'alreadyRunning' });
eq('start one shutting down', plan('start', busy.dir), { code: CLI.FAILED, key: 'busy' });
eq('start without a window', plan('start-detached', off.dir), { detached: 'Debian' });
eq('start without a window, running', plan('start-detached', on.dir), { code: CLI.OK, key: 'alreadyRunning' });
eq('start without a window, shutting down', plan('start-detached', busy.dir), { code: CLI.FAILED, key: 'busy' });

// open: only a machine that runs
eq('open a running one', plan('open', on.dir), { code: CLI.OK, key: 'opened', open: 'Win' });
eq('open an off one: never starts it', plan('open', off.dir), { code: CLI.NOT_RUNNING, key: 'notRunning' });
eq('open one shutting down', plan('open', busy.dir), { code: CLI.NOT_RUNNING, key: 'notRunning' });

// release: answered, nothing opened or started
eq('release', plan('release', on.dir), { code: CLI.OK, key: '' });
eq('a verb the channel never sends', plan('run', off.dir), { code: CLI.USAGE, key: '' });

eq('the name for the words', cliPlan({ verb: 'start', dir: off.dir }, list).vars, { name: 'Debian' });
eq('lord-vm\'s last word', lordVmWhy('starting\nlord-vm: first\nlord-vm: KVM is busy\n'), 'KVM is busy');
eq('else its output', lordVmWhy('  something odd \n'), 'something odd');
eq('empty', lordVmWhy(undefined), '');

console.log(`vm-cli-test: ${pass} passed, ${fail} failed`);
process.exit(fail ? 1 : 0);
