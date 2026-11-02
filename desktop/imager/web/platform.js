// platform.js — the LordOS Imager on the desktop (Tauri: `macos`, `windows`, `linux`). The same functions as
// apps/imager/platform.js (LordOS), backed by the app's Rust commands (src-tauri/src/main.rs) and
// lordos-imager-core. build-web.mjs puts this file in place of the native one.
import { createElement as h } from 'react';
import { invoke } from '@tauri-apps/api/core';
import { listen } from '@tauri-apps/api/event';
import { open as openDialog } from '@tauri-apps/plugin-dialog';
import { t, setLanguage, appearance, createStore } from './sdk.jsx';
import zonesData from '../../../apps/imager/data/zones.json';
import { parseReleases } from '../../../apps/imager/core/releases.js';
import layoutsData from '../../../apps/imager/data/layouts.json';

let info_ = { os: '', lang: 'en', timezone: '', country: '', cache: '' };
let options = {};
let drivesNow = [];
const driveSubs = new Set();
let progressFn = null;
listen('imager-progress', (e) => progressFn?.(e.payload));

// Rust errors are { code, detail }: the message is the translated code, as on LordOS
let currentDrive = '';
function fail(e) {
  const code = e?.code || 'imager.fetchFailed';
  const detail = e?.detail || '';
  let msg = t(`errors.${code}`, { name: currentDrive || detail, code: detail, size: '', capacity: '' });
  if (msg === `errors.${code}`) msg = code;
  else if (detail && !msg.includes(detail)) msg = `${msg} (${detail})`;
  return Object.assign(new Error(msg), { code, detail });
}
const call = async (cmd, args) => { try { return await invoke(cmd, args); } catch (e) { throw fail(e); } };

async function refreshDrives() {
  try { drivesNow = await invoke('list_drives'); } catch { drivesNow = []; }
  for (const f of driveSubs) f();
}

export const info = () => info_;
export async function init() {
  info_ = await invoke('platform');
  options = (await invoke('load_options')) || {};
  setLanguage(options.lang || info_.lang);
  appearance.set(options.theme || 'auto');
  await refreshDrives();
  setInterval(refreshDrives, 2000);        // drives appear and leave: a cheap listing every 2 s
}

const toDrive = (d) => ({ ...d, name: d.path.replace(/^\/dev\/r?/, '').replace(/^\\\\\.\\/, ''), rawModel: d.model, readOnly: d.read_only, raw: d });
const sizeCache = new Map();
// which release images are already downloaded (the UI asks synchronously while drawing)
async function cacheStates(text) {
  for (const dev of ['rpi5', 'pc']) {
    for (const r of parseReleases(text, dev)) {
      try { sizeCache.set(`${r.tag}/${r.image.name}`, await invoke('cache_state', { tag: r.tag, name: r.image.name, size: r.image.size })); } catch { /* not cached */ }
    }
  }
}

