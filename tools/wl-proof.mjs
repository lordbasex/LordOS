#!/usr/bin/env node
// wl-proof.mjs — real programs on a headless lord-server, each screenshotted, as proof that what the conformance tool
// checks by protocol works with the apps people use (docs/wayland-proofs/, docs/WAYLAND.md "Conformance").
//
//   node tools/wl-proof.mjs [--out docs/wayland-proofs] [--only name,name] [--keep]
//
// It never touches a session: its own `lord-server --headless 1280x800` on the CPU path (LORD_GPU=0), in a private
// runtime folder and home, with the theme plus a small hook that saves a screenshot when this script asks (a request
// file in that home; the screenshot goes through the Screenshots app's permission, granted in that home only). Each
// program runs with WAYLAND_DEBUG=1: the report counts the requests that matter (ping/pong, subsurface order,
// viewports, decorations) and any protocol error. Programs that aren't installed are listed, never installed.
// lord-server is stopped with SIGTERM, then SIGKILL, and the folder is removed (unless --keep).
import fs from 'node:fs';
import os from 'node:os';
import path from 'node:path';
import { spawn, execFileSync } from 'node:child_process';

const ROOT = path.resolve(path.dirname(new URL(import.meta.url).pathname), '..');
const SERVER = path.join(ROOT, 'server');
const BUILD = path.join(SERVER, 'build');
const args = process.argv.slice(2);
const opt = (k, d) => { const i = args.indexOf(k); return i >= 0 ? args[i + 1] : d; };
const OUT = path.resolve(opt('--out', path.join(ROOT, 'docs', 'wayland-proofs')));
const ONLY = (opt('--only', '') || '').split(',').filter(Boolean);
const KEEP = args.includes('--keep');
const sleep = (ms) => new Promise((r) => setTimeout(r, ms));

const HOOK = `
;(() => {
  // a screenshot when the proof script asks: $HOME/proof.req holds "<n> <name>"; the picture goes to $HOME/<name>.png
  const home = lord.sys && lord.sys.home ? lord.sys.home() : null;
  let done = 0;
  // permission prompts (idle notifications, shortcuts) are answered here: an open prompt holds the keyboard
  const orig = globalThis.__lordWayland;
  globalThis.__lordWayland = (id, type, d) => {
    if (type === 'idlenotify') { setTimeout(() => { try { lord.wayland.idleAnswer(d.app, true); } catch (e) {} }, 100); return undefined; }
    if (type === 'shortcuts') { setTimeout(() => { try { lord.wayland.shortcutsAnswer(d.app, true); } catch (e) {} }, 100); return undefined; }
    return orig(id, type, d);
  };
  setInterval(() => {
    try {
      const h = home || globalThis.__proofHome;
      if (!h) return;
      let t = '';
      try { t = String(lord.fs.read(h + '/proof.req') || '').trim(); } catch (e) { return; }   // (not asked yet)
      const m = t.match(/^(\\d+) ([a-z0-9_.-]+)$/);
      if (!m || +m[1] <= done) return;
      done = +m[1];
      const id = lord.media.screenshot('screenshots', {});
      lord.media.saveImage(id, h + '/' + m[2] + '.png', { format: 'png' });
      console.log('WLPROOF shot ' + m[2]);
    } catch (e) { console.log('WLPROOF error ' + e); }
  }, 300);
})();
`;

