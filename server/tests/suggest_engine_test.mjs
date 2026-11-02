// suggest_engine_test.mjs — the Store's "Suggested for your computer" rules
// (assets/store/suggestions.json + sdk/suggestengine.js) against fake hardware facts (the shape
// lord.hwinfo.scan() returns, hw/hwscan.c) and a fake package index, one fixture per rule; and the
// texts every rule needs in en/es/pt.
//
//   node tests/suggest_engine_test.mjs        (from server/; run_suites.sh runs it when node exists)
import fs from 'node:fs';
import path from 'node:path';
import { fileURLToPath } from 'node:url';

const here = path.dirname(fileURLToPath(import.meta.url));
const repo = path.join(here, '../..');
const { evaluate, matches, firmwareFromLog, fresh } = await import(path.join(repo, 'sdk/suggestengine.js'));
const DATA = JSON.parse(fs.readFileSync(path.join(repo, 'assets/store/suggestions.json'), 'utf8'));

let n = 0, bad = 0;
const eq = (name, got, want) => {
  n++;
  const g = JSON.stringify(got), w = JSON.stringify(want);
  if (g !== w) { bad++; console.log(`✗ ${name}: want ${w}, got ${g}`); }
};

// ---- fake facts ----
const base = () => ({ fixture: true, arch: 'x86_64', dmi: { sysVendor: 'Example Computer Co.', productName: 'Desktop 3000', boardVendor: '', boardName: '', chassisType: '3' },
  model: '', compatible: [], pci: [{ vendor: '8086', device: '29c0', class: '060000', driver: '' }], usb: [], virtio: [], firmware: { readable: true, missing: [] } });
const facts = (patch) => ({ ...base(), ...patch });

// ---- fake index: name → { installed } ; absent = not in the configured sources ----
const MAIN = ['open-vm-tools', 'qemu-guest-agent', 'linux-headers-amd64', 'hplip', 'libsane-hpaio', 'printer-driver-escpr', 'printer-driver-brlaser',
  'printer-driver-splix', 'printer-driver-gutenprint', 'ipp-usb', 'gpiod'];
const NONFREE = ['nvidia-driver', 'firmware-misc-nonfree', 'firmware-realtek', 'firmware-iwlwifi', 'firmware-atheros', 'firmware-brcm80211',
  'firmware-mediatek', 'firmware-amd-graphics', 'firmware-intel-graphics', 'bluez-firmware', 'firmware-libertas', 'firmware-nvidia-graphics'];
const RPI = ['rpi-eeprom', 'raspi-utils'];
const index = ({ nonfree = true, rpi = false, installed = [] } = {}) => {
  const names = new Set([...MAIN, ...(nonfree ? NONFREE : []), ...(rpi ? RPI : [])]);
  return (name) => (names.has(name) ? { name, installed: installed.includes(name) ? '1.0' : null } : null);
};
const run = (f, opts = {}, ev = {}) => evaluate(DATA, f, { info: index(opts), ...ev });
const ids = (list) => list.map((s) => `${s.id}=${s.state}`);
const one = (list, id) => list.find((s) => s.id === id);

// ---- nothing special: no suggestion ----
eq('plain PC', ids(run(facts({}))), []);
eq('no facts', evaluate(DATA, null, {}), []);

// ---- VMware: open-vm-tools (preinstalled → installed) ----
const vmware = facts({ dmi: { ...base().dmi, sysVendor: 'VMware, Inc.', productName: 'VMware Virtual Platform' }, pci: [{ vendor: '15ad', device: '0405', class: '030000', driver: 'vmwgfx' }] });
eq('vmware installed', ids(run(vmware, { installed: ['open-vm-tools'] })), ['vmwareTools=installed']);
eq('vmware not installed', ids(run(vmware)), ['vmwareTools=install']);
eq('vmware install list', one(run(vmware), 'vmwareTools').install, ['open-vm-tools']);
eq('vmware by PCI only', ids(run(facts({ pci: [{ vendor: '15ad', device: '0740', class: '088000', driver: 'vmw_vmci' }] }))), ['vmwareTools=install']);
eq('vmware: never the X11 desktop package', one(run(vmware), 'vmwareTools').packages.includes('open-vm-tools-desktop'), false);

