// Passwords — the LordOS password manager: items by category (a tree, with field templates) and
// tags, favorites, archived, trash, history, 2FA codes, Wi-Fi with QR, secure notes, cards, the
// security report, the generator, import/export and the session keyring. Everything decrypted is
// handled by lord-vault, sandboxed and without network; this window is protected (black in
// screenshots, recordings and the remote desktop). See docs/PASSWORDS.md.
// Layout (docs/DESIGN.md): a source list (search, sections, categories, tags), the item list and
// the detail pane; under 960 px the columns get narrower so nothing is clipped.
import {
  View, Text, useTheme, useStore, useAppWindow, useKeyboard, Button, ScrollView, SearchField, SectionTitle, ToolbarButton, EmptyState,
  hasTextFocus, direction, wm, t, vaultAvailable, ContextMenu,
} from '@lordos/sdk';
import { useEffect, useMemo, useRef, useState } from 'react';
import { app, req, refreshStatus, wireEvents, reloadLogin, keepCopy, toast, errorText } from './api.js';
import { allCategories, builtinById, catName } from './templates.js';
import { CatIcon, EntryIcon, Overlays } from './ui.jsx';
import { Glyph } from './glyphs.jsx';
import { UnlockView, CreateView } from './Lock.jsx';
import { EntryView, EntryEditor, tagColor, copyMenuItems } from './Detail.jsx';
import { SecurityView, CodesView } from './Security.jsx';
import { GeneratorSheet } from './Generator.jsx';
import { SettingsSheet, ImportSheet, ExportSheet, CategorySheet, TagSheet } from './Settings.jsx';

// ---- the app icon: a padlock with three dots, our own ----

export function Icon({ size: s }) {
  return (
    <View style={{ width: s, height: s, borderRadius: s * 0.225, background: 'linear-gradient(160deg, #3fd2c7, #1f6fd1)', overflow: 'hidden',
      boxShadow: `0 ${s * 0.04}px ${s * 0.12}px rgba(0,0,0,0.35)` }}>
      {/* shackle (absolute children are placed by left/top, as in the other app icons) */}
      <View style={{ position: 'absolute', left: s * 0.3, top: s * 0.14, width: s * 0.4, height: s * 0.4, borderRadius: s * 0.2, border: `${Math.max(2, s * 0.075)}px solid #ffffff` }} />
      {/* body */}
      <View style={{ position: 'absolute', left: s * 0.19, top: s * 0.38, width: s * 0.62, height: s * 0.44, borderRadius: s * 0.1, backgroundColor: '#ffffff',
        flexDirection: 'row', alignItems: 'center', justifyContent: 'center', gap: s * 0.05, boxShadow: `0 ${s * 0.02}px ${s * 0.05}px rgba(0,0,0,0.25)` }}>
        {[0, 1, 2].map((i) => <View key={i} style={{ width: s * 0.09, height: s * 0.09, borderRadius: s * 0.045, backgroundColor: '#1f6fd1' }} />)}
      </View>
    </View>
  );
}

// ---- source list ----

