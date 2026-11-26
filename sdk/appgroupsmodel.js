// appgroupsmodel.js — the start menu's apps by category (Classic, themes/classic/components/StartMenu.jsx):
// which group an app belongs to, the groups with their apps and counts, and the rows the keyboard walks.
// No `lord`, no React: node tests import it (server/tests/app_groups_model_test.mjs).
//
// An app's group comes from, in order: its manifest's "category" (a LordOS app, a web app), the first freedesktop
// main category in its .desktop's Categories (a Linux or Flatpak app), the first additional category this file
// knows (a .desktop that lists only "WebBrowser" or "TerminalEmulator"), and else "Other". The group ids are the
// freedesktop main categories, so their names are the same translations as the Store's and Flow's app launcher's
// (shell.category.<id>), plus "Other".

// The groups, in the order an English reader sees them; the start menu sorts them by their translated names.
export const APP_GROUPS = Object.freeze(['Utility', 'Development', 'Education', 'Game', 'Graphics', 'Network', 'Office',
  'AudioVideo', 'Science', 'System', 'Settings', 'Other']);

// The freedesktop main categories (Desktop Menu Specification, "Main Categories") and the group each one is in.
const MAIN = Object.freeze({
  AudioVideo: 'AudioVideo', Audio: 'AudioVideo', Video: 'AudioVideo', Development: 'Development', Education: 'Education',
  Game: 'Game', Graphics: 'Graphics', Network: 'Network', Office: 'Office', Science: 'Science', Settings: 'Settings',
  System: 'System', Utility: 'Utility',
});

// Additional categories ("Additional Categories") that name their group well enough when a .desktop lists no main one.
const EXTRA = Object.freeze({
  // Audio & video
  Midi: 'AudioVideo', Mixer: 'AudioVideo', Sequencer: 'AudioVideo', Tuner: 'AudioVideo', TV: 'AudioVideo',
  AudioVideoEditing: 'AudioVideo', Player: 'AudioVideo', Recorder: 'AudioVideo', DiscBurning: 'AudioVideo', Music: 'AudioVideo',
  // Development
  Building: 'Development', Debugger: 'Development', IDE: 'Development', GUIDesigner: 'Development', Profiling: 'Development',
  RevisionControl: 'Development', Translation: 'Development', WebDevelopment: 'Development', Database: 'Development',
  // Education
  Languages: 'Education', Literature: 'Education', Art: 'Education', Construction: 'Education', Geography: 'Education',
  History: 'Education', Humanities: 'Education', Sports: 'Education',
  // Games
  ActionGame: 'Game', AdventureGame: 'Game', ArcadeGame: 'Game', BoardGame: 'Game', BlocksGame: 'Game', CardGame: 'Game',
  KidsGame: 'Game', LogicGame: 'Game', RolePlaying: 'Game', Shooter: 'Game', Simulation: 'Game', SportsGame: 'Game',
  StrategyGame: 'Game', Emulator: 'Game',
  // Graphics
  '2DGraphics': 'Graphics', '3DGraphics': 'Graphics', VectorGraphics: 'Graphics', RasterGraphics: 'Graphics',
  Photography: 'Graphics', Scanning: 'Graphics', OCR: 'Graphics', ImageProcessing: 'Graphics', Viewer: 'Graphics',
  // Internet
  WebBrowser: 'Network', Email: 'Network', Chat: 'Network', InstantMessaging: 'Network', IRCClient: 'Network',
  Feed: 'Network', FileTransfer: 'Network', HamRadio: 'Network', News: 'Network', P2P: 'Network', RemoteAccess: 'Network',
  Telephony: 'Network', VideoConference: 'Network', Dialup: 'Network',
  // Office
  Calendar: 'Office', ContactManagement: 'Office', Dictionary: 'Office', Chart: 'Office', Finance: 'Office',
  FlowChart: 'Office', PDA: 'Office', ProjectManagement: 'Office', Presentation: 'Office', Spreadsheet: 'Office',
  WordProcessor: 'Office', Publishing: 'Office',
  // Science
  Astronomy: 'Science', Biology: 'Science', Chemistry: 'Science', ComputerScience: 'Science', DataVisualization: 'Science',
  Economy: 'Science', Electricity: 'Science', Geology: 'Science', Geoscience: 'Science', Math: 'Science',
  NumericalAnalysis: 'Science', MedicalSoftware: 'Science', Physics: 'Science', Robotics: 'Science', Engineering: 'Science',
  ArtificialIntelligence: 'Science', ParallelComputing: 'Science', Electronics: 'Science',
  // System
  TerminalEmulator: 'System', FileManager: 'System', Filesystem: 'System', Monitor: 'System', Security: 'System',
  PackageManager: 'System',
  // Settings
  DesktopSettings: 'Settings', HardwareSettings: 'Settings', Printing: 'Settings',
  // Utilities
  TextEditor: 'Utility', Accessibility: 'Utility', Archiving: 'Utility', Compression: 'Utility', Calculator: 'Utility',
  Clock: 'Utility', TextTools: 'Utility', FileTools: 'Utility', Documentation: 'Utility',
});