// ---- QEMU with a guest agent channel ----
const qemu = facts({ dmi: { ...base().dmi, sysVendor: 'QEMU', productName: 'Standard PC (Q35 + ICH9, 2009)' }, virtio: ['org.qemu.guest_agent.0', 'com.redhat.spice.0'] });
eq('qemu agent installed', ids(run(qemu, { installed: ['qemu-guest-agent'] })), ['qemuAgent=installed']);
eq('qemu agent missing', ids(run(qemu)), ['qemuAgent=install']);
eq('qemu without the channel', ids(run(facts({ dmi: qemu.dmi }))), []);

// ---- VirtualBox: the guest utilities are not in Debian 13 → explained ----
const vbox = facts({ dmi: { ...base().dmi, sysVendor: 'innotek GmbH', productName: 'VirtualBox' } });
eq('virtualbox unavailable', ids(run(vbox)), ['virtualboxGuest=unavailable']);
eq('virtualbox missing names it', one(run(vbox), 'virtualboxGuest').missing, ['virtualbox-guest-utils']);
eq('virtualbox by PCI 80ee:cafe', ids(run(facts({ pci: [{ vendor: '80ee', device: 'cafe', class: '088000', driver: 'vboxguest' }] }))), ['virtualboxGuest=unavailable']);
eq('another 80ee device is not the guest device', ids(run(facts({ pci: [{ vendor: '80ee', device: 'beef', class: '030000', driver: '' }] }))), []);

// ---- NVIDIA: non-free driver, never free, explained without non-free ----
const nv = facts({ pci: [...base().pci, { vendor: '10de', device: '2484', class: '030000', driver: 'nouveau' }] });
const s = one(run(nv), 'nvidiaDriver');
eq('nvidia install', s.state, 'install');
eq('nvidia packages', s.install, ['linux-headers-amd64', 'nvidia-driver', 'firmware-misc-nonfree']);
eq('nvidia not free', [s.free, s.component], [false, 'non-free']);
eq('nvidia notes', s.notes, ['nonfree', 'nouveau', 'secureBoot', 'older']);
eq('nvidia without non-free sources', ids(run(nv, { nonfree: false })), ['nvidiaDriver=unavailable']);
eq('nvidia already on the vendor driver', ids(run(nv, { installed: ['nvidia-driver'] })), ['nvidiaDriver=installed']);
eq('nvidia audio function only (class 0403)', ids(run(facts({ pci: [{ vendor: '10de', device: '10fa', class: '040300', driver: 'snd_hda_intel' }] }))), []);
eq('nvidia 3D controller (0302)', ids(run(facts({ pci: [{ vendor: '10de', device: '1eb8', class: '030200', driver: '' }] }))), ['nvidiaDriver=install']);
eq('nvidia on arm64: nothing', ids(run({ ...nv, arch: 'aarch64' })), []);
eq('nvidia: plan impossible (contrib missing) → explained', ids(run(nv, {}, { check: (names) => !names.includes('nvidia-driver') })), ['nvidiaDriver=unavailable']);
eq('nvidia: plan impossible names what it would install', one(run(nv, {}, { check: () => false }), 'nvidiaDriver').missing, ['linux-headers-amd64', 'nvidia-driver', 'firmware-misc-nonfree']);
eq('check passes → installable', ids(run(nv, {}, { check: () => true })), ['nvidiaDriver=install']);
eq('check is not asked for installed cards', ids(run(vmware, { installed: ['open-vm-tools'] }, { check: () => false })), ['vmwareTools=installed']);
eq('nvidia dismissed', ids(run(nv, {}, { dismissed: ['nvidiaDriver'] })), []);

