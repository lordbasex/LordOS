// platform.js — the LordOS Imager on LordOS itself: everything the shared UI needs from the system.
// The desktop app (desktop/imager) replaces this module with its own (desktop/imager/web/platform.js,
// Tauri commands) and keeps the same functions. docs/IMAGER.md, "One UI, two platforms".
//
// What needs root is only the closed admin actions imager-fetch, imager-fetch-cancel,
// imager-cache-clear and imager-write (server/src/imager/imager_admin.c), plus disk-mount,
// disk-unmount, disk-eject and disk-cancel (the Disks app's). The first-run file goes onto the
// card's boot partition through lord.fs, mounted for the user like any USB stick.
import { createElement as h } from 'react';
import { admin, disks, fs, settings, keyboard, sound, notify, t, FilePicker, createStore, useStore } from '@lordos/sdk';
import zonesData from './data/zones.json';
import layoutsData from './data/layouts.json';

const CACHE = '.cache/lordos/imager';
const has = () => typeof lord !== 'undefined' && !!lord.imager;

// progress phase=<p> done=<n> total=<n>
function progressOf(line) {
  const m = /^progress phase=(\w+) done=(\d+) total=(\d+)/.exec(line || '');
  return m ? { phase: m[1], done: Number(m[2]), total: Number(m[3]) } : null;
}

// the file picker sheet is drawn by <platform.Layer /> at the app's root
const picker = createStore(null);
function pick(filter, title, start = '') {
  return new Promise((resolve) => picker.set({ filter, title, resolve, start }));
}

function driveOf(d) {
  const mounted = [];
  for (const p of d.partitions || []) if (p.mountpoint) mounted.push(p.label || p.name);
  if (d.mountpoint) mounted.push(d.label || d.name);
  return {
    id: d.name, name: d.name, model: [d.vendor, d.model].filter(Boolean).join(' ').trim() || d.name, rawModel: d.model || '', serial: d.serial || '',
    size: d.size, transport: d.transport === 'mmc' ? 'sd' : d.transport || '', removable: !!d.removable, system: !!d.system, readOnly: !!d.ro,
    labels: (d.partitions || []).map((p) => p.label).filter(Boolean), mounted,
  };
}

