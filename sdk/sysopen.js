// sysopen.js — opening things for the isolated system apps (docs/PRIVACY.md, "System apps in their
// own process"). Mail, Calendar, Contacts and Reminders run as `axia-server --app <id>` and cannot
// open files or other apps themselves; axia-server checks what they ask (sec/brokermail.c) and the
// session does it here:
//
//   openFileWithApps(path, name)   the same rules as Files: our apps for documents, images and media,
//                                  the default app for the rest, otherwise reveal it in Files. Used by
//                                  Mail in-process and, for the isolated Mail, on a cached attachment
//                                  axia-server validated ("mailopen" broker event).
//   revealPath(path)
//   "launch" broker event          one of these apps opens another one with args (no paths).
import { wm, getApp } from './windows.js';
import * as fs from './fs.js';
import { defaultOpenerId, nativeOf } from './defaults.js';
import { events as calendarEvents } from './calendar.js';
import { books as addressBooks, contacts } from './contacts.js';

export const revealPath = (path) => wm.open('files', { new: true, args: { path: fs.parent(path), select: fs.basename(path), nonce: Date.now() } });

export function openFileWithApps(path, name) {
  const n = String(name || path).toLowerCase();
  if (/\.pdf$/.test(n) && getApp('pdf')) return wm.open('pdf', { new: true, args: { path } });
  if (/\.(png|jpe?g|gif|webp|bmp|heic|avif)$/.test(n) && getApp('photos')) return wm.open('photos', { new: true, args: { path } });
  if (/\.(mp3|ogg|flac|wav|m4a|opus|mp4|mkv|webm|mov|avi)$/.test(n) && getApp('player')) return wm.open('player', { args: { path } });
  if (/\.(txt|md|csv|log|json|xml|ya?ml|c|h|js|py|sh)$/.test(n) && getApp('editor')) return wm.open('editor', { args: { path } });
  if (/\.ics$/.test(n) && getApp('calendar')) { calendarEvents.importFile(path).then(() => wm.open('calendar')).catch(() => revealPath(path)); return true; }
  if (/\.vcf$/.test(n) && getApp('contacts')) {
    addressBooks.list().then((b) => (b[0] ? contacts.importFile(path, b[0].id) : null)).then(() => wm.open('contacts')).catch(() => revealPath(path));
    return true;
  }
  try {
    const def = defaultOpenerId(typeof lord !== 'undefined' && lord.mime ? lord.mime.guess(n) : '');
    const native = nativeOf(def);
    if (native && getApp(native)) return wm.open(native, { new: true, args: { path } });
    if (def && lord.apps.launchWith(def.replace(/\.desktop$/, ''), path)) return true;
    if (lord.apps.launchWith('', path)) return true;
  } catch (e) { /* no default app */ }
  return revealPath(path);
}

// the session side (privacy.js forwards these broker events here)
const SYSTEM_APPS = ['mail', 'calendar', 'contacts', 'reminders'];
if (typeof globalThis !== 'undefined') {
  globalThis.__lordSysOpen = (kind, data) => {
    if (!data) return;
    if (kind === 'mailopen' && typeof data.path === 'string') openFileWithApps(data.path, data.name);
    else if (kind === 'launch' && SYSTEM_APPS.includes(data.app) && getApp(data.app)) {
      const args = data.args && typeof data.args === 'object' ? data.args : {};
      wm.open(data.app, data.new ? { new: true, args } : { args });
    }
  };
}
