// profiles.js — Terminal profiles and color schemes.
// They live in the Terminal's own file, ~/.config/lordos/terminal.json or the one the user chose (config.js).
import { t } from '@lordos/sdk';
import { termConfig, loadTermConfig, saveTermConfig } from './config.js';

const ansi = (s) => s.split(' ').map((h) => `#${h}`);

// Bundled schemes: a classic default and some popular ones.
export const PRESETS = {
  lordos: { name: 'LordOS', fg: '#e5e5ea', bg: '#1c1c1f', cursor: '#0a84ff', cursorText: '#ffffff', selection: '#3a5a8c', selectionText: '#ffffff',
    ansi: ansi('2c2c30 ff453a 32d74b ffd60a 0a84ff bf5af2 64d2ff d1d1d6 636366 ff6961 30db5b ffd426 409cff da8fff 70d7ff ffffff') },
  iterm: { name: 'Classic', fg: '#c7c7c7', bg: '#000000', cursor: '#c7c7c7', cursorText: '#000000', selection: '#b5d5ff', selectionText: '#000000',
    ansi: ansi('000000 c91b00 00c200 c7c400 2225c4 ca30c7 00c5c7 c7c7c7 686868 ff6e67 5ffa68 fffc67 6871ff ff77ff 60fdff ffffff') },
  dark: { name: 'Dark Background', fg: '#bbbbbb', bg: '#000000', cursor: '#bbbbbb', cursorText: '#ffffff', selection: '#b5d5ff', selectionText: '#000000',
    ansi: ansi('000000 bb0000 00bb00 bbbb00 0000bb bb00bb 00bbbb bbbbbb 555555 ff5555 55ff55 ffff55 5555ff ff55ff 55ffff ffffff') },
  light: { name: 'Light Background', fg: '#000000', bg: '#ffffff', cursor: '#000000', cursorText: '#ffffff', selection: '#cae2ff', selectionText: '#000000',
    ansi: ansi('000000 bb0000 00bb00 bbbb00 0000bb bb00bb 00bbbb bbbbbb 555555 ff5555 55ff55 ffff55 5555ff ff55ff 55ffff ffffff') },
  pastel: { name: 'Pastel (Dark Background)', fg: '#bbbbbb', bg: '#000000', cursor: '#ffa560', cursorText: '#ffffff', selection: '#363983', selectionText: '#f2f2f2',
    ansi: ansi('4f4f4f ff6c60 a8ff60 ffffb6 96cbfe ff73fd c6c5fe eeeeee 7c7c7c ffb6b0 ceffac ffffcc b5dcff ff9cfe dfdffe ffffff') },
  smooth: { name: 'Smoooooth', fg: '#dcdcdc', bg: '#15191f', cursor: '#ffffff', cursorText: '#000000', selection: '#b3d7ff', selectionText: '#000000',
    ansi: ansi('14191e b43c2a 00c200 c7c400 2744c7 c040be 00c5c7 c7c7c7 686868 dd7975 58e790 ece100 a7abf2 e17ee1 60fdff ffffff') },
  solarizedDark: { name: 'Solarized Dark', fg: '#839496', bg: '#002b36', cursor: '#839496', cursorText: '#073642', selection: '#073642', selectionText: '#93a1a1',
    ansi: ansi('073642 dc322f 859900 b58900 268bd2 d33682 2aa198 eee8d5 002b36 cb4b16 586e75 657b83 839496 6c71c4 93a1a1 fdf6e3') },
  solarizedLight: { name: 'Solarized Light', fg: '#657b83', bg: '#fdf6e3', cursor: '#657b83', cursorText: '#eee8d5', selection: '#eee8d5', selectionText: '#586e75',
    ansi: ansi('073642 dc322f 859900 b58900 268bd2 d33682 2aa198 eee8d5 002b36 cb4b16 586e75 657b83 839496 6c71c4 93a1a1 fdf6e3') },
  tangoDark: { name: 'Tango Dark', fg: '#ffffff', bg: '#000000', cursor: '#ffffff', cursorText: '#000000', selection: '#c1deff', selectionText: '#000000',
    ansi: ansi('000000 cc0000 4e9a06 c4a000 3465a4 75507b 06989a d3d7cf 555753 ef2929 8ae234 fce94f 729fcf ad7fa8 34e2e2 eeeeec') },
  tangoLight: { name: 'Tango Light', fg: '#000000', bg: '#ffffff', cursor: '#000000', cursorText: '#ffffff', selection: '#c1deff', selectionText: '#000000',
    ansi: ansi('000000 cc0000 4e9a06 c4a000 3465a4 75507b 06989a d3d7cf 555753 ef2929 8ae234 fce94f 729fcf ad7fa8 34e2e2 eeeeec') },
  dracula: { name: 'Dracula', fg: '#f8f8f2', bg: '#282a36', cursor: '#f8f8f2', cursorText: '#282a36', selection: '#44475a', selectionText: '#ffffff',
    ansi: ansi('21222c ff5555 50fa7b f1fa8c bd93f9 ff79c6 8be9fd f8f8f2 6272a4 ff6e6e 69ff94 ffffa5 d6acff ff92df a4ffff ffffff') },
  nord: { name: 'Nord', fg: '#d8dee9', bg: '#2e3440', cursor: '#d8dee9', cursorText: '#2e3440', selection: '#434c5e', selectionText: '#eceff4',
    ansi: ansi('3b4252 bf616a a3be8c ebcb8b 81a1c1 b48ead 88c0d0 e5e9f0 4c566a bf616a a3be8c ebcb8b 81a1c1 b48ead 8fbcbb eceff4') },
  gruvbox: { name: 'Gruvbox Dark', fg: '#ebdbb2', bg: '#282828', cursor: '#ebdbb2', cursorText: '#282828', selection: '#504945', selectionText: '#ebdbb2',
    ansi: ansi('282828 cc241d 98971a d79921 458588 b16286 689d6a a89984 928374 fb4934 b8bb26 fabd2f 83a598 d3869b 8ec07c ebdbb2') },
  oneDark: { name: 'One Dark', fg: '#abb2bf', bg: '#282c34', cursor: '#528bff', cursorText: '#ffffff', selection: '#3e4451', selectionText: '#ffffff',
    ansi: ansi('282c34 e06c75 98c379 e5c07b 61afef c678dd 56b6c2 abb2bf 5c6370 e06c75 98c379 e5c07b 61afef c678dd 56b6c2 ffffff') },
  tokyoNight: { name: 'Tokyo Night', fg: '#c0caf5', bg: '#1a1b26', cursor: '#c0caf5', cursorText: '#1a1b26', selection: '#33467c', selectionText: '#c0caf5',
    ansi: ansi('15161e f7768e 9ece6a e0af68 7aa2f7 bb9af7 7dcfff a9b1d6 414868 f7768e 9ece6a e0af68 7aa2f7 bb9af7 7dcfff c0caf5') },
  catppuccin: { name: 'Catppuccin Mocha', fg: '#cdd6f4', bg: '#1e1e2e', cursor: '#f5e0dc', cursorText: '#1e1e2e', selection: '#585b70', selectionText: '#cdd6f4',
    ansi: ansi('45475a f38ba8 a6e3a1 f9e2af 89b4fa f5c2e7 94e2d5 bac2de 585b70 f38ba8 a6e3a1 f9e2af 89b4fa f5c2e7 94e2d5 a6adc8') },
};