export const platform = {
  kind: 'native',
  domFocus: false,
  a11y: () => ({}),               // LordOS has no screen reader yet; the desktop app fills these in
  available: () => has() && admin.available(),

  // ---- drives: removable, not the system disk, not read-only (the root side checks it again) ----
  drives() {
    return disks.list().filter((d) => d.removable && !d.system && !/^(loop|zram|sr|dm-|md)/.test(d.name)).map(driveOf);
  },
  watchDrives(fn) { return disks.watch(fn); },
  hiddenDriveCount() { return disks.list().filter((d) => !d.removable || d.system).length; },

  // ---- releases and downloads (curl as the user, in ~/.cache/lordos/imager) ----
  cacheDir: () => `${fs.home()}/${CACHE}`,
  cachePath: (tag, name) => `${fs.home()}/${CACHE}/${tag}/${name}`,
  async releases() {
    await admin.run('imager-fetch', 'meta', null, { quiet: true });
    return fs.read(`${CACHE}/releases.json`);
  },
  cachedReleases() { try { return fs.read(`${CACHE}/releases.json`); } catch { return null; } },
  isCached(tag, asset) { try { const s = fs.stat(`${CACHE}/${tag}/${asset.name}`); return !!s && s.size === asset.size; } catch { return false; } },
  partialSize(tag, asset) { try { return fs.stat(`${CACHE}/${tag}/${asset.name}.part`)?.size || 0; } catch { return 0; } },
  async download(tag, asset, onProgress) {
    await admin.run('imager-fetch', `file\n${tag}\n${asset.name}\n${asset.size}`, (l) => { const p = progressOf(l); if (p) onProgress?.(p); }, { quiet: true });
    return this.cachePath(tag, asset.name);
  },
  cancelDownload() { return admin.run('imager-fetch-cancel', '', null, { quiet: true }).catch(() => {}); },
  readText(path) { return fs.read(path); },
  checkSums: (sums, sig) => (has() ? lord.imager.checkSums(sums, sig) : { ok: false }),

  // ---- a file of the user's own ----
  async pickImage() {
    // start in Downloads (where an image usually is), relative to the home folder
    let start = '';
    try {
      const m = /^XDG_DOWNLOAD_DIR="\$HOME\/([^"]+)"/m.exec(fs.read('.config/user-dirs.dirs'));
      if (m && fs.stat(m[1])?.dir) start = m[1];
    } catch { /* the home folder */ }
    const path = await pick(/\.(img|img\.xz|iso)$/i, t('imager.image.pickTitle'), start);
    return path ? this.imageInfo(path) : null;
  },
  imageInfo: (path) => (has() ? lord.imager.imageInfo(path) : null),
  async pickKeyFile() {
    let start = '';
    try { if (fs.stat('.ssh')?.dir) start = '.ssh'; } catch { /* the home folder */ }
    const path = await pick(/\.pub$/i, t('imager.remote.pickKey'), start);
    return path ? fs.read(path) : null;
  },
  Layer() {
    const p = useStore(picker);
    if (!p) return null;
    const done = (v) => { picker.set(null); p.resolve(v); };
    return h(FilePicker, { filter: p.filter, title: p.title, onPick: (path) => done(path), onClose: () => done(null), start: p.start || '' });
  },

  // ---- secrets: derived here, only the results go into the file ----
  async hashPassword(p) { return has() ? lord.imager.hashPassword(p) : null; },
  async wifiPsk(ssid, p) { return has() ? lord.imager.wifiPsk(ssid, p) : null; },

  // ---- writing ----
  async write({ drive, image }, onProgress) {
    const lines = [drive.name, String(drive.size), drive.rawModel, drive.serial, image.official ? 'official' : 'custom', image.path];
    if (image.official) lines.push(image.sums, image.sig);
    const ok = await admin.run('imager-write', lines.join('\n'), (l) => { const p = progressOf(l); if (p) onProgress?.(p); }, { quiet: true });
    const kv = Object.fromEntries(String(ok).split(' ').slice(1).map((x) => x.split('=')));
    return { sha256: kv.sha256 || '', mbps: Number(kv.mbps || 0), ms: Number(kv.ms || 0) };
  },
  cancelWrite(drive) { return admin.run('disk-cancel', drive.name, null, { quiet: true }).catch(() => {}); },
  // the image's first partition (bootfs, FAT), mounted for the user, the file written, unmounted
  async writeFirstRun(drive, text, name) {
    const part = /[0-9]$/.test(drive.name) ? `${drive.name}p1` : `${drive.name}1`;
    let dir = '';
    for (let i = 0; i < 20 && !dir; i++) {
      try { dir = String(await admin.run('disk-mount', part, null, { quiet: true })); } catch (e) {
        if (!/disk\.alreadyMounted/.test(e.code || '') && i < 19) await new Promise((r) => setTimeout(r, 300)); else throw e;
      }
    }
    try { fs.write(`${dir}/${name}`, text); } finally { await admin.run('disk-unmount', part, null, { quiet: true }).catch(() => {}); }
  },
  eject(drive) { return admin.run('disk-eject', drive.name, null, { quiet: true }); },

  // ---- this computer's settings, as suggestions ----
  defaults() {
    const s = settings.get();
    const kb = keyboard.active?.() || {};
    const lang = ['en', 'es', 'pt', 'fr'].includes(s.language) ? s.language : 'en';
    let zone = '';
    try { zone = lord.time.zone() || ''; } catch { /* keep empty */ }
    const region = (s.region || '').split('_')[1] || '';
    return { lang, timezone: zone, keyboard: kb.layout || '', variant: kb.variant || '', country: /^[A-Z]{2}$/.test(region) ? region : '' };
  },
  zones: () => zonesData.zones,
  // the same lists the image ships (Debian 13 tzdata and xkb-data; apps/imager/data)
  layouts: () => layoutsData.layouts,
  async scanWifi() {
    if (typeof lord === 'undefined' || !lord.net) return null;
    const radio = (lord.net.interfaces() || []).find((i) => i.type === 'wifi');
    if (!radio) return null;
    const found = [];
    await admin.run('net-wifi-scan', radio.name, (line) => { try { found.push(JSON.parse(line)); } catch { /* text */ } }, { quiet: true });
    const seen = new Set();
    return found.sort((a, b) => b.signal - a.signal).filter((n) => n.ssid && !seen.has(n.ssid) && seen.add(n.ssid));
  },

  // ---- the app's own options and profiles (settings.json "imager"; profiles never hold secrets) ----
  loadOptions() { return settings.get().imager || {}; },
  saveOptions(o) { settings.set((s) => ({ ...s, imager: { ...(s.imager || {}), ...o } })); },
  loadProfiles() { try { const p = JSON.parse(fs.read('.config/lordos/imager-profiles.json')); return Array.isArray(p) ? p : []; } catch { return []; } },
  saveProfiles(list) { fs.mkdir('.config/lordos'); fs.write('.config/lordos/imager-profiles.json', JSON.stringify(list, null, 2)); },
  cacheSize() { try { return fs.du(CACHE, 200)?.size || 0; } catch { return 0; } },
  async clearCache() { await admin.run('imager-cache-clear', '', null, { quiet: true }); },
  // the language and theme follow LordOS; updates come with LordOS itself
  followsSystem: true,
  appUpdates: null,

  done({ title, body, sound: withSound }) {
    if (withSound) sound.play('notify');
    notify({ app: 'imager', title, body, sound: false });
  },
};
