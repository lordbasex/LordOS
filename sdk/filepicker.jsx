// filepicker.jsx — the system file dialog, drawn like Files with Files' own pieces (fileparts.jsx,
// fileglyphs.jsx): for apps that run in the theme's context (Mail attachments, a contact's photo,
// importing .ics/.vcf) and for Linux apps through the portal below. Isolated .lpk apps use the
// powerbox (fs.openFile) instead. It shows the home folder and the local disks mounted for the user
// (lord.fs enforces that anyway); what it decides, it decides in filepickermodel.js.
// A List / Thumbnails switch in the toolbar (remembered in settings.pickerView; thumbnails when the
// dialog takes only pictures): the thumbnails are lord-media's (useThumb, the cache Files and Photos
// use), asked for only for the tiles in sight, and a file shows its type icon until its own arrives.
//
//   <FilePicker onPick={(path) => …} onClose={() => …} filter={/\.(png|jpe?g)$/i} title={t('…')} multiple />
import { useEffect, useMemo, useRef, useState } from 'react';
import { View, Text, Image, addListener } from '@lordos/runtime';
import { useTheme, ThemeContext, DEFAULT_TOKENS, shellTokens } from './theme.js';
import { t, direction } from './i18n.js';
import { Button, ScrollView, TextInput, takeTextFocus, releaseTextFocus, hasTextFocus } from './components.jsx';
import { Toolbar, ToolbarButton, ToolbarGroup, Hairline, EmptyState, spacing, typeScale } from './controls.jsx';
import { createStore, useStore } from './store.js';
import * as fs from './fs.js';
import { settings } from './settings.js';
import { useThumb } from './media.js';
import { kindOf } from './filekinds.js';
import { formatBytes } from './system.js';
import { EntryIcon, FolderGlyph, DriveGlyph } from './fileglyphs.jsx';
import { SideItem, SideTitle, PlaceGlyph, PathCrumbs, listRowStyle, listHeaderCell, listCell, when, useWidth, userDirs } from './fileparts.jsx';
import { localDisks, startFolder, pathCrumbs, fitCrumbs, sortRows, stepRow, rangeOf, validName, primaryAction, enterAction,
  imagesOnly, viewFor, saveView, liveRange, gridStep, cloudPlaces, cloudRoot } from './filepickermodel.js';
import { cloudState } from './cloud.js';
import { CloudGlyph } from './cloudui.jsx';

const ROW_H = 24;
const TILE_W = 104, TILE_H = 118, THUMB = 72, GRID_PAD = 8;
const THUMB_KINDS = ['image', 'video', 'audio'];
const SIDE_W = 176, DATE_W = 150, SIZE_W = 84;
const COL_TITLE = { name: 'files.col.name', date: 'files.col.date', size: 'files.col.size' };

const statOf = (path) => { try { return fs.stat(path); } catch (e) { return null; } };

function SortTitle({ k, width, sort, onSort, t: th }) {
  const on = sort.by === k;
  return (
    <View onClick={() => onSort(k)} style={{ ...(width ? { width } : { flex: 1, minWidth: 0 }), height: 26, flexDirection: 'row', alignItems: 'center', gap: 4, paddingHorizontal: 8,
      justifyContent: k === 'size' ? 'flex-end' : 'flex-start' }}>
      <Text style={listHeaderCell(th, on)}>{t(COL_TITLE[k])}</Text>
      {on && <Text style={{ fontSize: 8, color: th.textDim }}>{sort.desc ? '▼' : '▲'}</Text>}
    </View>
  );
}

// A tile's picture: its thumbnail once lord-media has made it, its type icon until then (and for good
// when it has none). Mounted only for the tiles in sight, so a big folder asks for a screenful.
function Thumb({ e, dark }) {
  const src = useThumb(e.path, 256);
  if (!src) return <EntryIcon e={e} size={THUMB} dark={dark} thumb={false} />;
  return (
    <View style={{ width: THUMB, height: THUMB, alignItems: 'center', justifyContent: 'center' }}>
      <Image src={src} style={{ width: THUMB, height: THUMB, objectFit: 'contain' }} />
    </View>
  );
}