export const platform = {
  kind: 'desktop',
  domFocus: true,
  // screen-reader labels and keyboard activation for the browser view
  a11y({ label, role, selected, disabled, onActivate, invalid, value } = {}) {
    const o = {};
    if (role) o.role = role;
    if (label) o['aria-label'] = label;
    if (selected !== undefined && (role === 'radio' || role === 'tab' || role === 'option' || role === 'switch')) o[role === 'switch' || role === 'radio' ? 'aria-checked' : 'aria-selected'] = !!selected;
    if (disabled) o['aria-disabled'] = true;
    if (invalid) o['aria-invalid'] = true;
    if (role === 'progressbar') { o['aria-valuenow'] = value; o['aria-valuemin'] = 0; o['aria-valuemax'] = 100; }
    if (onActivate) {
      o.tabIndex = 0;
      o.onKeyDown = (e) => { if (e.key === 'Enter' || e.key === ' ') { e.preventDefault(); onActivate(); } };
    }
    return o;
  },
  available: () => true,

  drives: () => drivesNow.filter((d) => d.removable && !d.system).map(toDrive),
  watchDrives(fn) { driveSubs.add(fn); return () => driveSubs.delete(fn); },
  hiddenDriveCount: () => drivesNow.filter((d) => !d.removable || d.system).length,

  cacheDir: () => info_.cache,
  cachePath: (tag, name) => `${info_.cache}${info_.os === 'windows' ? '\\' : '/'}${tag}${info_.os === 'windows' ? '\\' : '/'}${name}`,
  async releases() {
    const text = await call('fetch_releases');
    await cacheStates(text);
    if (options.appUpdates !== false) findUpdate(text);
    return text;
  },
  appUpdate: null,
  async cachedReleases() {
    const text = await invoke('cached_releases');
    if (text) await cacheStates(text);
    return text;
  },
  isCached(tag, asset) { return sizeCache.get(`${tag}/${asset.name}`)?.cached || false; },
  partialSize(tag, asset) { return sizeCache.get(`${tag}/${asset.name}`)?.partial || 0; },
  async refreshCache(tag, asset) { sizeCache.set(`${tag}/${asset.name}`, await invoke('cache_state', { tag, name: asset.name, size: asset.size })); },
  async download(tag, asset, onProgress) {
    progressFn = (p) => { if (p.phase === 'download') onProgress?.(p); };
    try { return await call('download', { tag, name: asset.name, size: asset.size }); } finally { progressFn = null; this.refreshCache(tag, asset).catch(() => {}); }
  },
  cancelDownload: () => invoke('cancel'),
  readText: (path) => call('read_cache_text', { path }),
  checkSums: (sums, sig) => invoke('check_sums', { sumsText: sums, sig }),

  async pickImage() {
    const path = await openDialog({ title: t('imager.image.pickTitle'), multiple: false, directory: false, filters: [{ name: 'LordOS', extensions: ['img', 'xz', 'iso'] }] });
    if (!path) return null;
    return call('image_info', { path: String(path) });
  },
  imageInfo: null,
  async pickKeyFile() {
    const path = await openDialog({ title: t('imager.remote.pickKey'), multiple: false, filters: [{ name: 'OpenSSH', extensions: ['pub'] }] });
    return path ? call('read_key_file', { path: String(path) }) : null;
  },
  Layer: null,

  hashPassword: (password) => call('hash_password', { password }),
  wifiPsk: (ssid, password) => call('wifi_psk', { ssid, password }),

  async write({ drive, image }, onProgress) {
    currentDrive = drive.model;
    progressFn = (p) => { if (p.phase !== 'download') onProgress?.(p); };
    try {
      const r = await call('write_image', { drive: drive.raw, path: image.path, official: !!image.official, sumsPath: image.sums || null, sigPath: image.sig || null });
      return { sha256: r.sha256, mbps: r.mbps, ms: r.ms };
    } finally { progressFn = null; }
  },
  cancelWrite: () => invoke('cancel'),
  writeFirstRun: (drive, text, name) => call('write_firstrun', { drive: drive.raw, text, name }),
  eject: (drive) => call('eject', { drive: drive.raw }),

  defaults: () => ({ lang: info_.lang, timezone: zonesData.zones.includes(info_.timezone) ? info_.timezone : '', keyboard: '', variant: '', country: info_.country }),
  zones: () => zonesData.zones,
  layouts: () => layoutsData.layouts,
  scanWifi: null,

  loadOptions: () => options,
  saveOptions(o) { options = { ...options, ...o }; invoke('save_options', { options: o }).catch(() => {}); },
  loadProfiles: () => profilesCache,
  saveProfiles(list) { profilesCache = list; invoke('save_profiles', { profiles: list }).catch(() => {}); },
  cacheSize: () => cacheSizeNow,
  async clearCache() { await call('clear_cache'); cacheSizeNow = await invoke('cache_size'); },
  async chooseCacheDir() {
    const dir = await openDialog({ directory: true, multiple: false });
    if (!dir) return;
    this.saveOptions({ cacheDir: String(dir) });
    info_.cache = String(dir);
    cacheSizeNow = await invoke('cache_size');
  },
  setLanguage: (lang) => setLanguage(lang || info_.lang),
  setTheme: (th) => appearance.set(th || 'auto'),
  followsSystem: false,
  appUpdates: true,

  done({ sound: withSound }) { if (withSound) chime(); },
};

let profilesCache = [];
let cacheSizeNow = 0;

// "Check for app updates": a newer LordOS Imager among the release files GitHub lists (no other request)
const newer = (a, b) => { const x = a.split('.').map(Number), y = b.split('.').map(Number); for (let i = 0; i < 4; i++) if ((x[i] || 0) !== (y[i] || 0)) return (x[i] || 0) > (y[i] || 0); return false; };
function findUpdate(text) {
  try {
    for (const r of JSON.parse(text)) for (const a of r.assets || []) {
      const m = /^(?:lordos-imager|LordOS[ ._-]Imager)[_-]v?(\d+\.\d+\.\d+(?:\.\d+)?)/i.exec(a.name || '');
      if (m && newer(m[1], info_.version) && (!platform.appUpdate || newer(m[1], platform.appUpdate.version))) platform.appUpdate = { version: m[1], tag: r.tag_name };
    }
  } catch { /* not a list */ }
}

export async function warm() {
  profilesCache = (await invoke('load_profiles')) || [];
  cacheSizeNow = await invoke('cache_size');
}

// a short two-note chime (no sound files, no system sounds)
function chime() {
  try {
    const ctx = new (window.AudioContext || window.webkitAudioContext)();
    [[880, 0], [1318.5, 0.14]].forEach(([f, at]) => {
      const o = ctx.createOscillator(), g = ctx.createGain();
      o.type = 'sine'; o.frequency.value = f;
      g.gain.setValueAtTime(0.0001, ctx.currentTime + at);
      g.gain.exponentialRampToValueAtTime(0.25, ctx.currentTime + at + 0.02);
      g.gain.exponentialRampToValueAtTime(0.0001, ctx.currentTime + at + 0.5);
      o.connect(g).connect(ctx.destination);
      o.start(ctx.currentTime + at); o.stop(ctx.currentTime + at + 0.55);
    });
  } catch { /* no audio */ }
}

export { h };
