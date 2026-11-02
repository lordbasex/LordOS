// imager_core_test.mjs — the LordOS Imager's shared logic (apps/imager/core): validators, the
// first-run file it writes, profiles without secrets, and the GitHub release list. Every file the
// serializer writes is also checked by the C parser the board uses (build/firstrun-test check), so
// the two can't drift apart.
//
//   node tests/imager_core_test.mjs        (from server/; run_suites.sh runs it when node exists)
import fs from 'node:fs';
import os from 'node:os';
import path from 'node:path';
import { execFileSync } from 'node:child_process';
import { fileURLToPath } from 'node:url';

const here = path.dirname(fileURLToPath(import.meta.url));
const core = (f) => import(path.join(here, '../../apps/imager/core', f));
const v = await core('validate.js');
const fr = await core('firstrun.js');
const rel = await core('releases.js');
const cat = await core('catalog.js');

let n = 0, bad = 0;
const ok = (name, cond, info = '') => { n++; if (!cond) { bad++; console.log(`✗ ${name} ${info}`); } };

// ---- validators (the same cases as tests/firstrun_test.c) ----
for (const h of ['pi', 'living-room-pi5', 'a', 'pi5']) ok(`hostname ${h}`, v.hostname(h) === null);
for (const h of ['', '-pi', 'pi-', 'Pi', 'pi.local', 'pi_5', '12345', 'localhost', 'x'.repeat(64), 'piñ']) ok(`bad hostname ${h}`, v.hostname(h) !== null);
for (const u of ['ana', '_svc', 'ana-maria_2', 'a'.repeat(32)]) ok(`user ${u}`, v.username(u) === null);
for (const u of ['Ana', '1ana', '-ana', 'root', 'lordos', 'systemd-x', 'sudo', 'ana maria', '', 'a'.repeat(33)]) ok(`bad user ${u}`, v.username(u) !== null);
ok('suggest user', v.suggestUser('Ana Pérez') === 'ana');
ok('suggest user root → none', v.suggestUser('Root Admin') === '');
ok('suggest host', v.suggestHostname('Ana Pérez') === 'ana-perez-pi');
ok('fullname', v.fullname('Ana Pérez') === null && v.fullname('李小龍') === null);
for (const f of ['Ana:0', 'Ana, Room', ' Ana', 'Ana\u0001', 'A‮B', '-o']) ok(`bad fullname ${JSON.stringify(f)}`, v.fullname(f) !== null);
ok('ssid', v.ssid('My Red ❤') === null && v.utf8('My Red ❤').length === 10);
ok('ssid 33 bytes', v.ssid('é'.repeat(17)) !== null);
ok('wifi pass', v.wifiPassword('hunter22') === null && v.wifiPassword('short') !== null && v.wifiPassword('ñandú-ñandú') !== null && v.wifiPassword('x'.repeat(64)) !== null);
ok('password mismatch', v.password('a', 'b') === 'imager.err.passMismatch');
ok('strength common', v.strength('raspberry').score === 0);
ok('strength strong', v.strength('correct-Horse-battery-9').score >= 3);
ok('zone', v.timezone('America/Argentina/Buenos_Aires') === null && v.timezone('../etc') !== null && v.timezone('Europe//X') !== null);

const ED = 'ssh-ed25519 AAAAC3NzaC1lZDI1NTE5AAAAIF8zdvpQ/Ic++n9TOGaDGEe9/+IRgtRBf1H86da41yJ8 ana@laptop';
const EC = 'ecdsa-sha2-nistp256 AAAAE2VjZHNhLXNoYTItbmlzdHAyNTYAAAAIbmlzdHAyNTYAAABBBBwbMKUZp+OoFOXG9AqteOZlGM/WkyBjc1CpgUqvgeHFv2kzd7XYR87xzMpqjVv57J4/d1776zu3hG/O3KNB+EY= ';
ok('ed25519 key', v.sshKey(ED).line === ED);
ok('ecdsa key, trailing space trimmed', v.sshKey(EC).line === EC.trim());
ok('key with options refused', !!v.sshKey(`command="/bin/sh" ${ED}`).error);
ok('dss refused', v.sshKey('ssh-dss AAAAB3NzaC1kc3MAAACB').error === 'imager.err.keyDss');
ok('type mismatch', !!v.sshKey(ED.replace('ssh-ed25519', 'ssh-rsa')).error);
ok('truncated key', !!v.sshKey(ED.slice(0, 70)).error);
const many = v.sshKeys(`# my keys\n${ED}\n\n${EC}\n${ED}\nnot a key\n`);
ok('key list', many.keys.length === 2 && many.errors.length === 1);