function Tile({ e, on, live, th, onDown, onOpen }) {
  const dark = th.mode === 'dark';
  const pictured = live && !e.dir && THUMB_KINDS.includes(kindOf(e)) && !/\.svg$/i.test(e.name);
  return (
    <View onMouseDown={onDown} onDoubleClick={onOpen} style={{ width: TILE_W, height: TILE_H, paddingTop: 6, alignItems: 'center', gap: 4 }}>
      <View style={{ padding: 4, borderRadius: 7, backgroundColor: on ? (dark ? 'rgba(255,255,255,0.12)' : 'rgba(0,0,0,0.08)') : 'transparent' }}>
        {pictured ? <Thumb e={e} dark={dark} /> : <EntryIcon e={e} size={THUMB} dark={dark} thumb={false} />}
      </View>
      <View style={{ maxWidth: TILE_W - 8, paddingHorizontal: 5, paddingVertical: 1, borderRadius: 5, backgroundColor: on ? th.accent : 'transparent', alignItems: 'center' }}>
        <Text style={{ fontSize: 12, lineHeight: 16, maxHeight: 32, color: on ? '#fff' : th.text, textAlign: 'center', whiteSpace: 'normal', overflow: 'hidden',
          textOverflow: 'ellipsis', numberOfLines: 2 }}>{e.name}</Text>
      </View>
    </View>
  );
}

