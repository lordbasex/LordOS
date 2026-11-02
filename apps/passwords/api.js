// api.js — estado de la app Contraseñas y los pedidos a axia-vault (por @lordos/sdk vaultRequest).
// Lo descifrado vive en axia-vault; acá llega solo lo que se muestra: la lista liviana (sin
// secretos), la entrada abierta (los ocultos sin valor) y un valor cuando el usuario lo pide
// (mostrar, copiar, editar). Ver docs/PASSWORDS.md.
import { createStore, vaultRequest, onVaultEvent, clipboard, t } from '@lordos/sdk';

export const APP_ID = 'passwords';

// true when this UI runs as its own sandboxed process (axia-server --app passwords) with a direct
// channel to axia-vault; false in the shell (in-process fallback). See docs/PASSWORDS.md.
export const appMode = typeof lord !== 'undefined' && !!(lord.vault && lord.vault.appMode);

export const app = createStore({
  phase: 'loading',          // loading | none | locked | unlocked | unavailable
  status: null,              // respuesta de "status"
  meta: {}, cats: [], tags: [], entries: [],
  login: { entries: [], unlocked: false },
  toast: null,
  touch: false,              // la llave FIDO2 espera un toque
});

export const req = (op, args, opts) => vaultRequest(op, args, opts);

export async function refreshStatus() {
  try {
    const st = await req('status');
    app.set((s) => ({ ...s, status: st, phase: st.main.unlocked ? 'unlocked' : st.main.exists ? 'locked' : st.main.unreadable ? 'locked' : 'none' }));
    if (st.main.unlocked) await reload();
    return st;
  } catch (e) {
    app.set((s) => ({ ...s, phase: 'unavailable', error: e.code || e.message }));
    return null;
  }
}

export async function reload() {
  const r = await req('list');
  app.set((s) => ({ ...s, phase: 'unlocked', meta: r.meta || {}, cats: r.cats || [], tags: r.tags || [], entries: r.entries || [] }));
  reloadLogin();
}

export async function reloadLogin() {
  try {
    const r = await req('list', { v: 'login' });
    app.set((s) => ({ ...s, login: { entries: r.entries || [], unlocked: true } }));
  } catch (e) { app.set((s) => ({ ...s, login: { entries: [], unlocked: false } })); }
}

// reemplaza/agrega un ítem de la lista con lo que devolvió axia-vault
export function upsertItem(item) {
  if (!item) return;
  app.set((s) => {
    const i = s.entries.findIndex((e) => e.id === item.id);
    const entries = i >= 0 ? s.entries.map((e, k) => (k === i ? item : e)) : [...s.entries, item];
    return { ...s, entries };
  });
}

export function dropItem(id) { app.set((s) => ({ ...s, entries: s.entries.filter((e) => e.id !== id) })); }

let toastTimer = null;
export function toast(text, kind = 'info') {
  if (toastTimer) clearTimeout(toastTimer);
  app.set((s) => ({ ...s, toast: { text, kind } }));
  toastTimer = setTimeout(() => app.set((s) => ({ ...s, toast: null })), 2600);
}

// Copiar un secreto: marcado sensible (no entra al historial del portapapeles ni a los gestores
// de portapapeles de las apps Linux) y se borra solo a los N segundos si sigue siendo el mismo.
let clearTimer = null;
export function copySecret(value, label) {
  const secs = Number(app.get().meta?.clip ?? 30) || 30;
  if (appMode) {
    // app process: axia-server marks it sensitive and clears it after `secs` if unchanged
    lord.clipboard.set(value, { secure: true, clearAfter: secs });
    toast(t('passwords.copied', { what: label, secs }));
    return;
  }
  clipboard.set(value, { secure: true, origin: APP_ID });
  if (clearTimer) clearTimeout(clearTimer);
  clearTimer = setTimeout(() => {
    clearTimer = null;
    try { if (lord.clipboard.get() === value) clipboard.set('', { secure: true, origin: APP_ID }); } catch (e) { /* */ }
  }, secs * 1000);
  toast(t('passwords.copied', { what: label, secs }));
}

export function copyText(value, label) {
  clipboard.set(value, { origin: APP_ID });
  toast(t('passwords.copiedPlain', { what: label }));
}

export async function revealField(v, id, k, i = 0) {
  const r = await req('reveal', { v, id, k, i });
  return r.value;
}

export async function setMeta(patch) {
  const meta = { ...app.get().meta, ...patch };
  await req('set-meta', { meta });
  app.set((s) => ({ ...s, meta }));
}

export async function setCats(cats) {
  await req('set-cats', { cats });
  app.set((s) => ({ ...s, cats }));
}

export async function setTags(tags) {
  await req('set-tags', { tags });
  app.set((s) => ({ ...s, tags }));
}

// Eventos de axia-vault: bloqueo (automático, pantalla), cambios desde otro lado, toque de la llave
let wired = false;
export function wireEvents() {
  if (wired) return;
  wired = true;
  onVaultEvent((ev) => {
    if (ev.ev === 'locked' && ev.v === 'main') app.set((s) => ({ ...s, phase: s.phase === 'unlocked' ? 'locked' : s.phase, entries: [], touch: false }));
    else if (ev.ev === 'changed' && ev.v === 'login' && app.get().phase === 'unlocked') reloadLogin();
    else if (ev.ev === 'touch') app.set((s) => ({ ...s, touch: true }));
    else if (ev.ev === 'crash') app.set((s) => ({ ...s, phase: 'loading' }));
  });
}

// Mensaje legible para un código de error de axia-vault
export function errorText(code) {
  const known = ['bad-key', 'locked', 'not-found', 'corrupt', 'truncated', 'not-a-vault', 'unsupported-version', 'no-device', 'keyfile-read',
    'keyfile-short', 'keyfile-write', 'unknown-format', 'bad-password', 'encrypted-export', 'unsupported-cipher', 'unsupported-kdf',
    'too-big', 'no-qr', 'not-an-image', 'last-slot', 'exists', 'busy', 'file', 'fido-timeout', 'fido-pin-required', 'fido-pin-invalid',
    'fido-pin-blocked', 'fido-no-credential', 'fido-no-hmac', 'fido-denied', 'fido-restricted', 'unavailable', 'bad-keyfile'];
  return known.includes(code) ? t(`passwords.err.${code}`) : t('passwords.err.other', { code: String(code || '?') });
}
