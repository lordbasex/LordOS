// appskinmodel.js — the pure rules of style packs and app skins (part 4 of the community-themes plan; how to use them:
// docs/THEMES-GUIDE.md, "Style packs" and "App skins"). Plain data for node (server/tests/appskin_test.mjs).
//
// Level 1, a style pack: a theme brings icons (app, file-type and toolbar icons, by our names), pointer colours, a sound
// scheme of plain notes and an installed font. Level 2, app skins: a native app names the regions a theme may draw, and
// a theme registers a component per region. The app hands that component a view model (what it shows and the actions it
// offers, as functions) and keeps every rule: a skin only asks for an action; the app does it with its own checks.
//
// The person keeps the last word: Settings → Appearance → "From this style" turns each part off on its own
// (settings.themeParts[part] === false), so someone can keep a theme's frames and use LordOS's icons.

// The regions each native app lets a theme draw, in the order the app lays them out. An app that is not listed has none.
export const APP_REGIONS = {
  files: ['toolbar', 'addressBar', 'sidebar', 'itemView', 'statusBar', 'contextMenu'],
  settings: ['sidebar', 'home'],
  terminal: ['tabBar', 'statusBar', 'contextMenu'],
  editor: ['toolbar', 'tabBar', 'statusBar', 'contextMenu'],
  store: ['sidebar', 'toolbar'],
  notes: ['sidebar', 'toolbar'],
  calculator: ['display', 'keypad'],
  photos: ['sidebar', 'toolbar'],
  player: ['sidebar', 'toolbar', 'transport'],
  monitor: ['toolbar'],
  clock: ['toolbar'],
  screenshots: ['toolbar'],
  disks: ['sidebar'],
  about: ['banner'],
  help: ['sidebar'],
  characters: ['toolbar', 'sidebar'],
};

// The parts of a style a person can turn off one by one.
export const STYLE_PARTS = ['icons', 'cursors', 'sounds', 'fonts', 'layouts'];

// The sound events a theme can give a tune to.
export const SOUND_EVENTS = ['startup', 'notify', 'error', 'emptyTrash'];
export const MAX_NOTES = 8;
export const MAX_TUNE_MS = 2500;

// The icon groups of a pack: app icons by app id, file-type icons by kind (folder, text, image, …; sdk/filekinds.js),
// toolbar icons by our names (TOOLBAR_ICONS).
export const ICON_GROUPS = ['apps', 'files', 'toolbar'];
export const TOOLBAR_ICONS = ['back', 'forward', 'up', 'newFolder', 'search', 'views', 'icons', 'list', 'columns', 'gallery',
  'sort', 'tags', 'refresh', 'terminal', 'more', 'go', 'copy', 'cut', 'paste', 'trash', 'info', 'sidebar'];

const NAME = /^[A-Za-z][A-Za-z0-9_.-]{0,63}$/;
const HEX = /^#[0-9a-f]{6}$/i;

export const partOn = (s, part) => !(s && s.themeParts && s.themeParts[part] === false);
// The settings patch that turns one part on or off, keeping the others.
export const partPatch = (s, part, on) => ({ themeParts: { ...((s && s.themeParts) || {}), [part]: !!on } });

// A theme's skins: { appId: { region: Component } }, keeping only the apps and regions listed above and components
// (functions). Anything else is dropped, so a typo can't reach an app.
export function cleanSkins(map, regions = APP_REGIONS) {
  const out = {};
  if (!map || typeof map !== 'object') return out;
  for (const app of Object.keys(map)) {
    const names = regions[app];
    const given = map[app];
    if (!names || !given || typeof given !== 'object') continue;
    const kept = {};
    for (const r of names) if (typeof given[r] === 'function') kept[r] = given[r];
    if (Object.keys(kept).length) out[app] = kept;
  }
  return out;
}

// The component that draws a region now, or null for the app's own (no skin, or the person turned layouts off).
export function skinFor(skins, s, app, region) {
  if (!partOn(s, 'layouts')) return null;
  const a = skins && skins[app];
  return (a && typeof a[region] === 'function' && a[region]) || null;
}

// A tune: [[freq, ms], …] with freq 60–5000 Hz (0 is a rest) and ms 10–1000, at most MAX_NOTES notes and MAX_TUNE_MS in
// all (lord-server checks the same: server/src/sound/snd.h). null when it isn't one.
export function cleanTune(list) {
  if (!Array.isArray(list) || !list.length || list.length > MAX_NOTES) return null;
  let total = 0;
  const out = [];
  for (const n of list) {
    if (!Array.isArray(n) || n.length !== 2) return null;
    const [f, ms] = n;
    if (typeof f !== 'number' || typeof ms !== 'number' || !Number.isFinite(f) || !Number.isFinite(ms)) return null;
    if (!(f === 0 || (f >= 60 && f <= 5000)) || ms < 10 || ms > 1000) return null;
    total += ms;
    out.push([f, ms]);
  }
  return total <= MAX_TUNE_MS ? out : null;
}

export const cleanCursors = (c) => (c && HEX.test(c.fill || '') && HEX.test(c.outline || '') ? { fill: c.fill.toLowerCase(), outline: c.outline.toLowerCase() } : null);

// An installed font family's name ("DejaVu Sans"): letters, digits, spaces, '-' and '_', at most 63 characters.
export const cleanFamily = (f) => (typeof f === 'string' && /^[A-Za-z0-9][A-Za-z0-9 _-]{0,62}$/.test(f) ? f : null);