// ---- Raspberry Pi 5 ----
const pi = facts({ arch: 'aarch64', dmi: { sysVendor: '', productName: '', boardVendor: '', boardName: '', chassisType: '' }, model: 'Raspberry Pi 5 Model B Rev 1.1',
  compatible: ['raspberrypi,5-model-b', 'brcm,bcm2712'], pci: [{ vendor: '1de4', device: '0001', class: '020000', driver: 'rp1' }] });
eq('pi5 with the Raspberry Pi archive, all installed', ids(run(pi, { rpi: true, installed: ['rpi-eeprom', 'raspi-utils', 'gpiod'] })), ['rpi5Tools=installed']);
eq('pi5 with the archive, nothing installed', one(run(pi, { rpi: true }), 'rpi5Tools').install, ['rpi-eeprom', 'raspi-utils', 'gpiod']);
eq('pi5 without the archive: only what Debian has', one(run(pi), 'rpi5Tools').install, ['gpiod']);
eq('pi4 is not a pi5', ids(run({ ...pi, compatible: ['raspberrypi,4-model-b', 'brcm,bcm2711'] })), []);

// ---- USB printers and scanners ----
const hp = facts({ usb: [{ vendor: '03f0', product: '2b17', class: '00', ifaces: ['070102', '070104'], driver: 'usblp', sane: false }] });
eq('hp printer + ipp-usb', ids(run(hp)), ['printer:03f0=install', 'ippUsb=install']);
eq('hp printer vendor param', one(run(hp), 'printer:03f0').params, { vendor: 'HP' });
eq('hp printer installed', ids(run(hp, { installed: ['hplip', 'ipp-usb'] })), ['printer:03f0=installed', 'ippUsb=installed']);
const epsonScan = facts({ usb: [{ vendor: '04b8', product: '0142', class: '00', ifaces: ['ffffff'], driver: '', sane: true }] });
eq('epson scanner: no Debian scanner driver to add', ids(run(epsonScan)), []);
const hpScan = facts({ usb: [{ vendor: '03f0', product: '5817', class: '00', ifaces: ['ffffff'], driver: '', sane: true }] });
eq('hp scanner', ids(run(hpScan)), ['scanner:03f0=install']);
eq('epson printer', one(run(facts({ usb: [{ vendor: '04b8', product: '1129', class: '00', ifaces: ['070102'], driver: 'usblp', sane: false }] })), 'printer:04b8').install, ['printer-driver-escpr']);
eq('unknown vendor printer: only ipp-usb', ids(run(facts({ usb: [{ vendor: '1234', product: '0001', class: '00', ifaces: ['070104'], driver: '', sane: false }] }))), ['ippUsb=install']);
eq('two HP printers: one suggestion', ids(run(facts({ usb: [hp.usb[0], { ...hp.usb[0], product: '1234' }] }))), ['printer:03f0=install', 'ippUsb=install']);
eq('a keyboard (class 03): nothing', ids(run(facts({ usb: [{ vendor: '046d', product: 'c31c', class: '00', ifaces: ['030101'], driver: 'usbhid', sane: false }] }))), []);
eq('bad vendor id ignored', ids(run(facts({ usb: [{ vendor: '03F0x', product: '1', class: '00', ifaces: ['070102'], driver: '', sane: false }] }))), []);

