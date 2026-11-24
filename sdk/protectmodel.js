// protectmodel.js — protected windows' plain logic (sdk/windows.js, sdk/windowshell.jsx; server/tests/protect_model_test.mjs).
//
// A protected window looks normal on the screen and is an opaque neutral fill with a lock in every capture: screenshots,
// recordings, the remote desktop, screen sharing, portal stills, thumbnails that leave the compositor (docs/PRIVACY.md
// "Protected windows"). lord-server does the capture side (Node.protect, paint_protected_fill); this decides which
// windows carry the flag.
//
//   - wm.open(app, { protected: true }), or a native app whose manifest says "protected": true, opens the window
//     protected from its first frame, and locked: wm.setProtected(id, false) can't turn it off afterwards (a password
//     app, a machine with the anonymity profile and its apps-mode windows). The flag is never taken from a Linux app:
//     no Wayland or X11 protocol asks for it, so a client can't add or remove it (sdk/wayland.js protects only a
//     built-in isolated app whose manifest asks for it).
//   - wm.setProtected(id, on) turns it on or off on any other window (Screenshots' "Protect the front window", System
//     Information while it shows serial numbers).

// The window's fields when it opens: { protected, protectLocked } when it is protected, else null.
// opts: wm.open's options; app: the registry's entry (its manifest).
export function protectOnOpen(opts, app) {
  return opts?.protected === true || app?.protected === true ? { protected: true, protectLocked: true } : null;
}

// What wm.open does to a window that is already open: it can only become protected (never less), and locked.
export function protectOnReopen(win, opts, app) {
  const p = protectOnOpen(opts, app);
  return p && !(win?.protected && win?.protectLocked) ? p : null;
}

// wm.setProtected(id, on, { lock }): the patch for win, or null when nothing changes. A locked window stays protected;
// lock (with on) locks it too, as opening it protected does (a machine's window opened before it knew its profile).
export function protectPatch(win, on, lock = false) {
  if (!win) return null;
  const want = !!on;
  if (!want && win.protectLocked) return null;
  if (want && lock && !win.protectLocked) return { protected: true, protectLocked: true };
  return !!win.protected === want ? null : { protected: want };
}

// The label lord-server draws under the lock (lord.media.setProtectedLabel): printable text, at most 95 bytes of UTF-8,
// cut on a character boundary (as paint_set_protected_label keeps it).
export const PROTECT_LABEL_MAX = 95;
export function protectLabel(text) {
  const clean = String(text ?? '').replace(/[\u0000-\u001f\u007f]/g, '');
  let out = '', bytes = 0;
  for (const ch of clean) {
    const cp = ch.codePointAt(0);
    const n = cp < 0x80 ? 1 : cp < 0x800 ? 2 : cp < 0x10000 ? 3 : 4;
    if (bytes + n > PROTECT_LABEL_MAX) break;
    out += ch;
    bytes += n;
  }
  return out;
}
