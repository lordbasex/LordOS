// Notes — advanced sticky notes: markdown with reading view and outline, images, attachments, emojis and
// notes that live on the desktop as widgets (they move, resize and come back on
// login). Everything is saved automatically in ~/Notes/<note>/nota.md (see store.js and docs/NOTES.md).
import {
  View, Text, useTheme, useLocale, useAppWindow, useStore, useKeyboard, Button, TextInput, ScrollView, ThemeProvider, Segmented, speechItems, registerSelection, ContextMenu,
  Toolbar, ToolbarButton, ToolbarSpacer, MenuButton, SearchField, EmptyState, textDirection,
  Markdown, markdownHeadings, toggleTask, wm, fs, formatBytes, addListener, screen, t, print, FilePicker as SysFilePicker,
} from '@lordos/sdk';
import { useEffect, useRef, useState } from 'react';
import { NoteEditor } from './NoteEditor.jsx';
import { CATEGORIES, EMOJIS, emojiFor, searchEmoji } from './emoji.js';
import {
  notesStore, loadNotes, updateNote, createNote, duplicateNote, deleteNote, flushAll, attachFile, pasteClipboardImage,
  absFolder, folder, COLORS, setupWidgets, isImage,
} from './store.js';

// ---- sticky-note colors (light in both modes, with dark text) ----

const PAPER = {
  yellow: ['#fff6a8', '#fde68a'], pink: ['#ffd9e8', '#ffc2db'], green: ['#d9f6cc', '#c1edab'],
  blue: ['#d6ecff', '#bcdfff'], purple: ['#e9ddff', '#d9c6ff'], gray: ['#eeeeee', '#dedede'],
};
const ink = {
  text: '#2a2a2a', dim: '#6b6b6b', heading: '#151515', code: '#5b3fa0', accent: '#0a5fd0',
  selection: 'rgba(10,95,208,0.22)', caret: '#0a5fd0', popupBg: '#ffffff', popupText: '#222', border: 'rgba(0,0,0,0.15)',
};
// SDK tokens for drawing markdown on light paper
const paperTokens = (th) => ({ ...th, mode: 'light', text: ink.text, textDim: ink.dim, accent: ink.accent, separator: 'rgba(0,0,0,0.14)',
  hover: 'rgba(0,0,0,0.06)', control: 'rgba(0,0,0,0.08)', inputBg: 'rgba(255,255,255,0.7)' });

const noteBy = (slug) => notesStore.get().list.find((n) => n.slug === slug);

// ---- icon ----

export function Icon({ size: s }) {
  return (
    <View style={{ width: s, height: s, borderRadius: s * 0.225, background: 'linear-gradient(160deg, #ffe066, #f7b733)',
      boxShadow: `0 ${s * 0.04}px ${s * 0.12}px rgba(0,0,0,0.35)`, overflow: 'hidden' }}>
      {/* sheet with a folded corner and ruled lines */}
      <View style={{ position: 'absolute', left: s * 0.2, top: s * 0.18, width: s * 0.6, height: s * 0.64, borderRadius: s * 0.05,
        backgroundColor: '#fffbe0', boxShadow: `0 ${s * 0.02}px ${s * 0.05}px rgba(0,0,0,0.25)` }} />
      {[0, 1, 2, 3].map((i) => (
        <View key={i} style={{ position: 'absolute', left: s * 0.28, top: s * (0.32 + i * 0.11), width: s * (i === 3 ? 0.26 : 0.44), height: Math.max(1, s * 0.035),
          borderRadius: s * 0.02, backgroundColor: i === 0 ? '#e0a800' : '#c9b98a' }} />
      ))}
      <View style={{ position: 'absolute', left: s * 0.66, top: s * 0.68, width: s * 0.14, height: s * 0.14, backgroundColor: '#f3d77a',
        borderRadius: s * 0.03 }} />
    </View>
  );
}

// ---- emoji picker ----