// ---- missing firmware ----
const fw = (missing) => facts({ firmware: { readable: true, missing } });
const wifi = run(fw([{ file: 'iwlwifi-so-a0-gf-a0-89.ucode', driver: 'iwlwifi' }, { file: 'iwlwifi-so-a0-gf-a0-88.ucode', driver: 'iwlwifi' }]));
eq('iwlwifi: one suggestion for the package', ids(wifi), ['firmware:firmware-iwlwifi=install']);
eq('iwlwifi params', one(wifi, 'firmware:firmware-iwlwifi').params, { package: 'firmware-iwlwifi', driver: 'iwlwifi', files: 'iwlwifi-so-a0-gf-a0-89.ucode, iwlwifi-so-a0-gf-a0-88.ucode', count: 2 });
eq('iwlwifi text', one(wifi, 'firmware:firmware-iwlwifi').text, 'firmware.wifi');
eq('firmware is not free', one(wifi, 'firmware:firmware-iwlwifi').free, false);
const mixed = run(fw([{ file: 'rtl_bt/rtl8761bu_fw.bin', driver: 'bluetooth' }, { file: 'brcm/brcmfmac4360-pcie.bin', driver: 'brcmfmac' },
  { file: 'mediatek/WIFI_RAM_CODE_MT7922_1.bin', driver: 'mt7921e' }, { file: 'amdgpu/navi10_pfp.bin', driver: '' }, { file: 'unknown/thing.bin', driver: 'x' },
  { file: 'intel/ibt-0040-0041.sfi', driver: 'bluetooth' }, { file: 'brcm/BCM4345C0.hcd', driver: 'bluetooth' }, { file: 'htc_9271.fw', driver: 'ath9k_htc' }]));
eq('firmware mapping', ids(mixed), ['firmware:firmware-realtek=install', 'firmware:firmware-brcm80211=install', 'firmware:firmware-mediatek=install',
  'firmware:firmware-amd-graphics=install', 'firmware:firmware-iwlwifi=install', 'firmware:bluez-firmware=install', 'firmware:firmware-atheros=install']);
eq('firmware kinds', mixed.map((x) => x.text), ['firmware.bluetooth', 'firmware.wifi', 'firmware.wifi', 'firmware.graphics', 'firmware.bluetooth', 'firmware.bluetooth', 'firmware.wifi']);
eq('firmware package installed but file missing: nothing to offer', ids(run(fw([{ file: 'rtw88/rtw8822c_fw.bin', driver: 'rtw88_8822ce' }]), { installed: ['firmware-realtek'] })), []);
eq('firmware without non-free-firmware sources', ids(run(fw([{ file: 'rtw88/rtw8822c_fw.bin', driver: '' }]), { nonfree: false })), ['firmware:firmware-realtek=unavailable']);
eq('firmware component', one(run(fw([{ file: 'rtw88/rtw8822c_fw.bin', driver: '' }]), { nonfree: false }), 'firmware:firmware-realtek').component, 'non-free-firmware');
eq('unreadable log: nothing', ids(run(facts({ firmware: { readable: false, missing: [] } }))), []);

eq('firmware plan impossible → explained', ids(run(fw([{ file: 'rtw88/rtw8822c_fw.bin', driver: '' }]), {}, { check: () => false })), ['firmware:firmware-realtek=unavailable']);
eq('printer plan impossible → hidden (not explained)', ids(run(hp, {}, { check: () => false })), []);

// ---- ordering: installable first, then explained, then installed ----
const all = facts({ dmi: vmware.dmi, pci: [...nv.pci, ...vmware.pci], usb: hp.usb, firmware: { readable: true, missing: [{ file: 'rtw89/rtw8852b_fw.bin', driver: 'rtw89_8852be' }] } });
eq('order', ids(run(all, { nonfree: false, installed: ['open-vm-tools'] })),
  ['printer:03f0=install', 'ippUsb=install', 'nvidiaDriver=unavailable', 'firmware:firmware-realtek=unavailable', 'vmwareTools=installed']);

// ---- conditions ----
eq('unknown condition never matches', matches({ cpu: ['x'] }, base()), false);
eq('empty all never matches', matches({ all: [] }, base()), false);
eq('two keys = all', matches({ arch: ['x86_64'], dmi: { sysVendor: ['Example Computer Co.'] } }, base()), true);
eq('two keys, one false', matches({ arch: ['aarch64'], dmi: { sysVendor: ['Example Computer Co.'] } }, base()), false);