// An icon: a component (drawn with nodes, as LordOS's are) or an image the theme carries ("assets/…png" or ".svg",
// resolved against the theme's folder; never "..", an absolute path or a URL).
export function cleanIcon(v) {
  if (typeof v === 'function') return v;
  if (typeof v === 'string' && /^assets\/[A-Za-z0-9_./-]{1,120}\.(png|svg)$/i.test(v) && !v.includes('..')) return v;
  return null;
}

// A theme's style pack, checked: { icons: { apps, files, toolbar }, cursors, sounds, fonts }. What is wrong is dropped.
export function cleanStylePack(p) {
  const icons = { apps: {}, files: {}, toolbar: {} };
  const out = { icons, cursors: null, sounds: {}, fonts: null };
  if (!p || typeof p !== 'object') return out;
  for (const g of ICON_GROUPS) {
    const src = p.icons && p.icons[g];
    if (!src || typeof src !== 'object') continue;
    for (const k of Object.keys(src)) {
      if (!NAME.test(k)) continue;
      if (g === 'toolbar' && !TOOLBAR_ICONS.includes(k)) continue;
      const v = cleanIcon(src[k]);
      if (v) icons[g][k] = v;
    }
  }
  out.cursors = cleanCursors(p.cursors);
  if (p.sounds && typeof p.sounds === 'object') for (const e of SOUND_EVENTS) { const tn = cleanTune(p.sounds[e]); if (tn) out.sounds[e] = tn; }
  const fam = p.fonts && cleanFamily(p.fonts.family);
  out.fonts = fam ? { family: fam } : null;
  return out;
}

// Which parts a pack brings (for Settings: only those get a switch). layouts: the theme registered app skins.
export function packParts(pack, skins) {
  const has = [];
  if (pack && ICON_GROUPS.some((g) => Object.keys(pack.icons[g]).length)) has.push('icons');
  if (pack && pack.cursors) has.push('cursors');
  if (pack && Object.keys(pack.sounds).length) has.push('sounds');
  if (pack && pack.fonts) has.push('fonts');
  if (skins && Object.keys(skins).length) has.push('layouts');
  return has;
}

// The pack's icon for a group and name, or null (LordOS's own then), honouring the person's switch.
export function packIcon(pack, s, group, name) {
  if (!pack || !partOn(s, 'icons')) return null;
  const g = pack.icons[group];
  return (g && Object.prototype.hasOwnProperty.call(g, name) && g[name]) || null;
}

// What lord-server is told for a pack and the person's switches: the pointer colours, each event's tune (null: LordOS's
// own) and the font family (null: Inter).
export function packEffects(pack, s) {
  const p = pack || cleanStylePack(null);
  const sounds = {};
  for (const e of SOUND_EVENTS) sounds[e] = partOn(s, 'sounds') ? p.sounds[e] || null : null;
  return {
    cursors: partOn(s, 'cursors') ? p.cursors : null,
    sounds,
    font: partOn(s, 'fonts') && p.fonts ? p.fonts.family : null,
  };
}

// A view model handed to a skin: plain data frozen (a skin can't change what the app shows by writing into it), functions
// kept as they are (they are the app's actions). Cycles and depth beyond 6 stop the walk.
export function freezeModel(m, depth = 0) {
  if (!m || typeof m !== 'object' || Object.isFrozen(m) || depth > 6) return m;
  for (const k of Object.keys(m)) {
    const v = m[k];
    if (v && typeof v === 'object' && !isNodeLike(v)) freezeModel(v, depth + 1);
  }
  return Object.freeze(m);
}
// React elements and Sets/Maps are left alone (React owns the first; the others can't be frozen usefully)
const isNodeLike = (v) => (v.$$typeof !== undefined) || v instanceof Set || v instanceof Map;

// ---- two shapes most apps share (0.8.9.6.2) ----
// A command bar (an app's `toolbar` region): the app's title for the place shown and a closed list of its own commands,
// each { id, label, disabled, active }; the skin asks with actions.command(id), and the app runs only an id on its list
// and not disabled. A list pane (a `sidebar` region): the places or items the app's own sidebar lists, each { key, label,
// detail, selected, group }; the skin asks with actions.select(key). Both are plain data: the app builds them from what
// its own regions show, and a skin learns nothing more (docs/THEMES-GUIDE.md, "App skins → the shared shapes").
export function commandBar(s) {
  const cmds = (s.commands || []).filter((c) => c && typeof c.id === 'string').map((c) => ({ id: c.id, label: String(c.label || ''), disabled: !!c.disabled, active: !!c.active,
    ...(c.group ? { group: String(c.group) } : null) }));
  return { title: String(s.title || ''), commands: cmds };
}
// The command a skin asked for, if the bar offers it now (on the list and not disabled), else null.
export function pickCommand(bar, id) {
  const c = bar && bar.commands.find((x) => x.id === id);
  return c && !c.disabled ? c.id : null;
}
export function listPane(s) {
  return { title: String(s.title || ''), items: (s.items || []).filter((x) => x && x.key != null).map((x) => ({ key: String(x.key), label: String(x.label || ''),
    detail: x.detail == null ? '' : String(x.detail), selected: !!x.selected, ...(x.group ? { group: String(x.group) } : null) })) };
}
export const pickItem = (pane, key) => (pane && pane.items.some((x) => x.key === String(key)) ? String(key) : null);