function EmojiPicker({ onPick, onClose, x = 0, y = 0 }) {
  const th = useTheme();
  const { t: tr } = useLocale();
  const [q, setQ] = useState('');
  const [cat, setCat] = useState('caras');
  const list = (q ? searchEmoji(q) : EMOJIS.filter((e) => e.cat === cat)).slice(0, 96);
  return (
    <View onMouseDown={(e) => e.stopPropagation?.()} style={{ position: 'absolute', left: x, top: y, width: 308, padding: 10, gap: 8, borderRadius: 12,
      backgroundColor: th.windowBg, border: `1px solid ${th.separator}`, boxShadow: '0 12px 30px rgba(0,0,0,0.35)' }}>
      <TextInput value={q} onChange={setQ} placeholder={tr('notes.emoji.search')} autoFocus onCancel={onClose}
        onSubmit={() => list[0] && onPick(list[0].ch)} />
      {!q && (
        <View style={{ flexDirection: 'row', flexWrap: 'wrap', gap: 3 }}>
          {CATEGORIES.map((c) => (
            <View key={c} onClick={() => setCat(c)} style={{ paddingHorizontal: 6, paddingVertical: 2, borderRadius: 5, backgroundColor: c === cat ? th.accent : th.control }}>
              <Text style={{ fontSize: 10, color: c === cat ? '#fff' : th.text }}>{tr(`notes.emoji.cat.${c}`)}</Text>
            </View>
          ))}
        </View>
      )}
      <View style={{ flexDirection: 'row', flexWrap: 'wrap', gap: 2 }}>
        {list.map((e) => (
          <View key={e.ch + e.name} onClick={() => onPick(e.ch)} style={{ width: 32, height: 30, alignItems: 'center', justifyContent: 'center', borderRadius: 6 }}>
            <Text style={{ fontSize: 18 }}>{e.ch}</Text>
          </View>
        ))}
      </View>
      <Button label={tr('notes.close')} onClick={onClose} style={{ alignSelf: 'flex-end' }} />
    </View>
  );
}

// ---- pick a file (attach, insert image): the system file dialog (sdk/filepicker.jsx), drawn like Files ----

const IMAGE_FILE = { test: (name) => isImage(name) };
function FilePicker({ onPick, onClose, images }) {
  const { t: tr } = useLocale();
  return (
    <View onMouseDown={(e) => e.stopPropagation?.()} style={{ position: 'absolute', left: 0, top: 0, right: 0, bottom: 0, backgroundColor: 'rgba(0,0,0,0.2)' }}>
      <SysFilePicker title={tr(images ? 'notes.image' : 'notes.attach')} filter={images ? IMAGE_FILE : null}
        onClose={onClose} onPick={(p) => onPick(Array.isArray(p) ? p[0] : p)} />
    </View>
  );
}

// ---- open a note's links and attachments ----

function openLink(slug, url) {
  if (/^https?:/i.test(url)) return;                     // there is no browser yet: the link is shown
  const rel = url.startsWith('/') ? url.replace(`${fs.home()}/`, '') : `${folder(slug)}/${url}`;
  if (/\.(txt|md|js|jsx|ts|json|c|h|py|sh|css|html|ya?ml|toml|ini|csv|log)$/i.test(rel)) wm.open('editor', { args: { path: `${fs.home()}/${rel}` } });
  else wm.open('files', { new: true, args: { path: `${fs.home()}/${rel.split('/').slice(0, -1).join('/')}` } });
}

// ---- note body: reading (with outline) or editing ----

