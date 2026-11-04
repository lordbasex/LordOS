// state.js — the LordOS Imager's wizard state and the write pipeline. Shared by the native and the
// desktop app; only platform.js differs. The secrets (password, Wi-Fi password) live in memory only,
// in `secrets`, and are dropped once they are derived (docs/IMAGER.md, "Secrets").
import { createStore } from '@lordos/sdk';
import { platform } from './platform.js';
import { defaultConfig, deviceById } from './core/catalog.js';
import { parseReleases } from './core/releases.js';
import * as firstrun from './core/firstrun.js';

export const STEPS = ['device', 'image', 'storage', 'customise', 'write'];

const OPTION_DEFAULTS = { sound: true, eject: true, appUpdates: true, theme: 'auto', lang: '', showOld: false };

function initial() {
  const d = platform.defaults();
  const cfg = { ...defaultConfig(), lang: d.lang, timezone: d.timezone, keyboard: d.keyboard, variant: d.variant, wifiCountry: d.country };
  return {
    step: 'device', device: 'rpi5', devView: 'kind',   // devView: the kinds of device, or 'pi' (which Raspberry Pi)
    releases: { state: 'idle', list: [], error: null },
    image: null,                     // { official, release?, name, size, raw, path?, sums?, sig? }
    drive: null,
    customise: true, cfg, tab: 'general',
    secrets: { password: '', confirm: '', wifiPassword: '' },
    showErrors: false,
    job: null,                       // the write pipeline (see run())
    options: { ...OPTION_DEFAULTS, ...platform.loadOptions() },
    dialog: null,                    // 'options' | 'confirm' | 'profiles' | 'cancel'
  };
}

export const app = createStore(initial());
export const set = (p) => app.set((s) => ({ ...s, ...(typeof p === 'function' ? p(s) : p) }));
export const setCfg = (p) => app.set((s) => ({ ...s, cfg: { ...s.cfg, ...p } }));
export const setSecrets = (p) => app.set((s) => ({ ...s, secrets: { ...s.secrets, ...p } }));
export const setOptions = (p) => { platform.saveOptions(p); app.set((s) => ({ ...s, options: { ...s.options, ...p } })); };
export function restart({ keepCfg = true } = {}) {
  const s = app.get();
  const fresh = initial();
  app.set({ ...fresh, device: s.device, releases: s.releases, image: s.image, cfg: keepCfg ? s.cfg : fresh.cfg, step: 'storage' });
}

// ---- releases ----

export async function loadReleases() {
  const s = app.get();
  if (s.releases.state === 'loading') return;
  set({ releases: { ...s.releases, state: 'loading', error: null } });
  try {
    const text = await platform.releases();
    const list = parseReleases(text, app.get().device);
    // the latest is chosen unless the user already chose something
    set((st) => ({ releases: { state: 'ok', list, error: null, all: text }, image: st.image || (list[0] ? officialImage(list[0]) : null) }));
  } catch (e) {
    // offline: what was fetched before still works (the signature is checked again before writing)
    const cached = await platform.cachedReleases();
    const list = cached ? parseReleases(cached, app.get().device) : [];
    set({ releases: { state: list.length ? 'stale' : 'error', list, error: e.message || String(e), all: cached } });
  }
}

export function chooseDevice(id) {
  const s = app.get();
  if (s.device === id) return;
  const text = s.releases.all;
  const from = deviceById(s.device), to = deviceById(id);
  // two Raspberry Pi models share the image: keep the one chosen
  const same = from && to && String(from.image) === String(to.image);
  // a board without a fan (the Pi 500) cools passively: no fan, case or overclock to pick
  const cool = to && to.fan === false ? { cooler: 'passive', performance: 'balanced', overclock: false } : from && from.fan === false && s.cfg.cooler === 'passive' ? { cooler: '', performance: '', overclock: false } : {};
  set({ device: id, ...(same ? {} : { image: null, releases: { ...s.releases, list: text ? parseReleases(text, id) : [] } }), cfg: { ...s.cfg, ...cool } });
}

export function officialImage(r) {
  return { official: true, release: r, name: r.image.name, size: r.image.size, raw: null, tag: r.tag };
}

// ---- the pipeline: download → check the signature → derive secrets → write → verify → first run → eject ----

const now = () => Date.now();

function setJob(p) { app.set((s) => ({ ...s, job: s.job ? { ...s.job, ...p } : p })); }

