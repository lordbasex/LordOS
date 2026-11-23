// cameras.js — the list of cameras, shared by the Camera app and LordOS Studio (2026-11-23, docs/MAIN-THREAD.md).
//
// lord.media.cameras() opens every /dev/video* and asks each one what it is: 60–70 ms with two webcams (a USB camera
// wakes up to answer), on lord-server's main thread. The Camera app called it every 3 s and LordOS Studio's panel on
// every render. Here the list is made on one of lord-server's workers (lord.media.camerasAsync) when a picker shows,
// and again when lord-server says a video device came or went (__lordHw('cameras') from the kernel's uevents,
// sdk/hardware.jsx): nothing polls. The kernel announces a device before udev gives the user access to it, so a change
// is listed twice (after 0.3 s and 2 s). Without camerasAsync (an isolated app, whose calls go through the broker) the
// synchronous call is used, after the current render (a promise job).
import { useEffect } from 'react';
import { createStore, useStore } from './store.js';
import { workerReply } from './fs.js';
import { media } from './media.js';

const list = createStore(null);          // [{ device, name, bus, studio? }], null until first listed
let asking = null, again = false, timers = [];

const same = (a, b) => JSON.stringify(a) === JSON.stringify(b);
function ask() {
  const m = typeof lord !== 'undefined' ? lord.media : null;
  let req = null;
  if (m && typeof m.camerasAsync === 'function') {
    try { req = m.camerasAsync(); } catch (e) { return Promise.reject(e); }
  }
  // a request id; anything else (a stand-in without it) falls back to the synchronous list
  if (typeof req === 'number' && req > 0) return workerReply(req).then((r) => (Array.isArray(r.cameras) ? r.cameras : []));
  return Promise.resolve().then(() => media.cameras());
}

// Lists the cameras again; a Promise of the list. Asked while a listing runs: once more after it.
export function refreshCameras() {
  if (asking) { again = true; return asking; }
  asking = ask().catch(() => list.get() || []).then((cams) => {
    asking = null;
    if (!same(cams, list.get())) list.set(cams);
    if (again) { again = false; refreshCameras(); }
    return list.get();
  });
  return asking;
}

// From sdk/hardware.jsx: a video4linux device was added or removed.
export function camerasChanged() {
  for (const id of timers) clearTimeout(id);
  timers = [300, 2000].map((ms) => setTimeout(refreshCameras, ms));
}

// The last list (null until listed; the first call starts the listing). For code outside React.
export function camerasNow() {
  if (list.get() === null) refreshCameras();
  return list.get();
}
export const onCameras = (fn) => list.subscribe(fn);

// The cameras, for a component: null until the first answer. Listed again (on a worker) each time one mounts, so a
// session without uevents still sees a camera plugged in since the last picker.
export function useCameras() {
  const cams = useStore(list);
  useEffect(() => { refreshCameras(); }, []);
  return cams;
}