const own = (o, k) => Object.prototype.hasOwnProperty.call(o, k);

// The group of one app ({ category } from a manifest, or { categories: "A;B;" } from a .desktop).
export function launcherGroup(app) {
  const c = app && typeof app.category === 'string' ? app.category : '';
  if (c && own(MAIN, c)) return MAIN[c];
  const list = String((app && app.categories) || '').split(';').map((s) => s.trim()).filter(Boolean);
  for (const x of list) if (own(MAIN, x)) return MAIN[x];
  for (const x of list) if (own(EXTRA, x)) return EXTRA[x];
  return 'Other';
}

// The groups that hold apps: [{ id, count, apps }], sorted by label(id) with compare (the shell's collation), "Other"
// always last. Each group keeps the apps in the order given (the launcher's list is already sorted by name).
export function groupApps(apps, { label = (id) => id, compare = (a, b) => (a < b ? -1 : a > b ? 1 : 0) } = {}) {
  const by = new Map();
  for (const a of apps || []) {
    const g = launcherGroup(a);
    if (!by.has(g)) by.set(g, []);
    by.get(g).push(a);
  }
  const ids = APP_GROUPS.filter((g) => by.has(g));
  ids.sort((a, b) => (a === 'Other') - (b === 'Other') || compare(label(a), label(b)));
  return ids.map((id) => ({ id, count: by.get(id).length, apps: by.get(id) }));
}

// The view the "All apps" page opens with: the last one chosen (settings.startMenuView), A–Z when none or unknown.
export const START_VIEWS = Object.freeze(['az', 'categories']);
export const startView = (v) => (START_VIEWS.includes(v) ? v : 'az');

// The rows of the categories view, top to bottom: each group, then its apps when it is open.
//   [{ kind: 'group', id, count, open } | { kind: 'app', app, group }]
export function groupRows(groups, open) {
  const out = [];
  for (const g of groups || []) {
    const on = !!(open && open.has(g.id));
    out.push({ kind: 'group', id: g.id, count: g.count, open: on });
    if (on) for (const app of g.apps) out.push({ kind: 'app', app, group: g.id });
  }
  return out;
}

// A key on the categories view → what to do. rows: groupRows(); sel: the selected row's index; rtl: the reading
// direction (the "open" arrow points to where the text goes). Returns null for a key it doesn't use, else
// { sel } (move), { sel, toggle: groupId } (open or close a group and select it) or { launch: app }.
export function groupKey(key, rows, sel, rtl = false) {
  const n = rows ? rows.length : 0;
  if (!n) return null;
  const i = Math.max(0, Math.min(n - 1, sel | 0));
  const row = rows[i];
  const fwd = rtl ? 'ArrowLeft' : 'ArrowRight', back = rtl ? 'ArrowRight' : 'ArrowLeft';
  const groupAt = (id) => rows.findIndex((r) => r.kind === 'group' && r.id === id);
  switch (key) {
    case 'ArrowDown': return { sel: Math.min(n - 1, i + 1) };
    case 'ArrowUp': return { sel: Math.max(0, i - 1) };
    case 'Home': return { sel: 0 };
    case 'End': return { sel: n - 1 };
    case 'Enter': case ' ':
      return row.kind === 'group' ? { sel: i, toggle: row.id } : { launch: row.app };
    default: break;
  }
  if (key === fwd) {
    if (row.kind !== 'group') return { sel: i };
    return row.open ? { sel: Math.min(n - 1, i + 1) } : { sel: i, toggle: row.id };
  }
  if (key === back) {
    if (row.kind === 'app') { const g = groupAt(row.group); return { sel: g >= 0 ? g : i }; }
    return row.open ? { sel: i, toggle: row.id } : { sel: i };
  }
  return null;
}

// The selected row after the rows changed (a group opened or closed, apps installed): the same row if it is still
// there, else the group it was in, else the nearest index.
export function keepSelection(before, after, sel) {
  const r = before && before[sel];
  if (!r || !after || !after.length) return Math.max(0, Math.min((after ? after.length : 1) - 1, sel | 0));
  const same = after.findIndex((x) => x.kind === r.kind && (r.kind === 'group' ? x.id === r.id : x.app.id === r.app.id && x.group === r.group));
  if (same >= 0) return same;
  const g = after.findIndex((x) => x.kind === 'group' && x.id === (r.kind === 'group' ? r.id : r.group));
  return g >= 0 ? g : Math.max(0, Math.min(after.length - 1, sel));
}
