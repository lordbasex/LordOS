// livestart_test.mjs — what the live ISO opens by itself (sdk/livestart.js), and the boot menu that asks
// for it: the "Check this computer" entry in tools/iso/build.sh, and a theme whose menu has room for
// every entry (GRUB's theme has no scrollbar, so an entry that doesn't fit can't be seen).
//
//   node tests/livestart_test.mjs        (from server/; run_suites.sh runs it when node exists)
import fs from 'node:fs';
import path from 'node:path';
import { fileURLToPath } from 'node:url';

const here = path.dirname(fileURLToPath(import.meta.url));
const root = path.join(here, '../..');
const { liveStartWindow } = await import(path.join(root, 'sdk/livestart.js'));

let n = 0, bad = 0;
const eq = (name, got, want) => {
  n++;
  const g = JSON.stringify(got), w = JSON.stringify(want);
  if (g !== w) { bad++; console.log(`✗ ${name}: want ${w}, got ${g}`); }
};

const all = () => true;
const without = (id) => (x) => x !== id;
eq('installed: nothing', liveStartWindow({ live: false }, all), null);
eq('installed, the flag alone changes nothing', liveStartWindow({ live: false, liveCheck: true }, all), null);
eq('no information', liveStartWindow(null, all), null);
eq('live: the installer', liveStartWindow({ live: true, liveCheck: false }, all), ['installer']);
eq('live, an older server without the field', liveStartWindow({ live: true }, all), ['installer']);
eq('check: System Information on Performance', liveStartWindow({ live: true, liveCheck: true }, all), ['sysinfo', { args: { tab: 'perf' } }]);
eq('check without the app: the installer', liveStartWindow({ live: true, liveCheck: true }, without('sysinfo')), ['installer']);
eq('live without the installer', liveStartWindow({ live: true }, without('installer')), null);
eq('OVA first boot: the installer, on an installed system', liveStartWindow({ live: false, firstAccount: true }, all), ['installer']);
eq('OVA first boot without the installer', liveStartWindow({ live: false, firstAccount: true }, without('installer')), null);

const build = fs.readFileSync(path.join(root, 'tools/iso/build.sh'), 'utf8');
const entries = [...build.matchAll(/^menuentry "([^"]*)" \{\n\s*linux ([^\n]*)\n\s*initrd [^\n]*\n\}/gm)];
const check = entries.filter((m) => m[1].startsWith('Check this computer'));
eq('one entry for it', check.length, 1);
const words = (check[0] ? check[0][2] : '').split(/\s+/);
eq('it asks for the check', words.includes('lordos.check=1'), true);
eq('it boots the live system', words.includes('boot=live'), true);
eq('no other entry asks for it', entries.filter((m) => m[2].includes('lordos.check')).length, 1);
eq('it is not the default entry', entries[0] && entries[0][1].startsWith('LordOS'), true);

const theme = fs.readFileSync(path.join(root, 'tools/iso/grub/lordos/theme.txt'), 'utf8');
const num = (k) => Number(new RegExp(`^\\s*${k} = (\\d+)\\s*$`, 'm').exec(theme)?.[1]);
const menu = theme.slice(theme.indexOf('+ boot_menu'));
const height = Number(/height = (\d+)/.exec(menu)[1]);
// GRUB's own count (gui_list.c, get_num_shown_items): padding on both ends, and the selection band's top
// and bottom edges are taken from the height too. PNG height: bytes 20–23 of the header.
const edge = (f) => fs.readFileSync(path.join(root, 'tools/iso/grub/lordos', f)).readUInt32BE(20);
const fit = Math.floor((height + num('item_spacing') - 2 * num('item_padding') - edge('select_n.png') - edge('select_s.png')) / (num('item_height') + num('item_spacing')));
eq('the theme shows every entry', fit >= entries.length, true);

console.log(`livestart_test: ${n - bad}/${n} checks`);
process.exit(bad ? 1 : 0);