// mode: 'open' (the default), 'folder' (the selected folder, or the one in view) or 'save' (a name, with a question before replacing a file).
// modal: nothing else takes the keys while it is open (the portal's dialog, over everything).
export function FilePicker({ onPick, onClose, filter = null, title = '', multiple = false, start = '', mode = 'open', name = '', accept = '', modal = false }) {
  const th = useTheme();
  const home = fs.home();
  const disks = useMemo(() => { try { return localDisks(fs.volumes(), home); } catch (e) { return []; } }, []);
  const dirs = useMemo(() => userDirs().filter((d) => d.label !== 'desktop'), []);
  const [nav, setNav] = useState(() => {
    const dir = startFolder(start, home, disks);
    return { dir: statOf(dir)?.dir ? dir : home, back: [], fwd: [] };
  });
  const { dir } = nav;
  const [cursor, setCursor] = useState(null);        // the row selected, a path
  const [chosen, setChosen] = useState([]);          // multiple: files, also from other folders
  const anchor = useRef(null);
  const [sort, setSort] = useState({ by: 'name', desc: false });
  const [saveName, setSaveName] = useState(name);
  const [replacing, setReplacing] = useState(false);
  const root = useRef(null), listBox = useRef(null), scroll = useRef(null), typed = useRef({ text: '', at: 0 });
  const width = useWidth(root, 800);
  const listW = useWidth(listBox, 600);
  // List or Thumbnails: a dialog for pictures and one for anything else each remember their own
  const images = useMemo(() => imagesOnly(filter), []);
  const [view, setViewNow] = useState(() => viewFor(settings.get().pickerView, images));
  const setView = (v) => {
    if (v === view) return;
    setViewNow(v);
    settings.set((s) => ({ ...s, pickerView: saveView(s.pickerView, images, v) }));
    scroll.current?.scrollTo(0);
  };
  const thumbs = view === 'thumbs';
  const cols = Math.max(1, Math.floor((listW - 2 * GRID_PAD) / TILE_W));
  const [sight, setSight] = useState({ y: 0, h: 0 });

  const rows = useMemo(() => {
    let list = [];
    try { list = fs.list(dir).filter((e) => e.dir || (mode !== 'folder' && (!filter || filter.test(e.name)))); } catch (e) { list = []; }
    return sortRows(list.map((e) => ({ ...e, path: `${dir === '/' ? '' : dir}/${e.name}` })), sort);
  }, [dir, sort.by, sort.desc]);
  const row = rows.find((e) => e.path === cursor) || null;
  // lord.fs answers for a name inside a folder it may write to: the home folder and a disk are their own roots
  const writable = useMemo(() => mode !== 'save' || dir === home || disks.some((d) => d.path === dir) || !!statOf(dir)?.writable, [dir]);

  const show = (path) => {
    const i = rows.findIndex((r) => r.path === path);
    const s = scroll.current;
    if (i < 0 || !s) return;
    const rowH = thumbs ? TILE_H : ROW_H, top = (thumbs ? Math.floor(i / cols) : i) * rowH, box = lord.getLayout(listBox.current.id)[3];
    if (top < s.offset()) s.scrollTo(top);
    else if (top + 2 * rowH > s.offset() + box) s.scrollTo(top + 2 * rowH - box);
  };
  const go = (path, how = 'push') => {
    if (path === dir) return;
    setCursor(null); setReplacing(false); anchor.current = null;
    setNav((n) => {
      if (how === 'back') return { dir: n.back[n.back.length - 1], back: n.back.slice(0, -1), fwd: [n.dir, ...n.fwd] };
      if (how === 'fwd') return { dir: n.fwd[0], back: [...n.back, n.dir], fwd: n.fwd.slice(1) };
      return { dir: path, back: [...n.back, n.dir].slice(-50), fwd: [] };
    });
    scroll.current?.scrollTo(0);
  };
  const goBack = () => { if (nav.back.length) go(nav.back[nav.back.length - 1], 'back'); };
  const goForward = () => { if (nav.fwd.length) go(nav.fwd[0], 'fwd'); };

  const save = (fileName = saveName) => {
    if (!writable || !validName(fileName)) return;
    const target = `${dir === '/' ? '' : dir}/${fileName.trim()}`;
    const st = statOf(target);
    if (st?.dir) { setSaveName(''); go(target); return; }
    if (st && !replacing) { setReplacing(true); return; }
    onPick(target);
  };
  const act = (a) => {
    if (!a) return;
    if (a.go) go(a.go);
    else if (a.pick) onPick(a.pick);
    else if (a.pickMany) onPick(a.pickMany);
    else if (a.save) save();
  };
  const state = { mode, multiple, dir, row, chosen, name: saveName, writable };
  const primary = primaryAction(state);
  const choose = (e) => setChosen((c) => (c.includes(e.path) ? c.filter((x) => x !== e.path) : [...c, e.path]));
  const select = (e, ev) => {
    setCursor(e.path);
    setReplacing(false);
    if (e.dir) return;
    if (mode === 'save') { setSaveName(e.name); return; }
    if (!multiple) return;
    const range = ev?.shift && anchor.current ? rangeOf(rows, anchor.current, e.path) : null;
    if (range) { setChosen((c) => [...new Set([...c, ...range])]); return; }
    anchor.current = e.path;
    if (chosen.includes(e.path)) setCursor(null);
    choose(e);
  };
  const openRow = (e) => {
    if (e.dir) { go(e.path); return; }
    if (mode === 'save') { setSaveName(e.name); save(e.name); return; }
    if (multiple) { onPick([...new Set([...chosen, e.path])]); return; }
    onPick(e.path);
  };
  const moveTo = (e) => { if (!e) return; setCursor(e.path); show(e.path); if (mode === 'save' && !e.dir) setSaveName(e.name); };

  const onKey = (e) => {
    if (e.key === 'Escape') { if (replacing) setReplacing(false); else onClose(); return; }
    if (e.key === 'Enter') { act(enterAction(state)); return; }
    if (e.alt && e.key === 'ArrowLeft') { goBack(); return; }
    if (e.alt && e.key === 'ArrowRight') { goForward(); return; }
    const by = thumbs ? gridStep(e.key, cols, direction() === 'rtl') : 0;
    if (by) { moveTo(stepRow(rows, cursor, by)); return; }
    if (e.key === 'ArrowDown') { moveTo(stepRow(rows, cursor, 1)); return; }
    if (e.key === 'ArrowUp') { moveTo(stepRow(rows, cursor, -1)); return; }
    if (e.key === 'Home') { moveTo(rows[0]); return; }
    if (e.key === 'End') { moveTo(rows[rows.length - 1]); return; }
    if (e.char === ' ' && multiple && row && !row.dir) { choose(row); return; }
    // typing the start of a name selects it
    if (e.char && !e.ctrl && !e.meta && e.char.trim()) {
      const now = Date.now();
      const text = (now - typed.current.at < 900 ? typed.current.text : '') + e.char.toLowerCase();
      typed.current = { text, at: now };
      moveTo(rows.find((x) => x.name.toLowerCase().startsWith(text)));
    }
  };
  // The dialog holds the keys as a field would, so the app under it doesn't get them too. A press
  // anywhere else lets go; a press on the name field is the field's own.
  const token = useRef({}).current;
  const keys = useRef(onKey);
  keys.current = onKey;
  const hold = () => takeTextFocus(token, (e) => keys.current(e));
  useEffect(() => {
    if (mode !== 'save') hold();
    const off = modal ? addListener('pointerup', () => { if (!hasTextFocus()) hold(); }) : null;
    return () => { off?.(); releaseTextFocus(token); };
  }, []);

  const narrow = width < 560;
  // Cloud's accounts (docs/CLOUD.md): their folders, between Favorites and Locations; in one, no thumbnails
  // (making one would bring each file down from the cloud)
  const cloudSt = useStore(cloudState);
  const clouds = cloudPlaces(cloudSt && cloudSt.accounts, home, settings.get().cloudHidden || []);
  const inCloud = !!cloudRoot(dir, home);
  const showDate = width - (narrow ? 0 : SIDE_W) > 520;
  const crumbs = pathCrumbs(dir, home).map((c) => ({ label: c.home ? t('files.place.home') : c.label, folder: true, path: c.path }));
  const shown = fitCrumbs(crumbs);
  const label = replacing ? t('shell.picker.replace')
    : accept || (mode === 'save' ? t('shell.picker.save') : mode === 'folder' ? t('shell.picker.chooseFolder')
      : multiple ? t('shell.picker.choose', { n: chosen.length }) : t('shell.picker.open'));
  const place = (labelText, path, icon) => <SideItem key={path} t={th} label={labelText} icon={icon} selected={dir === path} onClick={() => go(path)} />;
  const rtl = direction() === 'rtl';
  const onSort = (k) => setSort((s) => ({ by: k, desc: s.by === k && !s.desc }));
  return (
    <View ref={root} onMouseDown={(e) => { e.stopPropagation?.(); hold(); }} style={{ position: 'absolute', left: 30, top: 40, right: 30, bottom: 30, flexDirection: 'row', borderRadius: 12, overflow: 'hidden',
      backgroundColor: th.windowBg, border: `1px solid ${th.separator}`, boxShadow: '0 16px 40px rgba(0,0,0,0.45)' }}>
      {!narrow && (
        <View style={{ width: SIDE_W, backgroundColor: th.sidebarBg }}>
          <Hairline edge="end" />
          <ScrollView style={{ flex: 1 }}>
            <View style={{ paddingHorizontal: 10, paddingBottom: 12 }}>
              <SideTitle t={th} first>{t('files.favorites')}</SideTitle>
              {place(t('files.place.home'), home, <PlaceGlyph ch="⌂" t={th} />)}
              {dirs.map((d) => place(t(`files.place.${d.label}`), d.path, <FolderGlyph size={16} />))}
              {clouds.length ? <SideTitle t={th}>{t('cloud.title')}</SideTitle> : null}
              {clouds.map((c) => place(c.name, c.path, <CloudGlyph kind="cloud" size={16} color={th.accent} />))}
              {disks.length ? <SideTitle t={th}>{t('files.locations')}</SideTitle> : null}
              {disks.map((v) => place(v.name, v.path, <DriveGlyph color={th.accent} />))}
            </View>
          </ScrollView>
        </View>
      )}
      <View style={{ flex: 1, minWidth: 0 }}>
        {title ? (
          <View style={{ height: 30, justifyContent: 'center', paddingHorizontal: spacing.m }}>
            <Text style={{ fontSize: typeScale.body, fontWeight: 600, color: th.text, textAlign: 'center', whiteSpace: 'nowrap', overflow: 'hidden' }}>{title}</Text>
          </View>
        ) : null}
        <Toolbar>
          <ToolbarGroup>
            <ToolbarButton icon={rtl ? '›' : '‹'} iconSize={20} small title={t('files.menu.back')} onClick={goBack} disabled={!nav.back.length} />
            <ToolbarButton icon={rtl ? '‹' : '›'} iconSize={20} small title={t('files.menu.forward')} onClick={goForward} disabled={!nav.fwd.length} />
          </ToolbarGroup>
          <View style={{ flex: 1, minWidth: 0, flexDirection: 'row', alignItems: 'center', gap: spacing.xs, overflow: 'hidden' }}>
            <PathCrumbs t={th} crumbs={shown} onGo={(i) => go(shown[i].path)} />
          </View>
          <ToolbarGroup>
            <ToolbarButton icon="≡" iconSize={14} small title={t('shell.picker.viewList')} active={!thumbs} onClick={() => setView('list')} />
            <ToolbarButton icon="▦" iconSize={14} small title={t('shell.picker.viewThumbs')} active={thumbs} onClick={() => setView('thumbs')} />
          </ToolbarGroup>
        </Toolbar>
        {thumbs ? <View style={{ height: 1, backgroundColor: th.separator }} /> : (
          <View style={{ flexDirection: 'row', height: 27, alignItems: 'center', paddingHorizontal: 18, borderBottomWidth: 1, borderBottomColor: th.separator }}>
            <SortTitle k="name" sort={sort} onSort={onSort} t={th} />
            {showDate && <SortTitle k="date" width={DATE_W} sort={sort} onSort={onSort} t={th} />}
            <SortTitle k="size" width={SIZE_W} sort={sort} onSort={onSort} t={th} />
          </View>
        )}
        <View ref={listBox} style={{ flex: 1, minHeight: 0 }} onMouseDown={() => { setCursor(null); setReplacing(false); }}>
          <ScrollView scrollRef={scroll} style={{ flex: 1 }} onScroll={(y, h) => setSight((v) => (v.y === y && v.h === h ? v : { y, h }))}>
            {thumbs ? (
              <View style={{ paddingHorizontal: GRID_PAD, paddingBottom: 12, flexDirection: 'row', flexWrap: 'wrap' }}>
                {(() => {
                  const [from, to] = liveRange(sight.y, sight.h, TILE_H, cols, rows.length);
                  return rows.map((e, i) => (
                    <Tile key={e.path} e={e} th={th} on={e.path === cursor || chosen.includes(e.path)} live={i >= from && i < to && !inCloud}
                      onDown={(ev) => { ev.stopPropagation(); hold(); select(e, ev); }} onOpen={(ev) => { ev.stopPropagation(); openRow(e); }} />
                  ));
                })()}
                {!rows.length && <EmptyState title={t('shell.picker.empty')} style={{ minHeight: 180, flex: 1 }} />}
              </View>
            ) : (
            <View style={{ paddingHorizontal: 8, paddingTop: 4, paddingBottom: 12 }}>
              {rows.map((e, i) => {
                const on = e.path === cursor || chosen.includes(e.path);
                return (
                  <View key={e.path} onMouseDown={(ev) => { ev.stopPropagation(); hold(); select(e, ev); }} onDoubleClick={(ev) => { ev.stopPropagation(); openRow(e); }} style={listRowStyle(th, on, i % 2 === 1)}>
                    <View style={{ flex: 1, minWidth: 0, flexDirection: 'row', alignItems: 'center', gap: 6 }}>
                      <EntryIcon e={e} size={16} dark={th.mode === 'dark'} thumb={false} />
                      <Text style={{ flex: 1, fontSize: 13, lineHeight: 16, color: on ? '#fff' : th.text, textAlign: 'start', whiteSpace: 'nowrap', overflow: 'hidden', textOverflow: 'ellipsis' }}>{e.name}</Text>
                    </View>
                    {showDate && <Text style={listCell(th, on, DATE_W, 'start')}>{when(e.mtime)}</Text>}
                    <Text style={listCell(th, on, SIZE_W, 'end')}>{e.dir ? '—' : formatBytes(e.size)}</Text>
                  </View>
                );
              })}
              {!rows.length && <EmptyState title={t('shell.picker.empty')} style={{ minHeight: 180 }} />}
            </View>
            )}
          </ScrollView>
        </View>
        <View style={{ flexDirection: 'row', alignItems: 'center', gap: spacing.s, padding: spacing.m, backgroundColor: th.mode === 'dark' ? 'rgba(0,0,0,0.12)' : 'rgba(0,0,0,0.025)' }}>
          <Hairline edge="top" />
          {mode === 'save' ? (
            <View onMouseDown={(e) => e.stopPropagation?.()} style={{ flex: 1, minWidth: 0 }}>
              <TextInput value={saveName} placeholder={t('shell.picker.fileName')} autoFocus selectAll
                onChange={(v) => { setSaveName(v); setReplacing(false); }} onSubmit={() => save()} onCancel={() => (replacing ? setReplacing(false) : onClose())} />
            </View>
          ) : (
            <Text style={{ flex: 1, minWidth: 0, fontSize: typeScale.caption, color: th.textDim, whiteSpace: 'nowrap', overflow: 'hidden', textAlign: 'start' }}>{row && !row.dir ? row.name : t('files.items', { count: rows.length })}</Text>
          )}
          <Button label={t('shell.picker.cancel')} onClick={onClose} />
          <Button primary label={label} disabled={!primary} onClick={() => act(primary)} />
        </View>
        {replacing || (mode === 'save' && !writable) ? (
          <Text style={{ paddingHorizontal: spacing.m, paddingBottom: spacing.s, fontSize: typeScale.caption, color: th.textDim, textAlign: 'end', backgroundColor: th.mode === 'dark' ? 'rgba(0,0,0,0.12)' : 'rgba(0,0,0,0.025)' }}>
            {replacing ? t('shell.picker.exists') : t('shell.picker.readOnly')}
          </Text>
        ) : null}
      </View>
    </View>
  );
}

