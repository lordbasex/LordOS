// prefs.js — Files → Settings, kept in settings.json under `files`. Every value here changes something (docs/FILES.md,
// "Settings").
import { settings, useSettings } from '@lordos/sdk';
import { normalColumns } from './columns.js';
import { SIDE_DEFAULT } from './viewprefs.js';

export { clampSide, viewFor, viewKey, folderViewsWith } from './viewprefs.js';

export const SIDEBAR_ITEMS = ['recent', 'apps', 'desktop', 'documents', 'downloads', 'pictures', 'music', 'videos', 'home', 'computer', 'network', 'volumes', 'trash', 'tags'];
const DEFAULTS = {
  newWindow: 'home',          // 'home' | 'recent' | 'computer' | a user folder's name
  foldersInTabs: false,       // a double click opens a folder in a new tab
  hiddenSide: [],             // sidebar items turned off
  hiddenTags: [],             // tag colors left out of the sidebar
  tagNames: {},               // color → the person's name for it
  showExtensions: true,
  warnExtension: true,
  warnEmptyTrash: true,
  trash30: false,             // Trash items older than 30 days go when Files starts
  foldersFirst: true,
  searchScope: 'here',        // 'here' | 'home' | 'last'
  lastScope: 'here',
  columns: null,              // columns.js
  recentFolders: [],
  sideWidth: SIDE_DEFAULT,
  defaultView: null,          // { view, sort } for a folder with none of its own
  folderViews: {},            // place → { view, sort }, the newest last
  servers: { history: [], favorites: [] },
  pins: [],                   // { address, path, name }: servers kept in Locations; passwords stay in the keyring
};

export const prefsOf = (s) => ({ ...DEFAULTS, ...(s.files || {}) });
export const filesPrefs = () => prefsOf(settings.get());
export function setPrefs(p) { settings.set({ files: { ...filesPrefs(), ...p } }); }
export function useFilesPrefs() { const [s] = useSettings(); return prefsOf(s); }
export const columnsOf = (p) => normalColumns(p.columns);

export function noteFolder(path) {
  const list = [path, ...filesPrefs().recentFolders.filter((x) => x !== path)].slice(0, 10);
  setPrefs({ recentFolders: list });
}
export function noteServer(address) {
  const { servers } = filesPrefs();
  setPrefs({ servers: { ...servers, history: [address, ...servers.history.filter((x) => x !== address)].slice(0, 12) } });
}
// Where a mounted folder came from, so that it can be pinned: its folder name alone doesn't say (fixed per server, share_leaf)
export function noteOrigin(path, address) {
  const { servers } = filesPrefs();
  const origin = Object.entries({ ...(servers.origin || {}), [path]: address }).slice(-32);
  setPrefs({ servers: { ...servers, origin: Object.fromEntries(origin) } });
}
export const originOf = (path, fstype, name) => {
  const known = (filesPrefs().servers.origin || {})[path];
  if (known) return known;
  const m = /^(.+) on ([^ ]+)$/.exec(name || '');
  return m && (fstype === 'cifs' || fstype === 'smb3') ? `smb://${m[2]}/${m[1]}` : '';
};
export function togglePin(pin) {
  const { pins } = filesPrefs();
  setPrefs({ pins: pins.some((p) => p.path === pin.path) ? pins.filter((p) => p.path !== pin.path) : [...pins, pin] });
}
export function toggleFavorite(address) {
  const { servers } = filesPrefs();
  const has = servers.favorites.includes(address);
  setPrefs({ servers: { ...servers, favorites: has ? servers.favorites.filter((x) => x !== address) : [...servers.favorites, address] } });
}
