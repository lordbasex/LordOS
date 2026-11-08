// voice.js — reading text aloud (docs/VOICE.md, phase 2): lord.voice starts lord-say, LordOS's `say`.
//
//   speak(text)              reads it, in the language it is written in (lord-say tells); ends any reading before
//   stopSpeaking()
//   pauseSpeaking(on)        freezes the reading where it is, or goes on
//   replaySpeaking()         reads the last text again from the start
//   closePlayer()            stops and hides the player
//   saveSpeech(text)         asks where (the save dialog, VoiceSavePrompt) and makes a WAV there; nothing is heard
//   speaking                 a store: true while reading or saving
//   player                   a store: what the player shows ({ state: 'playing'|'paused'|'done', source }), or null
//   speakSelection()         ❖Esc (sdk/shortcuts.js): reads what the window in front has selected, or stops reading
//   registerSelection(wid, fn)   a LordOS app's window says what it has selected (fn() → text); returns the undo
//   speechItems(getText)     [speechMenu(getText), '-'], or [] when it is off: to spread into a menu's items
//   speechMenu(getText)      the Speech submenu (Start Speaking, Stop Speaking, Save as Audio…) for an Edit menu or a
//                            context menu; null when reading aloud is off in Settings → AI
//   useMenusWithSpeech(wid)  an app's menus, and for a Linux app's window an Edit menu with Speech (its own selection)
//   voiceSettings()          Settings → AI → Voice: { enabled, shortcut, player }, all on by default
//
// A Linux app's selection is its primary selection: lord-server reads it (lord.voice.speakSelection, holdSelection) and
// only when that app made it, so JS never sees it. When nothing can be read, a short notice says why (toast).
import { createStore, useStore } from './store.js';
import { t, locale } from './i18n.js';
import { wm, useAppMenus } from './windows.js';
import { toast } from './clipboard.js';
import { settings } from './settings.js';
import { windowAppName } from './apps.js';
import { userDirs } from './media.js';

const api = () => (typeof lord !== 'undefined' && lord.voice) || null;
// for a text too short to tell its language (say_language in server/src/tts/saytext.c): the interface's
const uiLang = () => String(locale.get().lang || '').slice(0, 2);
const isLinux = (wid) => String(wid || '').startsWith('linux#');
const providers = new Map();          // window id → () => selected text
export const speaking = createStore(false);
export const player = createStore(null);
export const voiceSave = createStore(null);   // the save dialog: { text } or { held: true } (a Linux app's selection)

export const VOICE_DEFAULTS = { enabled: true, shortcut: true, player: true };
export const voiceSettings = () => ({ ...VOICE_DEFAULTS, ...(settings.get().voice || {}) });

// say.c's exit statuses: what to tell the person
const EXIT_TEXT = { 3: 'shell.voice.noModel', 4: 'shell.voice.noVoice', 5: 'shell.voice.notInstalled' };
const DONE_MS = 4000;                  // the player stays this long after the end, then goes

let timer = null, doneTimer = null, savingTo = '';
function watch() {
  clearInterval(timer);
  clearTimeout(doneTimer);
  speaking.set(true);
  timer = setInterval(() => {
    const s = api()?.state();
    if (s && s.speaking) {
      if (player.get() && !s.saving) player.set({ state: s.paused ? 'paused' : 'playing' });
      return;
    }
    clearInterval(timer);
    timer = null;
    speaking.set(false);
    const key = s && EXIT_TEXT[s.exit];
    if (key) toast.set(t(key));
    else if (savingTo && s && s.exit === 0) toast.set(t('shell.voice.saved', { name: savingTo.split('/').pop() }));
    else if (s && s.exit > 0 && s.exit !== 130) toast.set(t(savingTo ? 'shell.voice.saveFailed' : 'shell.voice.failed'));
    savingTo = '';
    if (player.get()) {
      if (s && s.exit === 0) { player.set({ state: 'done' }); doneTimer = setTimeout(() => player.set(null), DONE_MS); }
      else player.set(null);
    }
  }, 300);
}

function started(source) {
  watch();
  if (!voiceSettings().player) return;
  player.set(null);                     // a new one: not merged with the last
  player.set({ state: 'playing', source });
}

const sourceName = () => { const w = wm.get(wm.focused()); return w ? windowAppName(w) : ''; };

export function speakAvailable() { return !!api()?.available(); }

export function speak(text) {
  const v = api();
  const s = String(text || '').trim();
  if (!s) return false;
  if (!v || !v.available()) { toast.set(t('shell.voice.notInstalled')); return false; }
  const ok = v.speak(s, uiLang());
  if (ok) started(sourceName());
  else toast.set(t('shell.voice.failed'));
  return ok;
}

export function stopSpeaking() {
  api()?.stop();
  clearInterval(timer);
  timer = null;
  savingTo = '';
  speaking.set(false);
  if (player.get()) {
    player.set({ state: 'done' });
    clearTimeout(doneTimer);
    doneTimer = setTimeout(() => player.set(null), DONE_MS);
  }
}

