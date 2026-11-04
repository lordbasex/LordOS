// catalog.js — what the LordOS Imager offers: devices, cooling, appearance and the defaults. Data
// only (labels are i18n keys); shared by the native and desktop apps. docs/IMAGER.md.

// Devices. Only the Raspberry Pi 5 is supported (docs/RASPBERRY.md); the others are listed so
// people see why their board isn't there. `pc` and `pc-arm64` write a live ISO to a USB stick: the
// amd64 one, and the arm64 one for generic UEFI arm64 machines (no first-boot settings on either).
export const DEVICES = [
  { id: 'rpi5', name: 'Raspberry Pi 5', kind: 'pi', supported: true, image: /^lordos-[0-9][\w.~+-]*-rpi5-arm64\.img\.xz$/, customise: true },
  { id: 'pc', kind: 'pc', supported: true, image: /^lordos-live-[0-9][\w.~+-]*-amd64\.iso$/, customise: false },
  { id: 'pc-arm64', kind: 'pc', supported: true, image: /^lordos-live-[0-9][\w.~+-]*-arm64\.iso$/, customise: false },
  { id: 'rpi500', name: 'Raspberry Pi 500 / CM5', kind: 'pi', supported: false },
  { id: 'rpi4', name: 'Raspberry Pi 4 / 400', kind: 'pi', supported: false },
  { id: 'rpi3', name: 'Raspberry Pi 3 / Zero 2', kind: 'pi', supported: false, never: true },
];
export const deviceById = (id) => DEVICES.find((d) => d.id === id) || null;
// The name to show: a board's own, or the i18n key of a live ISO's card
export const deviceLabel = (d, tr) => (d && d.name) || tr(`imager.device.${d && d.id === 'pc-arm64' ? 'pcArm64' : 'pc'}`);

// RAM advice for the Pi 5 (docs/RASPBERRY.md, "Recommended equipment")
export const RAM_ADVICE = [
  { gb: 4, level: 'min' }, { gb: 8, level: 'comfortable' }, { gb: 16, level: 'max' },
];

// The languages a LordOS image can start in (lordos-firstrun's lang=). English, Español and
// Português are shown first; the others after them. since: the first image version whose first-run
// program knows the language (an older one would refuse the whole file).
export const LANGUAGES = [
  { id: 'en', name: 'English', english: 'English', locale: 'en_US.UTF-8', core: true },
  { id: 'es', name: 'Español', english: 'Spanish', locale: 'es_ES.UTF-8', core: true },
  { id: 'pt', name: 'Português', english: 'Portuguese', locale: 'pt_BR.UTF-8', core: true },
  { id: 'fr', name: 'Français', english: 'French', locale: 'fr_FR.UTF-8', since: '0.8.8' },
  { id: 'it', name: 'Italiano', english: 'Italian', locale: 'it_IT.UTF-8', since: '0.8.9' },
];

const vparts = (v) => String(v || '').replace(/^v/, '').split('-')[0].split('.').map((x) => Number(x) || 0);
export function versionAtLeast(v, min) {
  const a = vparts(v), b = vparts(min);
  for (let i = 0; i < Math.max(a.length, b.length); i++) if ((a[i] || 0) !== (b[i] || 0)) return (a[i] || 0) > (b[i] || 0);
  return true;
}
// Can this image start in language l? An image file of unknown version only gets the core three.
export function langOnImage(l, image) {
  if (!l || !l.since) return true;
  return !!(image?.official && image.release && versionAtLeast(image.release.version, l.since));
}

// Coolers and what each one allows (lordos-firstrun checks the same in C)
export const COOLERS = [
  { id: 'case-fan', fan: true },        // a case with a fan on the fan header (for example the Argon ONE V5)
  { id: 'active-cooler', fan: true },   // the official active cooler (heatsink + fan)
  { id: 'passive', fan: false },        // a passive heatsink case
  { id: 'none', fan: false },           // a bare board
];
export const coolerHasFan = (id) => !!COOLERS.find((c) => c.id === id && c.fan);
export function profilesFor(cooler) {
  if (coolerHasFan(cooler)) return ['balanced', 'performance', 'quiet'];
  if (cooler === 'none') return ['conservative', 'balanced'];
  return ['balanced', 'conservative'];
}
export const defaultProfile = (cooler) => (cooler === 'none' ? 'conservative' : 'balanced');
export const overclockAllowed = (cooler) => coolerHasFan(cooler);

export const THEMES = ['default', 'classic'];
export const MODES = ['light', 'dark'];
// Lord Flow's accents (themes/default/tokens.js)
export const ACCENTS = [
  { id: 'azul', color: '#0a84ff' }, { id: 'violeta', color: '#bf5af2' }, { id: 'rosa', color: '#ff375f' },
  { id: 'naranja', color: '#ff9f0a' }, { id: 'verde', color: '#30d158' }, { id: 'grafito', color: '#8e8e93' },
];
// A few of the LordOS wallpapers (assets/wallpapers/index.json ids)
export const WALLPAPERS = ['lumo-celestial', 'crystal-crown', 'aurora'];

// What a new customisation starts with (the board's own defaults where nothing is chosen)
export function defaultConfig() {
  return {
    hostname: '', lang: 'en', timezone: '', keyboard: '', variant: '', wifiCountry: '',
    user: { name: '', fullname: '', autologin: false },
    wifi: { enabled: false, ssid: '', hidden: false, open: false },
    ssh: 'off', sshKeys: [],
    remote: false, bluetooth: true,
    theme: 'default', mode: 'dark', accent: 'azul', wallpaper: 'lumo-celestial',
    cooler: '', performance: '', overclock: false,
    autoUpdates: true,
  };
}