function NoteBody({ note, mode, onMode, editorApi, compact, showToc, size = 14, onCommand }) {
  const th = useTheme();
  const scroll = useRef(null);
  const heads = useRef(new Map());
  const [w, setW] = useState(300);
  const box = useRef(null);
  useEffect(() => {
    const id = setInterval(() => { if (box.current) { const nw = lord.getLayout(box.current.id)[2]; if (Math.abs(nw - w) > 1) setW(nw); } }, 400);
    return () => clearInterval(id);
  });
  // right click while editing: Speech, for the selection or the whole note
  const [ctx, setCtx] = useState(null);
  const speechText = () => editorApi.current?.selection?.() || note.body || '';
  if (mode === 'edit') {
    return (
      <View ref={box} style={{ flex: 1 }}>
        <NoteEditor value={note.body} onChange={(body) => updateNote(note.slug, { body })} size={size} colors={ink} autoFocus
          apiRef={editorApi} emojiSearch={searchEmoji} padding={compact ? 8 : 14}
          onPasteImage={() => pasteClipboardImage(note.slug)} onEscape={() => onMode('read')} onBlur={compact ? () => onMode('read') : undefined}
          onCommand={(b) => { if (b === 'e') onMode('read'); else onCommand?.(b); }}
          onContextMenu={(e) => { const items = speechItems(speechText).filter((x) => x !== '-'); if (items.length) setCtx({ ...(box.current ? e.local(box.current) : e), items }); }} />
        {ctx && <ContextMenu x={ctx.x} y={ctx.y} items={ctx.items} onClose={() => setCtx(null)} bounds={{ w, h: box.current ? lord.getLayout(box.current.id)[3] : 600 }} />}
      </View>
    );
  }
  const toc = markdownHeadings(note.body);
  const jump = (id) => {
    const n = heads.current.get(id), sc = scroll.current;
    if (!n || !sc?.inner?.current) return;
    sc.scrollTo(lord.getLayout(n.id)[1] - lord.getLayout(sc.inner.current.id)[1] - 4);
  };
  return (
    // the note's own direction (an Arabic note reads right to left even in a Spanish interface)
    <View ref={box} style={{ flex: 1, flexDirection: 'row', direction: textDirection(note.body) }}>
      {showToc && toc.length > 1 && (
        <View style={{ width: 180, paddingTop: 20, paddingHorizontal: 16, gap: 2 }}>
          <View style={{ position: 'absolute', right: 0, top: 16, bottom: 16, width: 1, backgroundColor: th.separator }} />
          <Text style={{ fontSize: 11, fontWeight: 600, color: th.textDim, marginBottom: 6 }}>{t('notes.toc')}</Text>
          {toc.map((h) => (
            <View key={h.id} onClick={() => jump(h.id)} style={{ paddingVertical: 4, paddingLeft: (h.level - 1) * 10 }}>
              <Text style={{ fontSize: 12, color: h.level === 1 ? th.text : th.textDim, fontWeight: h.level === 1 ? 600 : 400, whiteSpace: 'nowrap', overflow: 'hidden' }}>{h.text}</Text>
            </View>
          ))}
        </View>
      )}
      <ScrollView scrollRef={scroll} style={{ flex: 1 }}>
        <View onDoubleClick={() => onMode('edit')} style={{ paddingVertical: compact ? 10 : 20, paddingHorizontal: compact ? 10 : 28, minHeight: 60 }}>
          {note.body.trim() ? (
            <Markdown source={note.body} base={absFolder(note.slug)} width={Math.max(80, w - (showToc && toc.length > 1 ? 180 : 0) - (compact ? 20 : 56))} size={size}
              emoji={emojiFor} onLink={(u) => openLink(note.slug, u)} onHeadingRef={(id, n) => { if (n) heads.current.set(id, n); }}
              onToggleTask={(line) => updateNote(note.slug, { body: toggleTask(note.body, line) })} />
          ) : <Text style={{ fontSize: 13, color: th.textDim }}>{t('notes.empty')}</Text>}
        </View>
      </ScrollView>
    </View>
  );
}

// ---- note on the desktop ----

function Dot({ color, selected, onClick, size = 12 }) {
  return <View onClick={onClick} style={{ width: size, height: size, borderRadius: size / 2, backgroundColor: PAPER[color][0], border: `1.5px solid ${selected ? '#333' : 'rgba(0,0,0,0.25)'}` }} />;
}

function HeadBtn({ label, onClick, title }) {
  const [hov, setHov] = useState(false);
  return (
    <View onClick={(e) => { e.stopPropagation?.(); onClick(); }} onMouseEnter={() => setHov(true)} onMouseLeave={() => setHov(false)}
      style={{ minWidth: 20, height: 20, paddingHorizontal: 3, borderRadius: 5, alignItems: 'center', justifyContent: 'center', backgroundColor: hov ? 'rgba(0,0,0,0.1)' : 'transparent' }}>
      <Text style={{ fontSize: 12, color: '#3a3a3a' }}>{label || title}</Text>
    </View>
  );
}