// ---- the first-run file ----
const H = '$6$rounds=10000$saltstringsaltst$OW1/O6BYHV6BcXZu8QVeXbDWra3Oeqh0sbHbbMCVNSnCM/UrjmM0Dp8vOuZeHBy/YTBmSK6H9qs/y3RnOaw5v.';
const PSK = 'f42c6fc52df0ebef9ebb4b90b38a5f902e83fe1b135a70e23aed762e9710a12e';
const full = {
  ...cat.defaultConfig(), hostname: 'pi-living', lang: 'es', timezone: 'America/Argentina/Buenos_Aires', keyboard: 'latam', variant: '',
  wifiCountry: 'AR', user: { name: 'ana', fullname: 'Ana Pérez', autologin: false },
  wifi: { enabled: true, ssid: 'My Red ❤', hidden: true, open: false }, ssh: 'keys', sshKeys: [v.sshKey(ED), v.sshKey(EC)],
  remote: true, bluetooth: false, theme: 'classic', mode: 'light', accent: 'violeta', wallpaper: 'lumo-celestial',
  cooler: 'active-cooler', performance: 'performance', overclock: true, autoUpdates: true,
};
ok('check full', fr.check(full, { passwordHash: H, psk: PSK }).length === 0, JSON.stringify(fr.check(full, { passwordHash: H, psk: PSK })));
const text = fr.build(full, { passwordHash: H, psk: PSK });
ok('no plain secrets', !/password=|wifi_password|hunter/.test(text));
ok('ssid as hex', text.includes('wifi_ssid_hex=4d792052656420e29da4\n'));
ok('two keys', text.split('\nssh_key=').length === 3);
const minimal = fr.build({ ...cat.defaultConfig(), hostname: 'pi' }, {});
ok('minimal has no user lines', !/user=|ssh=|remote_desktop/.test(minimal));
let threw = '';
try { fr.build({ ...full }, { psk: PSK }); } catch (e) { threw = e.message; }
ok('a user without a hash is refused', threw === 'imager.err.passEmpty');
ok('overclock without a fan is caught', fr.check({ ...full, cooler: 'passive' }, { passwordHash: H, psk: PSK }).some((p) => p.field === 'overclock'));
ok('ssh keys without keys is caught', fr.check({ ...full, sshKeys: [] }, { passwordHash: H, psk: PSK }).some((p) => p.key === 'imager.err.sshNeedsKey'));
const openNet = fr.build({ ...cat.defaultConfig(), wifi: { enabled: true, ssid: 'Cafe', hidden: false, open: true } }, {});
ok('open network: no psk line', openNet.includes('wifi_ssid_hex=43616665') && !openNet.includes('wifi_psk'));

// the C parser accepts every file the serializer writes
const cbin = path.join(here, '../build/firstrun-test');
if (fs.existsSync(cbin)) {
  const tmp = fs.mkdtempSync(path.join(os.tmpdir(), 'imager-core-'));
  const cases = [
    ['full', text], ['minimal', minimal], ['open', openNet],
    ['no cooling', fr.build({ ...cat.defaultConfig(), cooler: 'none', performance: 'conservative' }, {})],
    ['user only', fr.build({ ...cat.defaultConfig(), user: { name: 'bob', fullname: '', autologin: true }, ssh: 'password' }, { passwordHash: H })],
    ['defaults', fr.build(cat.defaultConfig(), {})],
  ];
  for (const [name, body] of cases) {
    const f = path.join(tmp, `${name.replace(/ /g, '-')}.txt`);
    fs.writeFileSync(f, body);
    let out = '';
    try { out = execFileSync(cbin, ['check', f], { encoding: 'utf8' }).trim(); } catch (e) { out = String(e.stdout || e).trim(); }
    ok(`C parser accepts "${name}"`, out === 'ok', out);
  }
  fs.rmSync(tmp, { recursive: true, force: true });
} else console.log('  (build/firstrun-test not built: the C cross-check is skipped)');

