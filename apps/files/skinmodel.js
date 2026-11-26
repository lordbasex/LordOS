// skinmodel.js — what Files hands a theme's skin (docs/THEMES-GUIDE.md, "App skins → Files"): the view model of its
// regions (toolbar, addressBar, sidebar, itemView, statusBar, contextMenu). Plain data and Files' own actions as
// functions; never lord.fs, never an entry object of Files' own (each item is a fresh copy), so a skin can only show
// what Files shows and ask for what Files offers. Files does every action with its own checks (read-only folders, the
// protected folders of lord.fs, the confirmations, the Trash instead of deleting). Node-tested by
// server/tests/appskin_test.mjs; Files with every region replaced by a stub: server/tests/files_skin_scene.mjs.
import { kindOf, extOf } from '../../sdk/filekinds.js';

// The places a skin can offer ("Other Places" in a task pane, a drop-down in an address bar), by id: Files navigates.
export const SKIN_PLACES = ['home', 'desktop', 'documents', 'downloads', 'pictures', 'music', 'videos', 'computer', 'network', 'recent', 'trash'];

// One item as a skin sees it. key: what the actions take (the path; "net:…" on the Network page).
export function skinItem(e, s) {
  return {
    key: e.path,
    name: e.name,
    label: s.shownName ? s.shownName(e) : e.name,
    dir: !!e.dir,
    size: typeof e.size === 'number' ? e.size : 0,
    mtime: typeof e.mtime === 'number' ? e.mtime : 0,
    kind: e.app ? 'app' : kindOf(e),
    ext: e.dir ? '' : extOf(e.name),
    selected: s.selected.has(e.path),
    cut: s.cut.has(e.path),
    renaming: !!s.renaming && s.renaming.path === e.path,
  };
}

// s: Files' state for this render (see apps/files/index.jsx); a: its actions.
export function filesViewModel(s, a) {
  const items = s.items.map((e) => skinItem(e, s));
  const sel = items.filter((x) => x.selected);
  const first = sel[0] || null;
  return {
    app: 'files',
    location: {
      type: s.loc.type,
      path: s.loc.type === 'dir' ? s.loc.path : null,
      title: s.title,
      crumbs: s.crumbs.map((c, i) => ({ label: c.label, folder: !!c.folder, index: i })),
      readOnly: !!s.readOnly,
      readOnlyText: s.readOnlyText || null,
      writable: !!s.writable,
    },
    history: { back: !!s.canBack, forward: !!s.canForward, up: !!s.canUp },
    view: s.view,
    views: s.views.slice(),
    sort: { by: s.sort.by, desc: !!s.sort.desc },
    zoom: s.zoom,
    query: s.query,
    items,
    selection: { count: sel.length, keys: sel.map((x) => x.key), first },
    renaming: s.renaming ? { key: s.renaming.path, value: s.renaming.value } : null,
    // the folder shown, or the first selected item: what an XP-style "Details" box describes
    details: first ? { name: first.label, kind: first.kind, dir: first.dir, size: first.size, mtime: first.mtime, count: sel.length }
      : s.loc.type === 'dir' ? { name: s.title, kind: 'folder', dir: true, size: 0, mtime: s.folderMtime || 0, count: 0 } : null,
    places: s.places.map((p) => ({ id: p.id, label: p.label, kind: p.kind, selected: !!p.selected })),
    status: { text: s.statusText, message: s.message || null, job: s.job || null },
    empty: items.length ? null : (s.emptyText || null),
    loading: !!s.loading,
    canPaste: !!s.canPaste,
    actions: a,
    FileIcon: s.FileIcon,
  };
}

// Whether an address typed in a skin's address bar names a folder Files can open: '~', '~/…', an absolute path, or a
// path under the folder shown. Returns the absolute path (no trailing slash) or null. Files still checks it exists.
export function addressPath(text, home, cwd) {
  const t = String(text || '').trim();
  if (!t || t.length > 4096 || /[\0\n]/.test(t)) return null;
  let p = t.replace(/^~(?=\/|$)/, home);
  if (!p.startsWith('/')) { if (!cwd) return null; p = `${cwd === '/' ? '' : cwd}/${p}`; }
  const parts = [];
  for (const seg of p.split('/')) {
    if (!seg || seg === '.') continue;
    if (seg === '..') parts.pop(); else parts.push(seg);
  }
  return `/${parts.join('/')}`;
}