// A source-list row (as the SDK's SidebarItem): 28 px, a glyph or tile, the label and a count;
// twisty: a disclosure triangle for categories with children.
function SideRow({ icon, label, count, selected, onClick, depth = 0, color, onDoubleClick, twisty, onTwisty }) {
  const th = useTheme();
  const [hover, setHover] = useState(false);
  const glyph = typeof icon === 'string' ? <Glyph kind={icon} size={15} color={selected ? '#fff' : th.accent} /> : icon;
  return (
    <View onClick={onClick} onDoubleClick={onDoubleClick} onMouseEnter={() => setHover(true)} onMouseLeave={() => setHover(false)}
      style={{ height: 28, paddingInlineStart: 8 + depth * 16, paddingInlineEnd: 10, borderRadius: 6, flexDirection: 'row', alignItems: 'center', gap: 8,
        backgroundColor: selected ? th.accent : hover ? th.hover : 'transparent' }}>
      {twisty != null ? (
        <View onClick={(e) => { e.stopPropagation?.(); onTwisty?.(); }} style={{ width: 12, height: 20, marginInlineEnd: -4, alignItems: 'center', justifyContent: 'center' }}>
          <Text style={{ fontSize: 8, color: selected ? '#fff' : th.textDim, transform: twisty ? 'rotate(90deg)' : direction() === 'rtl' ? 'rotate(180deg)' : undefined }}>▶</Text>
        </View>
      ) : depth ? <View style={{ width: 12, marginInlineEnd: -4 }} /> : null}
      {color ? <View style={{ width: 18, alignItems: 'center' }}><View style={{ width: 9, height: 9, borderRadius: 5, backgroundColor: color }} /></View> : glyph}
      <Text style={{ flex: 1, minWidth: 0, fontSize: 13, fontWeight: 500, color: selected ? '#fff' : th.text, whiteSpace: 'nowrap', textOverflow: 'ellipsis' }}>{label}</Text>
      {count != null && count > 0 && <Text style={{ fontSize: 11, color: selected ? 'rgba(255,255,255,0.85)' : th.textDim }}>{String(count)}</Text>}
    </View>
  );
}

function inCat(entryCat, catId, cats) {
  if (entryCat === catId) return true;
  let c = cats.find((x) => x.id === entryCat);
  for (let guard = 0; c && c.parent && guard < 16; guard++) {
    if (c.parent === catId) return true;
    c = cats.find((x) => x.id === c.parent);
  }
  return false;
}

function Sidebar({ sel, onSel, issues, onNewCat, onEditCat, onEditTag }) {
  const th = useTheme();
  const st = useStore(app);
  const cats = allCategories(st.cats);
  const live = st.entries.filter((e) => !e.trash);
  const [open, setOpen] = useState({});
  const count = (fn) => live.filter(fn).length;
  const tags = [...new Set(live.flatMap((e) => e.tags || []))].sort((a, b) => a.localeCompare(b));
  const childrenOf = (pid) => cats.filter((c) => (c.parent || '') === pid);
  const tree = (pid, depth) => childrenOf(pid).map((c) => {
    const n = count((e) => inCat(e.cat, c.id, cats));
    const kids = childrenOf(c.id);
    if (c.builtin && !n && !kids.length && sel !== `cat:${c.id}`) return null;     // empty built-in ones take no room
    const isOpen = open[c.id] !== false;
    return [
      <SideRow key={c.id} icon={<CatIcon cat={c} size={18} />} label={catName(c)} count={n} depth={depth} selected={sel === `cat:${c.id}`}
        onClick={() => onSel(`cat:${c.id}`)} onDoubleClick={() => !c.builtin && onEditCat(c)}
        twisty={kids.length ? isOpen : null} onTwisty={() => setOpen((o) => ({ ...o, [c.id]: !isOpen }))} />,
      ...(kids.length && isOpen ? tree(c.id, depth + 1) : []),
    ];
  });
  return (
    <ScrollView style={{ flex: 1 }}>
      <View style={{ paddingHorizontal: 10, paddingBottom: 12, gap: 2 }}>
        <SideRow icon="lock" label={t('passwords.side.all')} count={count((e) => !e.arch)} selected={sel === 'all'} onClick={() => onSel('all')} />
        <SideRow icon="star" label={t('passwords.side.fav')} count={count((e) => e.fav)} selected={sel === 'fav'} onClick={() => onSel('fav')} />
        <SideRow icon="clock" label={t('passwords.side.codes')} count={count((e) => e.totp)} selected={sel === 'codes'} onClick={() => onSel('codes')} />
        <SideRow icon="shield" label={t('passwords.side.security')} count={issues} selected={sel === 'security'} onClick={() => onSel('security')} />
        <SideRow icon="wifi" label={t('passwords.side.wifi')} count={count((e) => inCat(e.cat, 'wifi', cats))} selected={sel === 'cat:wifi'} onClick={() => onSel('cat:wifi')} />
        <SideRow icon="note" label={t('passwords.side.notes')} count={count((e) => inCat(e.cat, 'notes', cats))} selected={sel === 'cat:notes'} onClick={() => onSel('cat:notes')} />
        <SideRow icon="card" label={t('passwords.side.cards')} count={count((e) => inCat(e.cat, 'cards', cats))} selected={sel === 'cat:cards'} onClick={() => onSel('cat:cards')} />
        <SideRow icon="archive" label={t('passwords.side.archived')} count={count((e) => e.arch)} selected={sel === 'archived'} onClick={() => onSel('archived')} />
        <View style={{ flexDirection: 'row', alignItems: 'center', marginTop: 8 }}>
          <View style={{ flex: 1 }}><SectionTitle>{t('passwords.side.categories')}</SectionTitle></View>
          <ToolbarButton small icon="+" iconSize={15} color={th.textDim} title={t('passwords.menu.newCat')} onClick={onNewCat} style={{ marginTop: 6 }} />
        </View>
        {tree('', 0)}
        {tags.length > 0 && <View style={{ marginTop: 8 }}><SectionTitle>{t('passwords.side.tags')}</SectionTitle></View>}
        {tags.map((tg) => (
          <SideRow key={tg} color={tagColor(tg)} label={tg} count={count((e) => (e.tags || []).includes(tg))} selected={sel === `tag:${tg}`}
            onClick={() => onSel(`tag:${tg}`)} onDoubleClick={() => onEditTag(tg)} />
        ))}
        <View style={{ marginTop: 8 }}><SectionTitle>{t('passwords.side.system')}</SectionTitle></View>
        <SideRow icon="person" label={t('passwords.side.login')} count={st.login.entries.length} selected={sel === 'login'} onClick={() => { reloadLogin(); onSel('login'); }} />
        <SideRow icon="trash" label={t('passwords.side.trash')} count={st.entries.filter((e) => e.trash).length} selected={sel === 'trash'} onClick={() => onSel('trash')} />
      </View>
    </ScrollView>
  );
}