async function startServer() {
  const theme = path.join(ROOT, 'themes', 'default', 'theme.bundle.js');
  const bin = path.join(BUILD, 'lord-server-test');     // the conformance tool's build (same sources, -DLORD_TEST_HOOKS)
  if (!fs.existsSync(bin) || !fs.existsSync(theme)) throw new Error('needs server/build/lord-server-test and the theme bundle (make -C server; npm run build)');
  const D = fs.mkdtempSync(path.join(os.tmpdir(), 'lwproof.'));
  fs.chmodSync(D, 0o700);
  const home = path.join(D, 'home');
  fs.mkdirSync(path.join(home, '.config', 'lordos'), { recursive: true });
  fs.writeFileSync(path.join(home, '.config', 'lordos', 'settings.json'), '{"schema": 9, "desktopChosen": true, "language": "en", "welcomeShown": true}\n');
  fs.writeFileSync(path.join(home, '.config', 'lordos', 'permisos.json'), '{ "version": 1, "apps": { "screenshots": { "screen": { "state": "allow", "when": 1 } } } }\n');
  fs.writeFileSync(path.join(D, 'theme.js'), fs.readFileSync(theme, 'utf8') + `;globalThis.__proofHome = ${JSON.stringify(home)};` + HOOK);
  const env = { ...process.env, XDG_RUNTIME_DIR: D, HOME: home, LORD_GPU: '0', LANG: 'en_US.UTF-8', LORD_HEADLESS_DRAW: '1' };
  for (const k of ['WAYLAND_DISPLAY', 'WAYLAND_SOCKET', 'DISPLAY', 'XAUTHORITY', 'LORD_SESSIOND', 'LORDOS_SESSIOND', 'DBUS_SESSION_BUS_ADDRESS']) delete env[k];
  const out = fs.openSync(path.join(D, 'server.log'), 'w');
  const P = spawn(bin, ['--headless', '1280x800', '--theme', path.join(D, 'theme.js'), '--timeout', '900', '--user', os.userInfo().username],
    { cwd: SERVER, env, stdio: ['ignore', out, out], detached: true });
  let sock = '';
  for (let i = 0; i < 120 && !sock; i++) { await sleep(250); sock = fs.readdirSync(D).find((n) => /^wayland-\d+$/.test(n)) || ''; }
  const stop = async () => {
    try { process.kill(-P.pid, 'SIGTERM'); } catch { return; }
    for (let i = 0; i < 30; i++) { await sleep(100); if (P.exitCode !== null || P.signalCode !== null) return; }
    try { process.kill(-P.pid, 'SIGKILL'); } catch {}
  };
  if (!sock) { await stop(); throw new Error(`lord-server made no Wayland socket (log: ${D}/server.log)`); }
  await sleep(3000);
  let n = 0;
  const shot = async (name) => {
    const file = path.join(home, `${name}.png`);
    try { fs.unlinkSync(file); } catch {}
    fs.writeFileSync(path.join(home, 'proof.req'), `${++n} ${name}\n`);
    for (let i = 0; i < 100; i++) {
      await sleep(200);
      try { if (fs.statSync(file).size > 0) { await sleep(300); return file; } } catch {}
    }
    return null;
  };
  return { D, home, sock, env: { ...env, WAYLAND_DISPLAY: sock }, stop, shot, alive: () => P.exitCode === null && P.signalCode === null };
}

// a screenshot as a small PNG: 256 colours (python3 with PIL), else copied as it is
const small = (src, dst) => {
  try {
    execFileSync('python3', ['-I', '-c', 'import sys\nfrom PIL import Image\nImage.open(sys.argv[1]).convert("RGB").quantize(256).save(sys.argv[2], optimize=True)', src, dst], { stdio: 'ignore' });
  } catch { fs.copyFileSync(src, dst); }
};
const have = (exe) => { try { return execFileSync('sh', ['-c', `command -v ${exe}`], { encoding: 'utf8' }).trim(); } catch { return ''; } };
const version = (cmd) => { try { return execFileSync(cmd[0], cmd.slice(1), { encoding: 'utf8', stdio: ['ignore', 'pipe', 'pipe'], timeout: 5000 }).split('\n')[0].trim(); } catch (e) { return String(e.stdout || '').split('\n')[0].trim() || '?'; } };
const pkgVersion = (pkg) => { try { return execFileSync('dpkg-query', ['-W', '-f=${Version}\n', pkg], { encoding: 'utf8' }).split('\n')[0].trim(); } catch { return '?'; } };