// ---- the root helper's lines ----
eq('firmwareFromLog', firmwareFromLog(['fw iwlwifi-cc-a0-77.ucode iwlwifi', 'fw rtl_bt/rtl8761bu_fw.bin -', 'progress x', 'fw ../etc/shadow x', 'fw /etc/shadow x', 'fw a b c', 42]),
  [{ file: 'iwlwifi-cc-a0-77.ucode', driver: 'iwlwifi' }, { file: 'rtl_bt/rtl8761bu_fw.bin', driver: '' }]);

// ---- notify once ----
const list = run(all, { installed: ['open-vm-tools'] });
eq('fresh: installable only', fresh(list, []).map((x) => x.id), ['nvidiaDriver', 'printer:03f0', 'ippUsb', 'firmware:firmware-realtek']);
eq('fresh: minus notified', fresh(list, ['printer:03f0', 'nvidiaDriver']).map((x) => x.id), ['ippUsb', 'firmware:firmware-realtek']);

// ---- data hygiene ----
const PKG = /^[a-z0-9][a-z0-9+.-]+$/;
const named = [...DATA.rules.flatMap((r) => r.packages), ...Object.values(DATA.usb.vendors).flatMap((v) => [...(v.printer || []), ...(v.scanner || [])]),
  ...DATA.usb.ippUsb, ...DATA.firmware.map.map((m) => m.package)];
eq('every package name is valid', named.filter((p) => !PKG.test(p)), []);
eq('rule ids unique', DATA.rules.length, new Set(DATA.rules.map((r) => r.id)).size);
eq('vendor ids are 4 hex digits', Object.keys(DATA.usb.vendors).filter((v) => !/^[0-9a-f]{4}$/.test(v)), []);
eq('no X11 guest package', named.includes('open-vm-tools-desktop'), false);

// every text a card can show, in every language
const texts = ['title', 'why', 'detail', 'unavailable'];
const need = [
  ...DATA.rules.flatMap((r) => [...texts.map((x) => `${r.id}.${x}`), ...(r.notes || []).filter((x) => x !== 'nonfree').map((x) => `${r.id}.note.${x}`)]),
  ...['printer', 'scanner', 'ippUsb'].flatMap((k) => texts.map((x) => `${k}.${x}`)),
  ...['wifi', 'bluetooth', 'network', 'graphics', 'other'].flatMap((k) => texts.map((x) => `firmware.${k}.${x}`)),
  'title', 'subtitle', 'fixture', 'nonfreeBadge', 'installNonfree', 'confirmNonfree', 'dismiss', 'whatChanges', 'job', 'note.nonfree', 'note.firmware',
  'notify.title', 'notify.body', 'notify.open',
  'argonCase.title', 'argonCase.why', 'argonCase.detail',
];
for (const lang of ['en', 'es', 'pt', 'fr']) {
  const cat = JSON.parse(fs.readFileSync(path.join(repo, `assets/locales/${lang}/store.json`), 'utf8'));
  eq(`${lang}: every suggestion text`, need.filter((k) => !(`suggest.${k}` in cat)), []);
}
eq('firmware kinds in the data are known', DATA.firmware.map.filter((m) => !['wifi', 'bluetooth', 'network', 'graphics', 'other'].includes(m.kind)).length, 0);

const argon = evaluate(DATA, { caseFound: 'argon-one-v5' }, {});
eq('a case found leads the list', argon[0] && [argon[0].id, argon[0].state], ['argonCase', 'setup']);
eq('a case found is news once', fresh(argon, ['argonCase']).length, 0);
eq('no case, no card', evaluate(DATA, { caseFound: null }, {}).some((s) => s.id === 'argonCase'), false);

console.log(`suggest-engine-test: ${n - bad}/${n} ok`);
process.exit(bad ? 1 : 0);
