// vm_usb_e2e.js — the theme tests/vm_usb_e2e.sh loads into a lord-server started as root (so its admin helper is real):
// a host USB device into a running machine through lord.vm (docs/VIRTUALIZATION.md 6.6). Prints "USBE2E ok <check>" or
// "USBE2E FAIL <check> <detail>", and "USBE2E DONE" at the end. vm_usb_e2e.sh puts its settings first in the copy it loads:
// globalThis.USB_E2E = { dir, bus, dev, vendor, product, hub: { bus, dev, vendor, product } }.
const out = (s) => console.log(`USBE2E ${s}`);
const check = (name, ok, detail = '') => out(ok ? `ok ${name}` : `FAIL ${name} ${detail}`);
const cfg = globalThis.USB_E2E;
const adminSaid = {};
globalThis.__lordAdmin = (id, kind, text) => { if (kind !== 'LOG') adminSaid[id] = `${kind} ${text}`; };
const wait = (ms) => new Promise((r) => setTimeout(r, ms));
async function until(f, ms, step = 250) { const end = Date.now() + ms; while (Date.now() < end) { const v = f(); if (v) return v; await wait(step); } return f(); }

async function main() {
  const { dir, bus, dev, vendor, product, hub } = cfg;
  const sid = lord.vm.start(dir);
  check('the machine starts', sid > 0, `sid ${sid}`);
  const run = await until(() => lord.vm.poll(sid)?.state === 'running', 60000);
  check('it runs', run, JSON.stringify(lord.vm.poll(sid)?.state));
  await wait(25000);                                            // the guest's kernel up, its xHCI driver ready
  let st = lord.vm.poll(sid);
  check('the controller is there', st.usb?.controller === true && st.usb?.locked === false, JSON.stringify(st.usb));
  const list = lord.vm.usbDevices();
  const it = list.find((d) => d.bus === bus && d.dev === dev);
  check('usbDevices lists the device', !!it && it.vendor === vendor && it.product === product, JSON.stringify(it));
  check('a camera is said to be one', !!it?.camera, JSON.stringify(it));
  const h = list.find((d) => d.bus === hub.bus && d.dev === hub.dev);
  check('a hub is listed with why', h?.why === 'vm.usbHub', JSON.stringify(h));
  check('lord-server refuses the hub itself', lord.vm.usbAttach(sid, hub.bus, hub.dev) === 'vm.usbHub');
  // root refuses it too, asked directly (lord.admin is open to JS: the helper checks everything again)
  const id = lord.vm.list().find((m) => m.dir === dir)?.id;
  const r1 = lord.admin.request('vm-usb-allow', `${id} ${hub.bus} ${hub.dev} ${hub.vendor}:${hub.product}`);
  await until(() => adminSaid[r1], 5000);
  check('the admin helper refuses the hub', /^ERR vm\.usbHub/.test(adminSaid[r1] || ''), adminSaid[r1]);
  const r2 = lord.admin.request('vm-usb-allow', `${id} ${bus} ${dev} ffff:ffff`);
  await until(() => adminSaid[r2], 5000);
  check('the admin helper refuses another vendor and product at that address', /^ERR vm\.usbGone/.test(adminSaid[r2] || ''), adminSaid[r2]);
  const r3 = lord.admin.request('vm-usb-allow', `${id} ${bus} ${dev} ${vendor}:${product} x`);
  await until(() => adminSaid[r3], 5000);
  check('the admin helper refuses a malformed request', /^ERR admin\.invalidArgument/.test(adminSaid[r3] || ''), adminSaid[r3]);
  // a request JS made itself: the answer reaches JS, its descriptor never does (lord-server closes it: the shell checks)
  const r4 = lord.admin.request('vm-usb-allow', `${id} ${bus} ${dev} ${vendor}:${product}`);
  await until(() => adminSaid[r4], 5000);
  check("JS's own request is answered, without the device", /^OK vm\.usbReady/.test(adminSaid[r4] || ''), adminSaid[r4]);
  out('MARK js-request-done');
  await wait(1500);
  // no permission yet: the camera waits for it
  lord.perms.set('machines', 'camera', 'ask');
  lord.perms.set('machines', 'microphone', 'ask');
  check('a camera without the permission is refused', lord.vm.usbAttach(sid, bus, dev) === 'vm.usbCameraPerm');
  lord.perms.set('machines', 'camera', 'allow');
  const why = lord.vm.usbAttach(sid, bus, dev);
  check('a camera with a microphone needs that permission too', why === 'vm.usbMicPerm', why);
  lord.perms.set('machines', 'microphone', 'allow');
  check('with both, it is asked for', lord.vm.usbAttach(sid, bus, dev) === '');
  const inIt = await until(() => (lord.vm.poll(sid).usb?.devices || []).some((d) => d.bus === bus && d.dev === dev), 10000);
  st = lord.vm.poll(sid);
  check('the machine holds it', !!inIt, JSON.stringify(st.usb));
  check('the poll says it is a camera', (st.usb.devices || []).some((d) => d.camera && d.audio), JSON.stringify(st.usb.devices));
  check('another machine sees it held', lord.vm.usbDevices().find((d) => d.bus === bus && d.dev === dev)?.holder === sid);
  out('MARK in');
  await wait(10000);                                            // vm_usb_e2e.sh looks at who holds the node meanwhile
  // the rule, while it runs (lord-vm writes machine.json)
  check('usbAuto adds a rule', lord.vm.usbAuto(sid, `${vendor}:${product}`, true));
  await until(() => (lord.vm.list().find((m) => m.dir === dir)?.usbAuto || []).length === 1, 5000);
  check('machine.json has it', (lord.vm.list().find((m) => m.dir === dir)?.usbAuto || [])[0] === `${vendor}:${product}`);
  check('usbDetach', lord.vm.usbDetach(sid, bus, dev));
  const outOf = await until(() => !(lord.vm.poll(sid).usb?.devices || []).length, 10000);
  check('out of the machine', outOf, JSON.stringify(lord.vm.poll(sid).usb));
  await wait(3000);
  check('a detached device is not brought back by its rule while it stays plugged in', !(lord.vm.poll(sid).usb?.devices || []).length);
  out('MARK out');
  // off and on again: the rule brings it in by itself
  lord.vm.cmd(sid, 'poweroff');
  await until(() => ['stopped', 'crashed'].includes(lord.vm.poll(sid)?.state), 30000);
  const sid2 = lord.vm.start(dir);
  await until(() => lord.vm.poll(sid2)?.state === 'running', 60000);
  const auto = await until(() => (lord.vm.poll(sid2).usb?.devices || []).some((d) => d.bus === bus && d.dev === dev), 30000, 500);
  check('the rule connects it at the next start', !!auto, JSON.stringify(lord.vm.poll(sid2).usb));
  out('MARK auto-in');
  await wait(3000);
  // the camera permission taken away: the device leaves the machine
  lord.perms.set('machines', 'camera', 'deny');
  const gone = await until(() => !(lord.vm.poll(sid2).usb?.devices || []).length, 10000, 500);
  check('a permission taken away takes the camera out', gone, JSON.stringify(lord.vm.poll(sid2).usb));
  await wait(3000);
  check('and its rule doesn\'t bring it back', !(lord.vm.poll(sid2).usb?.devices || []).length, JSON.stringify(lord.vm.poll(sid2).usb));
  check('usbAuto removes the rule', lord.vm.usbAuto(sid2, `${vendor}:${product}`, false));
  await wait(1000);
  lord.vm.cmd(sid2, 'poweroff');
  await until(() => ['stopped', 'crashed'].includes(lord.vm.poll(sid2)?.state), 30000);
  check('machine.json has no rule left', !(lord.vm.list().find((m) => m.dir === dir)?.usbAuto || []).length);
  out('DONE');
}
if (!cfg) out('FAIL config no USB_E2E'); else main().catch((e) => out(`FAIL exception ${e && e.stack}`));