// what each program shows, what its WAYLAND_DEBUG log must hold
const PROOFS = [
  { name: 'wl_subcompositor-wp_viewporter', exe: path.join(BUILD, 'wlproof-client'), cmd: [path.join(BUILD, 'wlproof-client'), '25'], wait: 2500,
    app: 'build/wlproof-client (tests/wlproof_client.c, purpose-built)', ver: () => 'this tree',
    shows: 'Subsurface order and viewport: red placed above green (place_above), blue placed below the window (place_below, seen only through its transparent right third), and a 64×64 four-colour buffer cropped to its white quadrant and scaled to 100×100 (wp_viewport set_source + set_destination).',
    want: ['wl_subsurface#\\d+\\.place_above', 'wl_subsurface#\\d+\\.place_below', 'wp_viewport#\\d+\\.set_source'] },
  { name: 'xdg_wm_base-foot', exe: 'foot', cmd: ['foot', '--override=colors.background=202830', 'sh', '-c', 'printf "LordOS xdg-shell conformance proof\\nfoot on a headless lord-server\\n"; sleep 30'], wait: 4000,
    app: 'foot', ver: () => `foot ${pkgVersion('foot')}`,
    shows: 'foot (wayland-native terminal) mapped and focused: it answered xdg_wm_base.ping with pong, acked each configure once (invalid_serial is now enforced), and asked for the theme\'s frame through xdg-decoration.',
    want: ['xdg_wm_base#\\d+\\.pong', 'xdg_surface#\\d+\\.ack_configure', 'zxdg_toplevel_decoration_v1#\\d+\\.set_mode'] },
  { name: 'xdg_wm_base-gtk4-zenity', exe: 'zenity', cmd: ['zenity', '--info', '--title=GTK 4 proof', '--text=GTK 4 / libadwaita (zenity) on a headless lord-server: xdg-shell, subsurfaces, viewporter'], wait: 5000,
    env: { GDK_BACKEND: 'wayland', GSK_RENDERER: 'cairo' },
    app: 'zenity (GTK 4, libadwaita)', ver: () => `zenity ${pkgVersion('zenity')}, GTK ${pkgVersion('libgtk-4-1')}`,
    shows: 'A GTK 4 / libadwaita dialog with its own decorations: GTK 4 answers ping, acks configures once and creates its xdg_surface before any buffer (invalid_surface_state and unconfigured_buffer are now enforced).',
    want: ['xdg_wm_base#\\d+\\.pong', 'xdg_surface#\\d+\\.ack_configure'] },
  { name: 'xdg_wm_base-qt-featherpad', exe: 'featherpad', cmd: ['featherpad', '--standalone'], wait: 6000,
    env: { QT_QPA_PLATFORM: 'wayland' },
    app: 'FeatherPad (Qt 6)', ver: () => `featherpad ${pkgVersion('featherpad')}, Qt ${pkgVersion('libqt6gui6')}`,
    shows: 'A Qt 6 editor (FeatherPad) on Wayland: xdg-shell with the new error checks, xdg-decoration (server-side frame from the theme).',
    want: ['xdg_wm_base#\\d+\\.pong', 'xdg_surface#\\d+\\.ack_configure'] },
  { name: 'linux-dmabuf-vkcube', exe: 'vkcube', cmd: ['vkcube', '--wsi', 'wayland', '--c', '2000'], wait: 5000,
    app: 'vkcube (Vulkan)', ver: () => `vkcube ${pkgVersion('vulkan-tools')}, Mesa ${pkgVersion('mesa-vulkan-drivers')}`,
    shows: 'vkcube through the Vulkan WSI: its buffers pass the new zwp_linux_buffer_params_v1 out_of_bounds checks (offset + stride × height inside each dmabuf).',
    want: ['wl_surface#\\d+\\.commit'] },
  { name: 'wp_fifo_manager_v1-wp_commit_timing_manager_v1-wp_linux_drm_syncobj_manager_v1-vkcube', exe: 'vkcube', cmd: ['vkcube', '--wsi', 'wayland', '--c', '2000'], wait: 5000,
    app: 'vkcube (Vulkan)', ver: () => `vkcube ${pkgVersion('vulkan-tools')}, Mesa ${pkgVersion('mesa-vulkan-drivers')}`,
    shows: 'vkcube\'s FIFO present mode through Mesa\'s Vulkan WSI on the new protocols: every frame waits on the fifo-v1 barrier the one before set, carries a commit-timing-v1 target time, and explicit-sync acquire and release points (linux-drm-syncobj-v1) on timelines it imported; no protocol error.',
    want: ['wp_fifo_v1#\\d+\\.set_barrier', 'wp_fifo_v1#\\d+\\.wait_barrier', 'wp_commit_timer_v1#\\d+\\.set_timestamp', 'wp_linux_drm_syncobj_manager_v1#\\d+\\.import_timeline',
      'wp_linux_drm_syncobj_surface_v1#\\d+\\.set_acquire_point', 'wp_linux_drm_syncobj_surface_v1#\\d+\\.set_release_point'] },
  { name: 'xdg_system_bell_v1-wp_color_manager_v1-foot', exe: 'foot', cmd: ['foot', '--override=colors.background=302820', 'sh', '-c', 'printf "LordOS: foot rings the bell (BEL)\\n"; sleep 1; printf "\\a"; sleep 30'], wait: 4000,
    app: 'foot', ver: () => `foot ${pkgVersion('foot')}`,
    shows: 'foot rang the terminal bell (BEL) through xdg-system-bell-v1 (its default bell.system=yes), on its own surface (LordOS plays its alert sound unless Do Not Disturb is on), and described its surfaces as sRGB through color-management-v1 (a parametric image description set on each). No protocol error.',
    want: ['xdg_system_bell_v1#\\d+\\.ring', 'wp_color_manager_v1#\\d+\\.create_parametric_creator', 'wp_color_management_surface_v1#\\d+\\.set_image_description'] },
  { name: 'egl-es2gears', exe: 'es2gears_wayland', cmd: ['es2gears_wayland'], wait: 5000,
    app: 'es2gears_wayland (EGL / GLES2)', ver: () => `mesa-utils ${pkgVersion('mesa-utils-bin')}`,
    shows: 'An EGL client: its buffers (dmabuf or shm through Mesa\'s Wayland EGL) and xdg-shell flow pass the new checks.',
    want: ['wl_surface#\\d+\\.commit'] },
  { name: 'chromium', exe: 'chromium', cmd: ['chromium', '--ozone-platform=wayland', '--no-first-run', '--no-default-browser-check', '--disable-gpu', '--user-data-dir=@D/chromium',
    'data:text/html,<body style="font:28px sans-serif;background:%23eef"><h1>LordOS Wayland conformance</h1><p>Chromium on a headless lord-server (CPU path)</p><select><option>xdg-shell</option><option>viewporter</option></select></body>'], wait: 9000,
    app: 'Chromium', ver: () => version(['chromium', '--version']),
    shows: 'Chromium (Ozone/Wayland): subsurfaces, viewporter, xdg-shell, xdg-decoration, xdg-activation and the data device with the new error checks; no protocol error.',
    want: ['xdg_wm_base#\\d+\\.pong'] },
];

