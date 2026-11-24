// vm_sound_e2e.js — the theme tests/vm_sound_e2e.sh loads into a headless lord-server (docs/VIRTUALIZATION.md 6.9, 6.20):
// a machine whose guest plays a 440 Hz tone and records its microphone all the time, through lord-server's own window-less
// path: the picture (lord-vmdisplay), the speakers' stream, the microphone with LordOS's permission refused and then
// given, a screenshot and a recording with sound. Prints "SNDE2E ok|FAIL <check>", marks for the script, "SNDE2E DONE".
const out = (s) => console.log(`SNDE2E ${s}`);
const check = (name, ok, detail = '') => out(ok ? `ok ${name}` : `FAIL ${name} ${detail}`);
const cfg = globalThis.SND_E2E;
const wait = (ms) => new Promise((r) => setTimeout(r, ms));
async function until(f, ms, step = 250) { const end = Date.now() + ms; while (Date.now() < end) { const v = f(); if (v) return v; await wait(step); } return f(); }

async function main() {
  lord.perms.set('machines', 'microphone', 'deny');
  const sid = lord.vm.start(cfg.dir);
  check('the machine starts', sid > 0);
  const up = await until(() => { const s = lord.vm.poll(sid); return s?.state === 'running' && s.display === 'up' && s.w > 0; }, 90000);
  check('it runs and its picture is up', !!up, JSON.stringify(lord.vm.poll(sid)?.display));
  const playing = await until(() => lord.vm.poll(sid).sound?.playing, 90000, 500);
  check('the guest plays (its tone service)', !!playing, JSON.stringify(lord.vm.poll(sid).sound));
  const st = lord.vm.poll(sid);
  check('the speakers: on, at the machine\'s volume (0 here: nobody hears the test)', st.sound.on === true && st.sound.volume === 0, JSON.stringify(st.sound));
  const rec = await until(() => lord.vm.poll(sid).sound?.recording, 30000, 500);
  check('the guest records (its microphone service)', !!rec, JSON.stringify(lord.vm.poll(sid).sound));
  const waits = await until(() => lord.vm.poll(sid).sound?.mic === 'wait', 10000, 250);
  check('without LordOS\'s permission the microphone stays closed (wait)', !!waits, JSON.stringify(lord.vm.poll(sid).sound));
  out('MARK speakers');
  await wait(8000);                                              // the script looks at PipeWire's nodes meanwhile
  lord.perms.set('machines', 'microphone', 'allow');
  const live = await until(() => lord.vm.poll(sid).sound?.mic === 'live', 10000, 250);
  check('with it the microphone opens (live)', !!live, JSON.stringify(lord.vm.poll(sid).sound));
  out('MARK mic');
  await wait(12000);                                             // a few of the guest's 2 s recordings with the real microphone
  lord.perms.set('machines', 'microphone', 'deny');
  const closed = await until(() => lord.vm.poll(sid).sound?.mic !== 'live', 10000, 250);
  check('the permission taken away closes it again', !!closed, JSON.stringify(lord.vm.poll(sid).sound));
  // a screenshot: the guest's picture at its size, a PNG in ~/Pictures/Screenshots
  const shot = lord.vm.screenshot(sid);
  check('a screenshot', typeof shot === 'string' && /\.png$/.test(shot), String(shot));
  out(`SHOT ${shot} ${lord.vm.poll(sid).w}x${lord.vm.poll(sid).h}`);
  // a recording with sound, 8 s
  const id = lord.vm.record(sid);
  check('a recording starts', id > 0, String(id));
  await wait(8000);
  lord.media.stop(id);
  await wait(3000);
  out('MARK recorded');
  lord.vm.cmd(sid, 'poweroff');
  await until(() => ['stopped', 'crashed'].includes(lord.vm.poll(sid)?.state), 30000);
  out('DONE');
}
if (!cfg) out('FAIL config'); else main().catch((e) => out(`FAIL exception ${e && e.stack}`));