// ---- profiles never keep secrets ----
const prof = fr.toProfile('Living room', { ...full, passwordHash: H });
const ps = JSON.stringify(prof);
ok('profile has no hash or psk', !ps.includes(H) && !ps.includes(PSK) && !/password|psk/i.test(ps));
const back = fr.fromProfile(JSON.parse(ps), cat.defaultConfig());
ok('profile round trip', back.hostname === 'pi-living' && back.user.name === 'ana' && back.sshKeys.length === 2 && back.wifi.ssid === 'My Red ❤');
ok('foreign profile refused', fr.fromProfile({ format: 'x' }, cat.defaultConfig()) === null);

// ---- the release list ----
const list = rel.parseReleases(fs.readFileSync(path.join(here, 'imager/releases.json'), 'utf8'), 'rpi5');
ok('releases found', list.length >= 3, String(list.length));
ok('newest first', list[0].tag === 'v0.8.3-beta' && list[0].image.name === 'lordos-0.8.3-rpi5-arm64.img.xz' && list[0].image.size === 558119764);
ok('signature files', list.every((r) => r.sums.name === 'SHA256SUMS' && r.sig.name === 'SHA256SUMS.sig'));
const pc = rel.parseReleases(fs.readFileSync(path.join(here, 'imager/releases.json'), 'utf8'), 'pc');
ok('pc iso', pc[0].image.name === 'lordos-live-0.8.3-amd64.iso');
const armAssets = (v) => [{ name: `lordos-live-${v}-amd64.iso`, size: 7 }, { name: 'SHA256SUMS', size: 5 }, { name: 'SHA256SUMS.sig', size: 5 }];
ok('arm64 iso: no image, no release', rel.parseReleases([{ tag_name: 'v0.8.6-beta', assets: armAssets('0.8.6') }], 'pc-arm64').length === 0);
const armIso = rel.parseReleases([{ tag_name: 'v0.8.7-beta', assets: [...armAssets('0.8.7'), { name: 'lordos-live-0.8.7-arm64.iso', size: 9 }] }], 'pc-arm64');
ok('arm64 iso: its own image', armIso.length === 1 && armIso[0].image.name === 'lordos-live-0.8.7-arm64.iso');
ok('garbage list', rel.parseReleases('nope', 'rpi5').length === 0 && rel.parseReleases('{}', 'rpi5').length === 0);
ok('hostile names dropped', rel.parseReleases([{ tag_name: 'v1.0.0', assets: [{ name: '../lordos-1.0.0-rpi5-arm64.img.xz', size: 5 }, { name: 'SHA256SUMS', size: 5 }, { name: 'SHA256SUMS.sig', size: 5 }] }], 'rpi5').length === 0);
const point = rel.parseReleases([{ tag_name: 'v0.8.4.1-beta', published_at: '2026-10-25T20:00:00Z', assets: [{ name: 'lordos-0.8.4.1-rpi5-arm64.img.xz', size: 5 }, { name: 'SHA256SUMS', size: 5 }, { name: 'SHA256SUMS.sig', size: 5 }] }], 'rpi5');
ok('point release (four numbers)', point.length === 1 && point[0].version === '0.8.4.1' && point[0].image.name === 'lordos-0.8.4.1-rpi5-arm64.img.xz');
ok('five numbers dropped', rel.parseReleases([{ tag_name: 'v0.8.4.1.2', assets: [{ name: 'lordos-0.8.4.1.2-rpi5-arm64.img.xz', size: 5 }, { name: 'SHA256SUMS', size: 5 }, { name: 'SHA256SUMS.sig', size: 5 }] }], 'rpi5').length === 0);
ok('bad tag dropped', rel.parseReleases([{ tag_name: 'v1.0.0/../x', assets: [] }], 'rpi5').length === 0);
ok('fits', rel.fits(2.8e9, 4e9) === 'yes' && rel.fits(2.8e9, 2.9e9) === 'tight' && rel.fits(3e9, 2e9) === 'no');
ok('download url', rel.downloadUrl('v0.8.3-beta', 'SHA256SUMS') === 'https://github.com/lordbasex/LordOS/releases/download/v0.8.3-beta/SHA256SUMS');

console.log(`imager-core-test: ${n} checks, ${bad} failed`);
process.exit(bad ? 1 : 0);