function NoteCard({ widget }) {
  const th = useTheme();
  const note = useStore(notesStore, (s) => s.list.find((n) => n.slug === widget.id));
  const [hover, setHover] = useState(false);
  const [mode, setMode] = useState('read');
  const [live, setLive] = useState(null);           // { x, y, w, h } while dragging
  const [emoji, setEmoji] = useState(false);
  const drag = useRef(null);
  const api = useRef(null);
  useEffect(() => {
    const offMove = addListener('pointermove', (e) => {
      const d = drag.current;
      if (!d) return;
      const dx = e.x - d.mx, dy = e.y - d.my;
      if (d.kind === 'move') setLive({ x: Math.max(0, Math.min(screen.width - 60, d.x + dx)), y: Math.max(24, Math.min(screen.height - 40, d.y + dy)), w: d.w, h: d.h });
      else setLive({ x: d.x, y: d.y, w: Math.max(160, d.w + dx), h: Math.max(110, d.h + dy) });
    });
    const offUp = addListener('pointerup', () => {
      const d = drag.current;
      drag.current = null;
      setLive((l) => { if (d && l) updateNote(widget.id, { x: Math.round(l.x), y: Math.round(l.y), w: Math.round(l.w), h: Math.round(l.h) }, { touch: false }); return null; });
    });
    return () => { offMove(); offUp(); };
  }, [widget.id]);
  if (!note) return null;
  const g = live || widget;
  const [bg, head] = PAPER[note.color] || PAPER.yellow;
  const start = (kind) => (e) => { drag.current = { kind, mx: e.x, my: e.y, x: g.x, y: g.y, w: g.w, h: g.h }; };
  const showHead = hover || mode === 'edit' || emoji;
  return (
    <ThemeProvider tokens={paperTokens(th)}>
      <View onMouseEnter={() => setHover(true)} onMouseLeave={() => setHover(false)}
        style={{ position: 'absolute', left: g.x, top: g.y, width: g.w, height: g.h, borderRadius: 4, backgroundColor: bg,
          boxShadow: '0 6px 18px rgba(0,0,0,0.28)', overflow: 'hidden' }}>
        <View onMouseDown={start('move')} style={{ height: 26, flexDirection: 'row', alignItems: 'center', gap: 4, paddingHorizontal: 6,
          backgroundColor: showHead ? head : bg }}>
          {showHead ? (
            <>
              {COLORS.map((c) => <Dot key={c} color={c} size={11} selected={c === note.color} onClick={() => updateNote(note.slug, { color: c }, { touch: false })} />)}
              <View style={{ flex: 1 }} />
              <HeadBtn label={mode === 'edit' ? '👁' : '✎'} onClick={() => setMode(mode === 'edit' ? 'read' : 'edit')} />
              <HeadBtn label="😀" onClick={() => { setMode('edit'); setEmoji(!emoji); }} />
              <HeadBtn label={note.pinned ? '📌' : '📍'} onClick={() => updateNote(note.slug, { pinned: !note.pinned }, { touch: false })} />
              <HeadBtn label="⤢" onClick={() => wm.open('notes', { args: { slug: note.slug } })} />
              <HeadBtn label="✕" onClick={() => { flushAll(); updateNote(note.slug, { desktop: false }, { touch: false }); }} />
            </>
          ) : <Text style={{ flex: 1, fontSize: 11, fontWeight: 600, color: '#5a5a5a', whiteSpace: 'nowrap', overflow: 'hidden' }}>{note.title || ' '}</Text>}
        </View>
        <NoteBody note={note} mode={mode} onMode={setMode} editorApi={api} compact size={13} />
        <View onMouseDown={start('size')} style={{ position: 'absolute', right: 0, bottom: 0, width: 16, height: 16 }}>
          <Text style={{ fontSize: 10, color: 'rgba(0,0,0,0.35)', marginLeft: 4 }}>◢</Text>
        </View>
        {emoji && <EmojiPicker x={Math.max(0, g.w - 304)} y={28} onClose={() => setEmoji(false)} onPick={(ch) => { api.current?.insert(ch); setEmoji(false); }} />}
      </View>
    </ThemeProvider>
  );
}
setupWidgets(NoteCard);

