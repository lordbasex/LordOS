// iso_firmware_test.mjs — the live ISO's firmware list (tools/iso/build.sh PKGS_FIRMWARE) against the missing-firmware
// map the Store uses (assets/store/suggestions.json). The ISO installs with --no-install-recommends, so the packages
// Debian split out of firmware-misc-nonfree (firmware-mediatek, firmware-intel-graphics, which it only Recommends) must
// be named one by one, or a Wi-Fi card such as the MediaTek MT7921 binds its driver but never gets an interface (#227).
// Every package the map names for Wi-Fi, network or graphics must be in the ISO unless it is listed below with a reason.
// Run: node tests/iso_firmware_test.mjs
import { readFileSync } from 'node:fs';
import path from 'node:path';
import { fileURLToPath } from 'node:url';

const repo = path.resolve(path.dirname(fileURLToPath(import.meta.url)), '../..');
const build = readFileSync(path.join(repo, 'tools/iso/build.sh'), 'utf8');
const m = build.match(/^PKGS_FIRMWARE="([^"]*)"/m);
let ok = 0, bad = 0;
const check = (name, c) => { if (c) ok++; else { bad++; console.log(`FAIL ${name}`); } };
check('PKGS_FIRMWARE found in tools/iso/build.sh', !!m);
const iso = new Set((m ? m[1] : '').split(/\s+/).filter(Boolean));

// left out on purpose: old Marvell SDIO/USB Wi-Fi (rare on the PCs the ISO targets; the Store offers it when the
// kernel asks), and NVIDIA's GSP firmware (65 MB; nouveau runs without it and the Store offers it)
const NOT_IN_ISO = new Set(['firmware-libertas', 'firmware-nvidia-graphics']);
const map = JSON.parse(readFileSync(path.join(repo, 'assets/store/suggestions.json'), 'utf8')).firmware.map;
const wanted = new Set(map.filter(e => ['wifi', 'network', 'graphics'].includes(e.kind)).map(e => e.package));
for (const p of wanted) {
  if (NOT_IN_ISO.has(p)) continue;
  check(`${p} in the ISO's PKGS_FIRMWARE`, iso.has(p));
}
// the two cases the reports named
check('firmware-mediatek (MT7921/MT7922 Wi-Fi, #227)', iso.has('firmware-mediatek'));
check('firmware-intel-graphics (i915 DMC, runtime power management)', iso.has('firmware-intel-graphics'));
for (const p of NOT_IN_ISO) check(`${p} is still a map package (else drop it from NOT_IN_ISO)`, wanted.has(p));

console.log(`iso-firmware-test: ${ok} ok, ${bad} failed`);
process.exit(bad ? 1 : 0);