// ---- item list ----

function ListRow({ e, cat, selected, onClick, onContextMenu }) {
  const th = useTheme();
  const [hover, setHover] = useState(false);
  const dim = selected ? 'rgba(255,255,255,0.82)' : th.textDim;
  return (
    <View onClick={onClick} onContextMenu={onContextMenu} onMouseEnter={() => setHover(true)} onMouseLeave={() => setHover(false)}
      style={{ height: 48, flexDirection: 'row', alignItems: 'center', gap: 10, paddingHorizontal: 10, borderRadius: 8,
        backgroundColor: selected ? th.accent : hover ? th.hover : 'transparent' }}>
      <EntryIcon entry={e} cat={cat} size={30} />
      <View style={{ flex: 1, minWidth: 0, gap: 1 }}>
        <Text style={{ fontSize: 13, fontWeight: 600, color: selected ? '#fff' : th.text, whiteSpace: 'nowrap', textOverflow: 'ellipsis' }}>{e.title || t('passwords.untitled')}</Text>
        <Text style={{ fontSize: 12, color: dim, whiteSpace: 'nowrap', textOverflow: 'ellipsis' }}>{e.sub || catName(cat)}</Text>
      </View>
      {e.natt ? <Glyph kind="clip" size={13} color={dim} /> : null}
      {e.totp && <Text style={{ fontSize: 10, fontWeight: 600, color: dim }}>2FA</Text>}
      {e.fav && <Text style={{ fontSize: 12, color: selected ? '#fff' : '#ffb300' }}>★</Text>}
    </View>
  );
}

// The width of a box after layout (measured again on every render).
function useWidth(ref, initial) {
  const [w, setW] = useState(initial);
  useEffect(() => {
    const id = setTimeout(() => {
      try { if (ref.current) { const nw = lord.getLayout(ref.current.id)[2]; if (nw && Math.abs(nw - w) > 0.5) setW(nw); } } catch (e) { /* not laid out */ }
    }, 20);
    return () => clearTimeout(id);
  });
  return w;
}

