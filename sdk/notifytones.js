// notifytones.js — which of LordOS's sounds a notification makes (Settings → Notifications → an app → Sound), as plain
// data for node (server/tests/notify_tones_test.mjs). The sounds are synthesized by lord-server (server/src/sound/snd.c,
// lord.sound.play): "notify" is the default; "pop", "chime", "ping" and "drop" can be chosen per app.
//
// The per-app setting is settings.notifications.apps[app].sound: true (the default sound, as before 0.8.8.8), false
// (none) or the name of one of the sounds. Older files only have true and false, which keep their meaning.

export const DEFAULT_TONE = 'notify';
export const NOTIFY_TONES = ['pop', 'chime', 'ping', 'drop'];
// What the Sound menu offers, in order: 'default', 'none', then each sound.
export const TONE_CHOICES = ['default', 'none', ...NOTIFY_TONES];

// The sound to play for a setting, or null for none.
export const toneOf = (pref) => (pref === false ? null : NOTIFY_TONES.includes(pref) ? pref : DEFAULT_TONE);
// The menu's choice for a setting.
export const toneChoice = (pref) => (pref === false ? 'none' : NOTIFY_TONES.includes(pref) ? pref : 'default');
// The setting a choice stores.
export const prefOfChoice = (c) => (c === 'none' ? false : NOTIFY_TONES.includes(c) ? c : true);