async function main() {
  fs.mkdirSync(OUT, { recursive: true });
  const S = await startServer();
  console.log(`lord-server (headless, CPU) on ${S.sock} in ${S.D}`);
  const results = [];
  try {
    for (const p of PROOFS) {
      if (ONLY.length && !ONLY.includes(p.name)) continue;
      const exe = p.exe.startsWith('/') ? (fs.existsSync(p.exe) ? p.exe : '') : have(p.exe);
      if (!exe) { results.push({ name: p.name, result: 'missing', note: `${p.exe} not installed` }); console.log(`${p.name}: skip (${p.exe} not installed)`); continue; }
      const log = path.join(S.D, `${p.name}.wl.log`);
      const fd = fs.openSync(log, 'w');
      const cmd = p.cmd.map((a) => a.replace('@D', S.D));
      const C = spawn(cmd[0], cmd.slice(1), { env: { ...S.env, WAYLAND_DEBUG: '1', ...(p.env || {}) }, cwd: S.home, stdio: ['ignore', fd, fd], detached: true });
      await sleep(p.wait);
      const file = await S.shot(p.name);
      const text = fs.readFileSync(log, 'utf8');
      const errors = (text.match(/wl_display#1\.error\([^\n]*/g) || []).slice(0, 3);
      const found = p.want.map((w) => ({ w, n: (text.match(new RegExp(w, 'g')) || []).length }));
      const alive = C.exitCode === null && C.signalCode === null;
      try { process.kill(-C.pid, 'SIGTERM'); } catch {}
      await sleep(500);
      try { process.kill(-C.pid, 'SIGKILL'); } catch {}
      const ok = file && !errors.length && found.every((f) => f.n > 0);
      if (file) small(file, path.join(OUT, `${p.name}.png`));
      const r = { name: p.name, result: ok ? 'pass' : 'fail', app: p.app, version: p.ver(), shows: p.shows, alive, errors, found, png: file ? `${p.name}.png` : null };
      results.push(r);
      console.log(`${p.name}: ${r.result} — ${r.version}; ${found.map((f) => `${f.w.replace(/@\\\\d\+\\\\./, '.')} ×${f.n}`).join(', ')}${errors.length ? '; ERROR ' + errors.join(' | ') : ''}${alive ? '' : ' (it exited)'}`);
    }
    if (!S.alive()) console.log('lord-server was not alive at the end');
  } finally {
    await S.stop();
    if (!KEEP) fs.rmSync(S.D, { recursive: true, force: true }); else console.log(`kept ${S.D}`);
  }
  // --only: the other programs' results stay as they were recorded
  let all = results;
  if (ONLY.length) {
    try {
      const old = JSON.parse(fs.readFileSync(path.join(OUT, 'proofs.json'), 'utf8')).results || [];
      all = PROOFS.map((p) => results.find((r) => r.name === p.name) || old.find((r) => r.name === p.name)).filter(Boolean);
    } catch {}
  }
  fs.writeFileSync(path.join(OUT, 'proofs.json'), JSON.stringify({ date: new Date().toISOString().slice(0, 10), results: all }, null, 1) + '\n');
  process.exitCode = results.some((r) => r.result === 'fail') ? 1 : 0;
}
main().catch((e) => { console.error(e.message || e); process.exitCode = 2; });