// ---- window ----

export default function Passwords() {
  const th = useTheme();
  const win = useAppWindow();
  const st = useStore(app);
  const [sel, setSel] = useState('all');
  const [cur, setCur] = useState(null);           // id of the open item
  const [editing, setEditing] = useState(null);   // null | { entry } | { new: catId }
  const [q, setQ] = useState('');
  const [hits, setHits] = useState(null);         // ids found by the search
  const [searchMs, setSearchMs] = useState(null);
  const [sort, setSort] = useState('title');
  const [sheet, setSheet] = useState(null);       // gen | settings | import | export | cat | tag
  const [sheetArg, setSheetArg] = useState(null);
  const [issues, setIssues] = useState(0);
  const [ctx, setCtx] = useState(null);         // the list's right-click menu: { x, y, items }
  const rootRef = useRef(null);
  const winW = useWidth(rootRef, 1040);

  // protected: black in screenshots, recordings, the remote desktop and thumbnails
  useEffect(() => { if (win?.id) wm.setProtected(win.id, true); }, [win?.id]);
  useEffect(() => { wireEvents(); refreshStatus(); }, []);
  useEffect(() => {
    if (st.phase !== 'unlocked') return;
    req('report').then((r) => setIssues(r.weak.length + r.reused.reduce((a, g) => a + g.length, 0) + r.old.length)).catch(() => {});
  }, [st.phase, st.entries.length]);
  useEffect(() => { if (st.phase !== 'unlocked') { setCur(null); setEditing(null); setSheet(null); } }, [st.phase]);
  useEffect(() => {
    if (!q.trim() || sel === 'login') { setHits(null); return undefined; }
    const id = setTimeout(() => {
      req('search', { q }).then((r) => { setHits(new Set(r.ids)); setSearchMs(r.ms); }).catch(() => setHits(null));
    }, 60);
    return () => clearTimeout(id);
  }, [q, sel, st.entries]);

  const lock = () => req('lock').then(refreshStatus).catch(() => {});
  const newEntry = () => {
    const catId = sel.startsWith('cat:') ? sel.slice(4) : sel === 'login' || sel === 'trash' || sel === 'security' || sel === 'codes' ? 'internet' : 'internet';
    setEditing({ new: catId });
    setCur(null);
  };
  const openSheet = (s, arg = null) => { setSheet(s); setSheetArg(arg); };
  // right click on an item: copy any of it without opening it
  const rowMenu = async (e, ev) => {
    ev.stopPropagation?.();
    const at = rootRef.current && ev.local ? ev.local(rootRef.current) : ev;
    try {
      const items = await copyMenuItems(v, e.id);
      if (items.length) setCtx({ x: at.x, y: at.y, items });
    } catch (x) { toast(errorText(x.code)); }
  };

  useEffect(() => {
    if (!win?.id) return;
    const unlocked = st.phase === 'unlocked';
    wm.setMenus(win.id, [
      { label: t('passwords.menu.file'), items: [
        { label: t('passwords.menu.new'), hint: '❖N', onClick: newEntry, disabled: !unlocked },
        { label: t('passwords.menu.newCat'), onClick: () => openSheet('cat'), disabled: !unlocked }, '-',
        { label: t('passwords.menu.import'), onClick: () => openSheet('import'), disabled: !unlocked },
        { label: t('passwords.menu.export'), onClick: () => openSheet('export'), disabled: !unlocked }, '-',
        { label: t('passwords.menu.lock'), hint: '❖⇧L', onClick: lock, disabled: !unlocked },
      ] },
      { label: t('passwords.menu.tools'), items: [
        { label: t('passwords.menu.generator'), hint: '❖G', onClick: () => openSheet('gen'), disabled: !unlocked },
        { label: t('passwords.menu.security'), onClick: () => setSel('security'), disabled: !unlocked },
        { label: t('passwords.menu.settings'), hint: '❖,', onClick: () => openSheet('settings'), disabled: !unlocked },
      ] },
    ]);
  }, [win?.id, st.phase, sel]);

  // ↑ / ↓ move the selection in the item list (when no text field has the focus)
  const listRef = useRef([]);
  useKeyboard((e) => {
    if (st.phase !== 'unlocked') return;
    if (!e.meta && !e.ctrl && (e.key === 'ArrowDown' || e.key === 'ArrowUp') && !hasTextFocus() && !editing && !sheet) {
      const l = listRef.current;
      if (!l.length) return;
      const i = l.findIndex((x) => x.id === cur);
      const n = e.key === 'ArrowDown' ? Math.min(l.length - 1, i + 1) : Math.max(0, i < 0 ? 0 : i - 1);
      setCur(l[n].id);
      return;
    }
    if (!e.meta) return;
    const k = String(e.base || e.key || '').toLowerCase();
    if (k === 'n') newEntry();
    else if (k === 'g') openSheet('gen');
    else if (k === ',') openSheet('settings');
    else if (k === 'l' && e.shift) lock();
  });

  const cats = useMemo(() => allCategories(st.cats), [st.cats]);
  const wrap = (el) => <View ref={rootRef} style={{ flex: 1 }}>{el}<Overlays /></View>;
  if (!vaultAvailable() || st.phase === 'unavailable') return <EmptyState icon={<Glyph kind="lock" size={40} color={th.textDim} />} title={t('passwords.unavailable')} />;
  if (st.phase === 'loading') return <View style={{ flex: 1, backgroundColor: th.windowBg }} />;
  if (st.phase === 'none') return wrap(<CreateView />);
  if (st.phase === 'locked') return wrap(<UnlockView />);

  const v = sel === 'login' ? 'login' : 'main';
  const base = sel === 'login' ? st.login.entries : st.entries;
  let list = base.filter((e) => {
    if (sel === 'login') return true;
    if (sel === 'trash') return !!e.trash;
    if (e.trash) return false;
    if (sel === 'all') return !e.arch;
    if (sel === 'fav') return e.fav;
    if (sel === 'archived') return e.arch;
    if (sel.startsWith('cat:')) return inCat(e.cat, sel.slice(4), cats);
    if (sel.startsWith('tag:')) return (e.tags || []).includes(sel.slice(4));
    return true;
  });
  if (q.trim()) {
    if (sel === 'login') { const qq = q.toLowerCase(); list = list.filter((e) => `${e.title} ${e.sub} ${e.url || ''}`.toLowerCase().includes(qq)); }
    else if (hits) list = list.filter((e) => hits.has(e.id));
  }
  list = [...list].sort(sort === 'title' ? (a, b) => String(a.title).localeCompare(String(b.title)) : (a, b) => b.modified - a.modified);
  const title = sel === 'all' ? t('passwords.side.all') : sel === 'fav' ? t('passwords.side.fav') : sel === 'archived' ? t('passwords.side.archived')
    : sel === 'trash' ? t('passwords.side.trash') : sel === 'login' ? t('passwords.side.login') : sel.startsWith('tag:') ? `#${sel.slice(4)}`
      : sel.startsWith('cat:') ? catName(cats.find((c) => c.id === sel.slice(4)) || builtinById(sel.slice(4))) : '';
  listRef.current = list;
  const fullPane = sel === 'security' || sel === 'codes';
  const narrow = winW < 960;
  const sideW = narrow ? 200 : 220, listW = narrow ? 250 : 290;
  const catOf = (e) => cats.find((c) => c.id === e.cat) || builtinById(e.cat) || { id: e.cat };

  let detail;
  if (editing) {
    detail = <EntryEditor key={editing.entry?.id || `new-${editing.new}`} v={v} entry={editing.entry} catId={editing.new}
      onDone={(id) => { setEditing(null); setCur(id); }} onCancel={() => setEditing(null)} />;
  } else if (cur) {
    detail = <EntryView key={cur} v={v} id={cur} onEdit={(entry) => setEditing({ entry })} onDeleted={() => setCur(null)} />;
  } else {
    detail = <EmptyState icon={<Icon size={56} />} title={list.length ? t('passwords.pick') : t('passwords.list.empty')}
      hint={list.length ? t('passwords.pickHint') : t('passwords.emptyHint')} />;
  }

  const canAdd = sel !== 'login' && sel !== 'trash';
  return (
    <View ref={rootRef} style={{ flex: 1, flexDirection: 'row', backgroundColor: th.windowBg }}>
      <View style={{ width: sideW, backgroundColor: th.sidebarBg, borderInlineEndWidth: 1, borderInlineEndColor: th.separator }}>
        <View style={{ paddingHorizontal: 10, paddingTop: 10, paddingBottom: 6 }}>
          <SearchField value={q} onChange={setQ} placeholder={t('passwords.search')} />
        </View>
        <Sidebar sel={sel} onSel={(s) => { setSel(s); setCur(null); setEditing(null); }} issues={issues}
          onNewCat={() => openSheet('cat')} onEditCat={(c) => openSheet('cat', c)} onEditTag={(tg) => openSheet('tag', tg)} />
        <View style={{ height: 41, flexDirection: 'row', alignItems: 'center', gap: 4, paddingHorizontal: 10, borderTopWidth: 1, borderTopColor: th.separator }}>
          <ToolbarButton icon="⚄" iconSize={16} title={t('passwords.menu.generator')} onClick={() => openSheet('gen')} />
          <ToolbarButton icon="⚙" iconSize={15} title={t('passwords.menu.settings')} onClick={() => openSheet('settings')} />
          <View style={{ flex: 1 }} />
          <ToolbarButton icon={<Glyph kind="lock" size={14} color={th.text} />} label={t('passwords.lock')} title={t('passwords.menu.lock')} onClick={lock} />
        </View>
      </View>
      {fullPane ? (
        <View style={{ flex: 1, minWidth: 0 }}>{sel === 'security'
          ? <SecurityView onOpen={(id) => { setSel('all'); setCur(id); }} />
          : <CodesView onOpen={(id) => { setSel('all'); setCur(id); }} />}</View>
      ) : (
        <>
          <View style={{ width: listW, borderInlineEndWidth: 1, borderInlineEndColor: th.separator }}>
            <View style={{ height: 56, flexDirection: 'row', alignItems: 'center', paddingInlineStart: 16, paddingInlineEnd: 10, gap: 4 }}>
              <View style={{ flex: 1, minWidth: 0 }}>
                <Text style={{ fontSize: 17, fontWeight: 600, color: th.text, whiteSpace: 'nowrap', textOverflow: 'ellipsis' }}>{title}</Text>
                <Text style={{ fontSize: 11, color: th.textDim, whiteSpace: 'nowrap', textOverflow: 'ellipsis' }}>{q.trim() && searchMs != null && sel !== 'login'
                  ? t('passwords.list.found', { count: list.length, ms: searchMs.toFixed(1) }) : t('passwords.list.count', { count: list.length })}</Text>
              </View>
              <ToolbarButton small label={sort === 'title' ? t('passwords.list.sortName') : t('passwords.list.sortDate')} color={th.textDim}
                title={t('passwords.list.sortHint')} onClick={() => setSort(sort === 'title' ? 'modified' : 'title')} />
              {canAdd && <ToolbarButton icon="+" iconSize={17} title={t('passwords.menu.new')} onClick={newEntry} />}
            </View>
            {sel === 'login' && <Text style={{ fontSize: 11, color: th.textDim, paddingHorizontal: 16, paddingBottom: 8, lineHeight: 15 }}>{t('passwords.login.hint')}</Text>}
            {sel === 'trash' && list.length > 0 && <View style={{ paddingHorizontal: 16, paddingBottom: 8, alignItems: 'flex-start' }}>
              <Button small destructive label={t('passwords.trash.empty')} onClick={() => req('empty-trash').then(refreshStatus).catch(() => {})} /></View>}
            {list.length ? (
              <ScrollView style={{ flex: 1 }}>
                <View style={{ paddingHorizontal: 8, paddingBottom: 10, gap: 2 }}>
                  {list.slice(0, 600).map((e) => <ListRow key={e.id} e={e} cat={catOf(e)} selected={cur === e.id} onClick={() => { setCur(e.id); setEditing(null); }}
                    onContextMenu={(ev) => rowMenu(e, ev)} />)}
                  {list.length > 600 && <Text style={{ fontSize: 11, color: th.textDim, padding: 10 }}>{t('passwords.list.more', { count: list.length - 600 })}</Text>}
                </View>
              </ScrollView>
            ) : (
              <EmptyState icon={<Glyph kind={q ? 'note' : sel === 'trash' ? 'trash' : 'lock'} size={30} color={th.textDim} />}
                title={q ? t('passwords.list.noResults') : t('passwords.list.empty')} hint={q ? null : canAdd ? t('passwords.list.emptyHint') : null}
                style={{ justifyContent: 'flex-start', paddingTop: 72 }} />
            )}
          </View>
          <View style={{ flex: 1, minWidth: 0 }}>{detail}</View>
        </>
      )}
      {sheet === 'gen' && <GeneratorSheet onClose={() => setSheet(null)} />}
      {sheet === 'settings' && <SettingsSheet onClose={() => setSheet(null)} />}
      {sheet === 'import' && <ImportSheet onClose={() => setSheet(null)} />}
      {sheet === 'export' && <ExportSheet onClose={() => setSheet(null)} />}
      {sheet === 'cat' && <CategorySheet cat={sheetArg} onClose={() => setSheet(null)} />}
      {sheet === 'tag' && <TagSheet tag={sheetArg} onClose={() => setSheet(null)} />}
      <Overlays />
      {ctx && <ContextMenu x={ctx.x} y={ctx.y} items={ctx.items} onClose={() => setCtx(null)} bounds={{ w: winW, h: rootRef.current ? lord.getLayout(rootRef.current.id)[3] : 660 }} />}
      {st.clip ? (
        // the secret just copied: how long before it's cleared, and Keep
        <View style={{ position: 'absolute', bottom: 20, left: 0, right: 0, alignItems: 'center', zIndex: 80 }}>
          <View style={{ flexDirection: 'row', alignItems: 'center', gap: 10, paddingInlineStart: 14, paddingInlineEnd: 5, height: 34, borderRadius: 17,
            backgroundColor: th.mode === 'dark' ? '#3a3a3c' : '#1c1c1e', boxShadow: '0 6px 20px rgba(0,0,0,0.3)' }}>
            <Text style={{ fontSize: 12, fontWeight: 500, color: '#fff', whiteSpace: 'nowrap' }}>{t('passwords.copied', { what: st.clip.label, secs: st.clip.left })}</Text>
            <View onClick={keepCopy} style={{ paddingHorizontal: 10, height: 24, borderRadius: 12, justifyContent: 'center', backgroundColor: 'rgba(255,255,255,0.16)' }}>
              <Text style={{ fontSize: 12, fontWeight: 600, color: '#fff' }}>{t('passwords.clip.keep')}</Text>
            </View>
          </View>
        </View>
      ) : st.toast && (
        <View style={{ position: 'absolute', bottom: 20, left: 0, right: 0, alignItems: 'center', zIndex: 80, pointerEvents: 'none' }}>
          <View style={{ paddingHorizontal: 14, minHeight: 32, maxWidth: 560, justifyContent: 'center', borderRadius: 16, boxShadow: '0 6px 20px rgba(0,0,0,0.3)',
            backgroundColor: st.toast.kind === 'warn' ? '#b25000' : th.mode === 'dark' ? '#3a3a3c' : '#1c1c1e' }}>
            <Text style={{ fontSize: 12, fontWeight: 500, color: '#fff', lineHeight: 16, paddingVertical: 8 }}>{st.toast.text}</Text>
          </View>
        </View>
      )}
    </View>
  );
}