// notes are read when the session starts (and the desktop ones appear by themselves)
queueMicrotask(() => { try { loadNotes(); } catch (e) { console.log(`notes: ${e.message}`); } });

// ---- window ----

const fmtDate = (iso, formatDate) => { try { return formatDate(new Date(iso), 'short'); } catch (e) { return ''; } };

export default function Notes() {
  const th = useTheme();
  const { t: tr, formatDate } = useLocale();
  const { id: wid, args } = useAppWindow();
  const { list, loaded } = useStore(notesStore);
  const [sel, setSel] = useState(null);
  const [q, setQ] = useState('');
  const [mode, setMode] = useState('read');
  const [panel, setPanel] = useState(null);             // 'emoji' | 'attach' | 'image' | 'delete'
  const [toc, setToc] = useState(true);
  const api = useRef(null);
  // ❖Esc reads the selection aloud (sdk/voice.js); while reading a note there is no selection
  const editing = useRef(false);
  useEffect(() => registerSelection(wid, () => (editing.current ? api.current?.selection?.() || '' : '')), [wid]);
  useEffect(() => { if (!loaded) loadNotes(); }, []);
  useEffect(() => { if (args?.slug) { setSel(args.slug); setMode('read'); } }, [args?.nonce]);
  useEffect(() => () => flushAll(), []);
  const needle = q.trim().toLowerCase();
  const shown = list.filter((n) => !needle || `${n.title}\n${n.body}`.toLowerCase().includes(needle));
  const note = list.find((n) => n.slug === sel) || shown[0] || null;
  editing.current = mode === 'edit';
  // block: images and attachments go on their own line
  const insert = (md, block = false) => {
    if (!md || !note) return;
    if (mode === 'edit' && api.current) api.current.insert(block ? `\n${md}\n` : md);
    else updateNote(note.slug, { body: `${note.body.replace(/\s*$/, '')}\n\n${md}\n` });
  };
  const newNote = () => { const s = createNote({ body: `# ${tr('notes.newTitle')}\n\n` }); setSel(s); setMode('edit'); };
  const cmd = {
    new: newNote,
    duplicate: () => note && setSel(duplicateNote(note.slug)),
    desktop: () => note && updateNote(note.slug, { desktop: !note.desktop }, { touch: false }),
    folder: () => note && wm.open('files', { new: true, args: { path: absFolder(note.slug) } }),
    del: () => note && setPanel('delete'),
    toggle: () => setMode((m) => (m === 'edit' ? 'read' : 'edit')),
    toc: () => setToc((v) => !v),
  };
  useKeyboard((e) => {
    if (!(e.meta || e.ctrl)) return;
    const b = (e.base || '').toLowerCase();
    if (b === 'n') cmd.new();
    else if (b === 'e') cmd.toggle();
  });
  useEffect(() => {
    wm.setMenus(wid, [
      { label: tr('notes.menu.file'), items: [
        { label: tr('notes.new'), hint: '❖N', onClick: cmd.new },
        { label: tr('notes.duplicate'), onClick: cmd.duplicate, disabled: !note },
        { label: tr('notes.showDesktop'), checked: !!note?.desktop, onClick: cmd.desktop, disabled: !note },
        { label: tr('notes.openFolder'), onClick: cmd.folder, disabled: !note }, '-',
        { label: tr('notes.print'), onClick: () => note && print.open({ title: note.title || tr('notes.untitled'), text: note.body }), disabled: !note }, '-',
        { label: tr('notes.delete'), onClick: cmd.del, disabled: !note },
      ] },
      { label: tr('notes.menu.format'), items: [
        { label: tr('notes.fmt.bold'), hint: '❖B', disabled: true }, { label: tr('notes.fmt.italic'), hint: '❖I', disabled: true },
        { label: tr('notes.fmt.link'), hint: '❖K', disabled: true }, '-',
        { label: tr('notes.fmt.h1'), hint: '❖1', disabled: true }, { label: tr('notes.fmt.h2'), hint: '❖2', disabled: true },
        { label: tr('notes.fmt.bullets'), hint: '❖⇧8', disabled: true }, { label: tr('notes.fmt.numbers'), hint: '❖⇧7', disabled: true },
        { label: tr('notes.fmt.task'), hint: '❖⇧9', disabled: true }, { label: tr('notes.fmt.code'), hint: '❖⇧C', disabled: true },
      ] },
      { label: tr('notes.menu.view'), items: [
        { label: mode === 'edit' ? tr('notes.read') : tr('notes.edit'), hint: '❖E', onClick: cmd.toggle },
        { label: tr('notes.toc'), checked: toc, onClick: cmd.toc },
        // the selection, or the whole note when nothing is selected (as a Mac's Start Speaking)
        ...(note ? ['-', ...speechItems(() => (mode === 'edit' && api.current?.selection?.()) || note.body || '')].slice(0, -1) : []),
      ] },
    ]);
  });
  // window width: a narrower sidebar and a compact toolbar in small windows
  const root = useRef(null);
  const [winW, setWinW] = useState(860);
  useEffect(() => {
    const id = setInterval(() => { if (root.current) { const w = lord.getLayout(root.current.id)[2]; if (Math.abs(w - winW) > 2) setWinW(w); } }, 300);
    return () => clearInterval(id);
  }, [winW]);
  const sideW = winW < 720 ? 200 : 240;
  const compact = winW - sideW < 470;
  const colorItems = note ? COLORS.map((c) => ({ label: tr(`notes.color.${c}`), checked: c === note.color, onClick: () => updateNote(note.slug, { color: c }, { touch: false }) })) : [];
  return (
    <View ref={root} style={{ flex: 1, flexDirection: 'row' }}>
      <View style={{ width: sideW, backgroundColor: th.sidebarBg, paddingTop: 12, paddingHorizontal: 10, gap: 10 }}>
        <View style={{ position: 'absolute', right: 0, top: 0, bottom: 0, width: 1, backgroundColor: th.separator }} />
        <View style={{ flexDirection: 'row', alignItems: 'center', gap: 6 }}>
          <SearchField value={q} onChange={setQ} placeholder={tr('notes.search')} style={{ flex: 1 }} />
          <ToolbarButton icon="✎" title={tr('notes.new')} onClick={newNote} />
        </View>
        <ScrollView style={{ flex: 1 }}>
          {shown.map((n) => {
            const on = note && n.slug === note.slug;
            const pv = previewOf(n.body);
            return (
              <View key={n.slug} onClick={() => { setSel(n.slug); setMode('read'); }} style={{ flexDirection: 'row', gap: 10, paddingVertical: 9, paddingHorizontal: 10,
                borderRadius: 8, marginBottom: 2, backgroundColor: on ? th.accent : 'transparent' }}>
                <View style={{ width: 8, height: 8, borderRadius: 4, marginTop: 5, backgroundColor: PAPER[n.color][1], border: '1px solid rgba(0,0,0,0.12)' }} />
                <View style={{ flex: 1, gap: 2 }}>
                  <Text style={{ fontSize: 13, fontWeight: 600, color: on ? '#fff' : th.text, whiteSpace: 'nowrap', overflow: 'hidden' }}>{n.title || tr('notes.untitled')}</Text>
                  <View style={{ flexDirection: 'row', gap: 6, overflow: 'hidden' }}>
                    <Text style={{ fontSize: 11, color: on ? 'rgba(255,255,255,0.85)' : th.text, whiteSpace: 'nowrap' }}>{fmtDate(n.updated, formatDate)}</Text>
                    <Text style={{ flex: 1, fontSize: 11, color: on ? 'rgba(255,255,255,0.75)' : th.textDim, whiteSpace: 'nowrap', overflow: 'hidden',
                      direction: n.desktop ? 'inherit' : textDirection(pv) }}>{n.desktop ? tr('notes.onDesktop') : pv}</Text>
                  </View>
                </View>
              </View>
            );
          })}
          {!shown.length && <Text style={{ fontSize: 12, color: th.textDim, margin: 10 }}>{loaded ? tr('notes.none') : '…'}</Text>}
        </ScrollView>
      </View>
      {note ? (
        <View style={{ flex: 1 }}>
          <Toolbar>
            <Segmented appearance="neutral" value={mode} onChange={setMode} options={[['read', tr('notes.read')], ['edit', tr('notes.edit')]]} />
            {!compact && <View style={{ flexDirection: 'row', gap: 6, alignItems: 'center', marginLeft: 8 }}>
              {COLORS.map((c) => <Dot key={c} color={c} size={14} selected={c === note.color} onClick={() => updateNote(note.slug, { color: c }, { touch: false })} />)}
            </View>}
            <ToolbarSpacer />
            <MenuButton icon="⊕" title={tr('notes.insert')} items={[
              { label: tr('notes.insertEmoji'), onClick: () => setPanel('emoji') },
              { label: tr('notes.insertImage'), onClick: () => setPanel('image') },
              { label: tr('notes.insertFile'), onClick: () => setPanel('attach') },
            ]} />
            <MenuButton title={tr('notes.more')} items={[
              { label: tr('notes.duplicate'), onClick: cmd.duplicate },
              { label: tr('notes.showDesktop'), checked: !!note.desktop, onClick: cmd.desktop },
              { label: tr('notes.openFolder'), onClick: cmd.folder },
              ...(compact ? ['-', ...colorItems] : []),
              '-',
              { label: tr('notes.delete'), destructive: true, onClick: cmd.del },
            ]} />
          </Toolbar>
          <View style={{ flex: 1, backgroundColor: PAPER[note.color][0] }}>
            <ThemeProvider tokens={paperTokens(th)}>
              <NoteBody key={note.slug} note={note} mode={mode} onMode={setMode} editorApi={api} showToc={toc && winW - sideW >= 520} onCommand={(b) => (b === 'n' ? cmd.new() : b === 'e' ? cmd.toggle() : null)} />
            </ThemeProvider>
          </View>
          {panel === 'emoji' && <EmojiPicker x={Math.max(8, winW - sideW - 316)} y={48} onClose={() => setPanel(null)} onPick={(ch) => { insert(ch); setPanel(null); }} />}
          {(panel === 'attach' || panel === 'image') && (
            <FilePicker images={panel === 'image'} onClose={() => setPanel(null)} onPick={(p) => { insert(attachFile(note.slug, p), true); setPanel(null); }} />
          )}
          {panel === 'delete' && (
            <View style={{ position: 'absolute', left: 0, right: 0, top: 0, bottom: 0, alignItems: 'center', justifyContent: 'center', backgroundColor: 'rgba(0,0,0,0.25)' }}>
              <View style={{ width: 340, padding: 20, gap: 8, borderRadius: 12, backgroundColor: th.windowBg, border: `1px solid ${th.separator}`, boxShadow: '0 16px 40px rgba(0,0,0,0.4)' }}>
                <Text style={{ fontSize: 15, fontWeight: 600, color: th.text }}>{tr('notes.deleteTitle', { title: note.title || tr('notes.untitled') })}</Text>
                <Text style={{ fontSize: 13, color: th.textDim, lineHeight: 1.4 }}>{tr('notes.deleteHint')}</Text>
                <View style={{ flexDirection: 'row', gap: 8, justifyContent: 'flex-end', marginTop: 12 }}>
                  <Button label={tr('notes.cancel')} onClick={() => setPanel(null)} />
                  <Button primary label={tr('notes.delete')} onClick={() => { deleteNote(note.slug); setSel(null); setPanel(null); }} />
                </View>
              </View>
            </View>
          )}
        </View>
      ) : (
        <EmptyState icon={<Icon size={56} />} title={tr('notes.none')} hint={tr('notes.noneHint')}
          action={<Button primary label={tr('notes.new')} onClick={newNote} />} />
      )}
    </View>
  );
}

// Preview line of a note: the first line with text after the title, without markdown marks.
function previewOf(body) {
  const lines = body.split('\n').map((l) => l.trim()).filter((l) => l && !/^(`{3,}|---|#)/.test(l));
  return (lines[0] || '').replace(/^(>\s*|[-*+]\s+(\[[ xX]\]\s+)?|\d+[.)]\s+)/, '').replace(/[*_`~]|!\[[^\]]*\]\([^)]*\)/g, '').slice(0, 80);
}