// speed and time left from the last samples of one phase (bytes per second, smoothed)
function progress(phase, done, total) {
  const j = app.get().job || {};
  const t = now();
  let { speed = 0, last } = j.phase === phase ? j : {};
  if (last && t > last.t) {
    const inst = ((done - last.done) * 1000) / (t - last.t);
    if (inst >= 0) speed = speed ? speed * 0.8 + inst * 0.2 : inst;
  }
  const eta = speed > 0 && total > done ? (total - done) / speed : null;
  setJob({ phase, done, total, speed, eta, last: { t, done }, ...(phase === 'write' && done > 0 ? { wrote: true } : {}) });
}

export async function run() {
  const s = app.get();
  const dev = deviceById(s.device);
  const img = s.image;
  const drive = s.drive;
  const custom = s.customise && dev.customise;
  app.set((st) => ({ ...st, dialog: null, job: { phase: 'prepare', started: now(), done: 0, total: 0, phases: [], cancel: false, error: null, result: null } }));
  const mark = (p) => app.set((st) => ({ ...st, job: { ...st.job, phases: [...(st.job.phases || []), p] } }));
  const cancelled = () => app.get().job?.cancel;
  try {
    let image = { ...img };
    if (img.official) {
      const r = img.release;
      // the two small files first, then the signature, then (only if it verifies) the image
      setJob({ phase: 'signature' });
      const sumsPath = await platform.download(r.tag, r.sums);
      const sigPath = await platform.download(r.tag, r.sig);
      const check = await platform.checkSums(await platform.readText(sumsPath), await platform.readText(sigPath));
      if (!check || !check.ok) throw Object.assign(new Error('imager.err.badSignature'), { code: 'imager.badSignature' });
      if (!check.files[r.image.name]) throw Object.assign(new Error('imager.err.notListed'), { code: 'imager.notListed' });
      mark('signature');
      if (cancelled()) throw Object.assign(new Error('cancel'), { code: 'cancelled' });
      if (!platform.isCached(r.tag, r.image)) {
        setJob({ phase: 'download', done: platform.partialSize(r.tag, r.image), total: r.image.size });
        await platform.download(r.tag, r.image, (p) => progress('download', p.done, r.image.size));
      }
      mark('download');
      image = { ...image, path: platform.cachePath(r.tag, r.image.name), sums: sumsPath, sig: sigPath, sha256: check.files[r.image.name] };
    }
    if (cancelled()) throw Object.assign(new Error('cancel'), { code: 'cancelled' });
    // the secrets become a hash and a key now; the plain ones are forgotten
    let text = null;
    if (custom) {
      setJob({ phase: 'secrets' });
      const sec = app.get().secrets;
      const passwordHash = s.cfg.user.name ? await platform.hashPassword(sec.password) : null;
      if (s.cfg.user.name && !passwordHash) throw Object.assign(new Error('imager.err.hashFailed'), { code: 'imager.hashFailed' });
      const psk = s.cfg.wifi.enabled && !s.cfg.wifi.open ? await platform.wifiPsk(s.cfg.wifi.ssid, sec.wifiPassword) : null;
      if (s.cfg.wifi.enabled && !s.cfg.wifi.open && !psk) throw Object.assign(new Error('imager.err.wifiPassLength'), { code: 'imager.pskFailed' });
      text = firstrun.build(s.cfg, { passwordHash, psk });
      app.set((st) => ({ ...st, secrets: { password: '', confirm: '', wifiPassword: '' } }));
    }
    setJob({ phase: 'write', done: 0, total: 0, speed: 0, eta: null, last: null });
    const result = await platform.write({ drive, image }, (p) => progress(p.phase, p.done, p.total));
    mark('write');
    mark('verify');
    if (custom) {
      setJob({ phase: 'firstrun' });
      await platform.writeFirstRun(drive, text, firstrun.FIRSTRUN_NAME);
      mark('firstrun');
    }
    let ejected = false;
    if (app.get().options.eject) {
      setJob({ phase: 'eject' });
      try { await platform.eject(drive); ejected = true; } catch { /* the card can still be removed after unmounting */ }
      mark('eject');
    }
    const j = app.get().job;
    setJob({ phase: 'done', result: { ...result, ejected, custom, seconds: Math.round((now() - j.started) / 1000) } });
    set({ step: 'done' });
    return true;
  } catch (e) {
    const code = e.code || '';
    setJob({ phase: 'failed', error: { code, message: code === 'cancelled' || /cancel/i.test(code) ? null : (e.message || String(e)) }, cancelledByUser: !!app.get().job?.cancel });
    return false;
  }
}

export async function cancel() {
  const s = app.get();
  if (!s.job) return;
  setJob({ cancel: true });
  if (s.job.phase === 'download' || s.job.phase === 'signature') await platform.cancelDownload();
  else if (['write', 'check', 'verify', 'prepare'].includes(s.job.phase)) await platform.cancelWrite(s.drive);
}

export const isBusy = (job) => !!job && !['done', 'failed'].includes(job.phase);