// ---- A Linux app asks the desktop for a file (org.freedesktop.portal.FileChooser, server/src/ui/portal.c) ----
// The home folder and the local disks, like every other picker here. The app gets only what the person picks.
export const portalFile = createStore(null);

const globPart = (c) => (c === '*' ? '.*' : c === '?' ? '.' : c.replace(/[.+^${}()|[\]\\]/g, '\\$&'));
const portalFilter = (patterns) => {
  if (!patterns?.length || patterns.includes('*')) return null;
  return new RegExp(`^(${patterns.map((p) => [...p].map(globPart).join('')).join('|')})$`, 'i');
};
export function PortalFilePrompt() {
  const p = useStore(portalFile);
  // the system layer is outside the theme's ThemeProvider: the dialog takes the same tokens as Files
  const tokens = useStore(shellTokens);
  if (!p) return null;
  const answer = (paths) => lord.portal.chosen(p.id, paths);
  return (
    <ThemeContext.Provider value={tokens || DEFAULT_TOKENS}>
      <View style={{ position: 'absolute', left: 0, top: 0, right: 0, bottom: 0, alignItems: 'center', justifyContent: 'center', backgroundColor: 'rgba(0,0,0,0.35)' }}>
        <View style={{ position: 'relative', width: 920, maxWidth: '96%', height: 620, maxHeight: '94%' }}>
          <FilePicker key={p.id} modal mode={p.mode} multiple={p.multiple} title={p.title} name={p.name} accept={p.accept} start={p.folder}
            filter={portalFilter(p.patterns)} onClose={() => answer(null)} onPick={(x) => answer(Array.isArray(x) ? x : [x])} />
        </View>
      </View>
    </ThemeContext.Provider>
  );
}