export function closePlayer() {
  stopSpeaking();
  clearTimeout(doneTimer);
  player.set(null);
}

export function pauseSpeaking(on) {
  if (api()?.pause(!!on)) player.set({ state: on ? 'paused' : 'playing' });
}

export function replaySpeaking() {
  const v = api();
  if (v && v.replay()) started(player.get()?.source || '');
}

export function registerSelection(wid, fn) {
  providers.set(wid, fn);
  return () => { if (providers.get(wid) === fn) providers.delete(wid); };
}

// what a LordOS window has selected; undefined for a Linux app's (lord-server reads it)
function selectionOf(wid) {
  const fn = wid && providers.get(wid);
  if (!fn) return undefined;
  try { return String(fn() || ''); } catch (e) { console.error('voice:', e); return ''; }
}

function speakLinuxSelection() {
  const v = api();
  if (!v) return;
  if (!v.available()) { toast.set(t('shell.voice.notInstalled')); return; }
  if (v.speakSelection(uiLang())) started(sourceName());
  else toast.set(t('shell.voice.nothingSelected'));
}

export function speakSelection() {
  const v = api();
  const vs = voiceSettings();
  if (!v || !vs.enabled || !vs.shortcut) return;
  if (speaking.get() || v.state().speaking) { stopSpeaking(); return; }
  const wid = wm.focused();
  const text = selectionOf(wid);
  if (text === undefined) { speakLinuxSelection(); return; }
  if (text.trim()) speak(text);
  else toast.set(t('shell.voice.nothingSelected'));
}

// ---- saving as a sound file (as macOS's Add to Music as a Spoken Track) ----

// a file name from the first words: letters, digits and spaces, at most 40 characters
export function speechFileName(text) {
  const words = String(text || '').replace(/[^\p{L}\p{N}\s]+/gu, ' ').trim().split(/\s+/).filter(Boolean);
  let name = '';
  for (const w of words) { if ((name + ' ' + w).trim().length > 40) break; name = (name + ' ' + w).trim(); }
  return `${name || t('shell.voice.fileName')}.wav`;
}

export const musicFolder = () => { try { return userDirs().music; } catch (e) { return ''; } };

export function saveSpeech(text) {
  const v = api();
  if (!v) return;
  if (!v.available()) { toast.set(t('shell.voice.notInstalled')); return; }
  const s = String(text || '').trim();
  if (!s) { toast.set(t('shell.voice.nothingSelected')); return; }
  voiceSave.set(null);
  voiceSave.set({ text: s, name: speechFileName(s) });
}

function saveLinuxSelection() {
  const v = api();
  if (!v) return;
  if (!v.available()) { toast.set(t('shell.voice.notInstalled')); return; }
  if (!v.holdSelection()) { toast.set(t('shell.voice.nothingSelected')); return; }
  voiceSave.set(null);
  voiceSave.set({ held: true, name: speechFileName('') });
}

// the dialog's answer: the path, or null when it was cancelled
export function saveSpeechTo(path) {
  const req = voiceSave.get();
  voiceSave.set(null);
  const v = api();
  if (!req || !v) return;
  if (!path) { if (req.held) v.saveSelection(''); return; }     // forgets the selection it kept
  const to = /\.wav$/i.test(path) ? path : `${path}.wav`;
  const ok = req.held ? v.saveSelection(to, uiLang()) : v.save(req.text, to, uiLang());
  if (!ok) { toast.set(t('shell.voice.saveFailed')); return; }
  savingTo = to;
  watch();
  toast.set(t('shell.voice.saving'));
}

// ---- the Speech submenu ----

// getText: () => the text this menu reads (a selection, or all of it); null for a Linux app's own selection
export function speechMenu(getText) {
  if (!voiceSettings().enabled) return null;
  // both always on: an app builds its menus when its own state changes, not when a reading starts or ends
  const run = (linux, other) => () => (getText ? other(getText()) : linux());
  return {
    label: t('shell.voice.speech'),
    submenu: [
      { label: t('shell.voice.start'), hint: voiceSettings().shortcut ? '❖Esc' : undefined, onClick: run(speakLinuxSelection, (s) => (String(s || '').trim() ? speak(s) : toast.set(t('shell.voice.nothingSelected')))) },
      { label: t('shell.voice.stop'), onClick: stopSpeaking },
      '-',
      { label: t('shell.voice.saveAs'), onClick: run(saveLinuxSelection, saveSpeech) },
    ],
  };
}

// the Speech submenu and a separator after it, for a menu's list; nothing when reading aloud is off
export function speechItems(getText) {
  const m = speechMenu(getText);
  return m ? [m, '-'] : [];
}

// a window's menus; a Linux app's gets an Edit menu with Speech, as its own menus aren't the desktop's
export function useMenusWithSpeech(wid) {
  const menus = useAppMenus(wid);
  useStore(settings, (s) => s.voice);    // Settings → AI turns it on and off
  if (!isLinux(wid)) return menus;
  const sp = speechMenu(null);
  if (!sp) return menus;
  return [...(menus || []), { label: t('shell.menu.edit'), items: [sp] }];
}