export const DEFAULT_PROFILE = {
  id: 'default',
  name: '',                  // '' = "Predeterminado" en el idioma de la interfaz
  colors: 'lordos',          // id de PRESETS o 'custom'
  custom: null,              // esquema propio cuando colors === 'custom'
  fontSize: 13,
  lineSpacing: 1.0,
  padding: 8,
  cursorStyle: 'block',      // 'block' | 'underline' | 'beam'
  cursorBlink: false,
  cursorGuide: false,
  boldBright: true,
  useBoldFont: true,
  useItalic: true,
  blinkText: false,
  minimumContrast: 0,
  transparency: 0,
  blur: true,
  dimInactive: true,
  statusBar: true,
  scrollback: 10000,
  mouseReporting: true,
  copyOnSelect: true,
  alternateScroll: true,
  altSendsEscape: true,
  wordChars: '/-+\\~_.',
  workingDir: 'reuse',       // 'home' | 'reuse' | 'custom'
  customDir: '',
  badge: '',
  confirmClose: true,
  bell: 'visual',            // 'visual' | 'none'
  triggers: [],              // [{ regex, action: 'notify' | 'bell' | 'send' | 'attention' | 'capture', param }]
  tabColor: '',
  rules: '',                 // cambio automático de perfil: una regla por línea (ver autoProfile en state.js)
  font: '',                  // familia ('' = JetBrains Mono incluida)
  nonAsciiFont: '',          // font for non-ASCII characters
  ligatures: false,
  bidi: true,          // árabe y hebreo en orden visual y con letras unidas
  keyMap: [],                // [{ combo, action: 'text'|'escape'|'hex'|'ignore'|<acción>, param }]
  initialText: '',           // se escribe en el shell al abrir la sesión
  columns: 0, rows: 0,       // tamaño de ventanas nuevas (0 = el de la app)
  itermGuid: '',
  tags: [],                  // iTerm2's profile tags: the profile menus group by them
};

const DEFAULTS = { profiles: [DEFAULT_PROFILE], defaultProfile: 'default', hotkey: 'F12', schemes: {}, shortcuts: {}, keyMap: [] };

export function terminalSettings() {
  loadTermConfig();
  const t = termConfig.get().data || {};
  const profiles = (t.profiles && t.profiles.length ? t.profiles : DEFAULTS.profiles).map((p) => ({ ...DEFAULT_PROFILE, ...p }));
  return { ...DEFAULTS, ...t, profiles };
}

// false while the chosen file is away (read-only): the change stays in memory (Preferences says so)
export function saveTerminalSettings(patch) {
  loadTermConfig();
  return saveTermConfig({ ...(termConfig.get().data || {}), ...patch });
}

export const getProfile = (id) => {
  const t = terminalSettings();
  return t.profiles.find((p) => p.id === id) || t.profiles.find((p) => p.id === t.defaultProfile) || t.profiles[0];
};

// Esquemas incluidos + los importados (terminal.schemes en los ajustes).
export const allSchemes = () => ({ ...PRESETS, ...(terminalSettings().schemes || {}) });
export const schemeOf = (p) => (p.colors === 'custom' && p.custom ? p.custom : allSchemes()[p.colors] || PRESETS.lordos);

const hex = (c) => parseInt(String(c).replace('#', '').slice(0, 6), 16) || 0;

// Perfil → configuración nativa de lord.term.configure().
export function toConfig(p, extra = {}) {
  const s = schemeOf(p);
  const cfg = {
    fontSize: p.fontSize, lineSpacing: p.lineSpacing, padding: p.padding,
    palette: s.ansi.map(hex), foreground: hex(s.fg), background: hex(s.bg), cursor: hex(s.cursor),
    selection: hex(s.selection), link: hex(s.link || '#2b7ff7'), cursorGuideColor: hex(s.guide || '#a6e8ff'),
    backgroundOpacity: 1 - (p.transparency || 0), minimumContrast: p.minimumContrast,
    boldBright: p.boldBright ? 1 : 0, useBoldFont: p.useBoldFont ? 1 : 0, useItalic: p.useItalic ? 1 : 0,
    cursorStyle: p.cursorStyle, cursorBlink: p.cursorBlink ? 1 : 0, cursorGuide: p.cursorGuide ? 1 : 0,
    blinkText: p.blinkText ? 1 : 0, altSendsEscape: p.altSendsEscape ? 1 : 0, mouseReporting: p.mouseReporting ? 1 : 0,
    copyOnSelect: p.copyOnSelect ? 1 : 0, alternateScroll: p.alternateScroll ? 1 : 0, wordChars: p.wordChars,
    timestamps: extra.timestamps ? 1 : 0,
    font: p.font || '', nonAsciiFont: p.nonAsciiFont || '', ligatures: p.ligatures ? 1 : 0, bidi: p.bidi === false ? 0 : 1,
  };
  if (s.cursorText) cfg.cursorText = hex(s.cursorText);
  if (s.selectionText) cfg.selectionText = hex(s.selectionText);
  if (s.bold) cfg.bold = hex(s.bold);
  if (extra.fontDelta) cfg.fontSize = Math.max(6, Math.min(72, p.fontSize + extra.fontDelta));
  return cfg;
}

export function newProfileId() {
  return `p${Date.now().toString(36)}`;
}

// Nombre visible: el perfil de fábrica se traduce (también el de ajustes viejos, "Predeterminado").
// The profile menus (Shell → Profiles, the Dock's): profiles without a tag first, then one submenu per tag in name order,
// a profile under each of its tags, as iTerm2 does. item(p) makes a profile's menu item.
const tagCmp = (a, b) => a.localeCompare(b, undefined, { sensitivity: 'base', numeric: true });
export function profileMenu(profiles, item) {
  const tagsOf = (p) => (Array.isArray(p.tags) ? p.tags.filter((x) => typeof x === 'string' && x.trim()) : []);
  const loose = profiles.filter((p) => !tagsOf(p).length).map(item);
  const tags = [...new Set(profiles.flatMap(tagsOf))].sort(tagCmp);
  if (!tags.length) return loose;
  return [...loose, ...(loose.length ? ['-'] : []), ...tags.map((tag) => ({ label: tag, submenu: profiles.filter((p) => tagsOf(p).includes(tag)).map(item) }))];
}

export const profileName = (p) => (!p?.name || (p.id === 'default' && p.name === 'Predeterminado') ? t('terminal.profile.default') : p.name);
