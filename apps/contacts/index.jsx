// Contacts — address books kept by lord-maild (local, or synced with CardDAV): see docs/MAIL.md.
// Three columns: books and groups, the contact list (search, letter sections, virtualized) and
// the card, with an edit mode. Groups are contacts with kind 'group' and a list of member uids.
import {
  View, Text, useTheme, useLocale, useKeyboard, useWindowState, usePim, addressBooks, contacts, contactName,
  Button, TextInput, SearchField, Hairline, EmptyState, ToolbarButton, FilePicker, addListener, direction, formatNumber, t,
  isolatedPim, pickFileFd, closeFileFd, openDavSetup,
} from '@lordos/sdk';
import { useEffect, useMemo, useRef, useState } from 'react';
import { RED, Sheet, Popover, MenuList, Layers, useLoad } from './ui.jsx';
import { Card, Editor, blankContact } from './card.jsx';

// ---- icon: an address-book card with a person silhouette ----

export function Icon({ size: s }) {
  return (
    <View style={{ width: s, height: s, borderRadius: s * 0.225, background: 'linear-gradient(180deg, #b49a7e, #8c7259)',
      boxShadow: `0 ${s * 0.04}px ${s * 0.12}px rgba(0,0,0,0.35)`, overflow: 'hidden' }}>
      {/* binding strip */}
      <View style={{ position: 'absolute', left: 0, top: 0, bottom: 0, width: s * 0.16, backgroundColor: '#6f5843' }} />
      {/* the card */}
      <View style={{ position: 'absolute', left: s * 0.24, top: s * 0.14, width: s * 0.62, height: s * 0.72, borderRadius: s * 0.06,
        backgroundColor: '#f4f1ec', overflow: 'hidden', alignItems: 'center' }}>
        <View style={{ marginTop: s * 0.12, width: s * 0.22, height: s * 0.22, borderRadius: s * 0.11, backgroundColor: '#9b9b9f' }} />
        <View style={{ marginTop: s * 0.04, width: s * 0.42, height: s * 0.42, borderRadius: s * 0.21, backgroundColor: '#9b9b9f' }} />
      </View>
      {[0.3, 0.5, 0.7].map((y) => (
        <View key={y} style={{ position: 'absolute', left: s * 0.86, top: s * y, width: s * 0.06, height: s * 0.1, borderRadius: s * 0.02, backgroundColor: '#e0a33a' }} />
      ))}
    </View>
  );
}

const ROW = 30, HEAD = 24;

const sortKey = (c) => {
  const n = c.n || {};
  const k = [n.family, n.given].filter(Boolean).join(' ');
  return (k || contactName(c) || '').trim();
};
function letterOf(c) {
  const ch = [...sortKey(c)][0] || '';
  return /\p{L}/u.test(ch) ? ch.toLocaleUpperCase() : '#';
}
const keyOf = (c) => `${c.book}/${c.uid}`;

function matchesQuery(c, q) {
  if (!q) return true;
  const hay = [contactName(c), c.org, c.nick, ...(c.emails || []).map((x) => x.value), ...(c.phones || []).map((x) => x.value)].join('\n').toLowerCase();
  return q.toLowerCase().split(/\s+/).filter(Boolean).every((w) => hay.includes(w));
}

export default function ContactsApp() {
  const th = useTheme();
  useLocale();
  const rev = usePim();
  const booksQ = useLoad(() => addressBooks.list(), [rev]);
  const allQ = useLoad(() => contacts.list(), [rev]);
  const books = booksQ.data || [];
  const all = allQ.data || [];
  const [src, setSrc] = useWindowState('src', 'all');      // 'all' | 'book:<id>' | 'group:<book>/<uid>'
  const [cur, setCur] = useWindowState('cur', null);       // `${book}/${uid}`
  const [query, setQuery] = useState('');
  const [draft, setDraft] = useState(null);                // the contact being edited (edit mode)
  const [sheet, setSheet] = useState(null);                // { group } | { newGroup } | { pick: 'vcf' } | { confirm: contact } | { notice }

  const people = useMemo(() => all.filter((c) => c.kind !== 'group'), [all]);
  const groups = useMemo(() => all.filter((c) => c.kind === 'group').sort((a, b) => contactName(a).localeCompare(contactName(b))), [all]);
  const group = src.startsWith('group:') ? groups.find((g) => keyOf(g) === src.slice(6)) : null;
  const bookId = src.startsWith('book:') ? src.slice(5) : group ? group.book : null;

  const list = useMemo(() => {
    let l = people;
    if (group) { const m = new Set(group.members || []); l = l.filter((c) => c.book === group.book && m.has(c.uid)); }
    else if (bookId) l = l.filter((c) => c.book === bookId);
    l = l.filter((c) => matchesQuery(c, query.trim()));
    return [...l].sort((a, b) => sortKey(a).localeCompare(sortKey(b), undefined, { sensitivity: 'base' }));
  }, [people, group, bookId, query]);

  const selected = all.find((c) => keyOf(c) === cur) || null;
  const justSaved = useRef(null);         // a new contact: wait until the list has it
  useEffect(() => {
    if (draft) return;
    const there = list.some((c) => keyOf(c) === cur);
    if (justSaved.current && justSaved.current === cur) { if (there) justSaved.current = null; return; }
    if (!there) setCur(list.length ? keyOf(list[0]) : null);
  }, [list, draft]);

  const targetBook = async () => {
    if (bookId && books.some((b) => b.id === bookId)) return bookId;
    if (books.length) return (books.find((b) => b.kind === 'local') || books[0]).id;
    const r = await addressBooks.create({ name: t('contacts.defaultBook') });
    return r.uid;
  };
  const startNew = () => setDraft({ ...blankContact(), _new: true });
  const saveDraft = async (c, photo) => {
    const isNew = !!c._new;
    const book = isNew ? await targetBook() : c.book;
    const { _new, ...clean } = c;
    const uid = await contacts.save(book, clean, { photo });
    if (isNew && group && uid) await contacts.save(group.book, { ...group, members: [...(group.members || []), uid] }, {});
    if (uid) { justSaved.current = `${book}/${uid}`; setCur(justSaved.current); }
    setDraft(null);
  };

  useKeyboard((e) => {
    if (sheet || draft) { if (e.key === 'Escape') { setSheet(null); } return; }
    const k = String(e.base || e.key || '').toLowerCase();
    if ((e.meta || e.ctrl) && k === 'n') startNew();
    else if (e.key === 'ArrowDown' || e.key === 'ArrowUp') {
      const i = list.findIndex((c) => keyOf(c) === cur);
      const j = Math.max(0, Math.min(list.length - 1, i + (e.key === 'ArrowDown' ? 1 : -1)));
      if (list[j]) setCur(keyOf(list[j]));
    }
  });

  const unavailable = (booksQ.error && !booksQ.data) || (allQ.error && !allQ.data);
  const importVcf = async (path) => {
    setSheet(null);
    try {
      const r = await contacts.importFile(path, await targetBook());
      setSheet({ notice: t('contacts.imported', { count: r && r.added != null ? r.added : 0, n: formatNumber(r && r.added != null ? r.added : 0, 0) }) });
    } catch (e) { setSheet({ notice: t('contacts.importFailed') }); }
    closeFileFd(path);
  };
  // in its own process Contacts gets the .vcf from the system's file dialog (only that file)
  const startImport = () => {
    if (!isolatedPim()) { setSheet({ pick: 'vcf' }); return; }
    let f = null;
    try { f = pickFileFd(); } catch (e) { setSheet({ notice: t('contacts.importFailed') }); }
    if (f) importVcf(f);
  };

  return (
    <View style={{ flex: 1, flexDirection: 'row', backgroundColor: th.windowBg }}>
      <Sources books={books} groups={groups} src={src} counts={people} onSrc={(s) => { setSrc(s); setDraft(null); }}
        onNewGroup={() => setSheet({ newGroup: true })} onImport={startImport}
        onRenameGroup={(g) => setSheet({ group: g })} onDeleteGroup={(g) => { contacts.remove(g.book, g.uid).catch(() => {}); if (src === `group:${keyOf(g)}`) setSrc('all'); }} />
      <View style={{ width: 270 }}>
        <Hairline edge="end" />
        <View style={{ paddingHorizontal: 12, paddingTop: 12, paddingBottom: 8, flexDirection: 'row', gap: 6, alignItems: 'center' }}>
          <SearchField value={query} onChange={setQuery} placeholder={t('contacts.search')} style={{ flex: 1 }} />
        </View>
        {unavailable ? null : list.length ? (
          <ContactList list={list} cur={cur} onPick={(c) => { setDraft(null); setCur(keyOf(c)); }} />
        ) : (
          <EmptyState title={query ? t('contacts.noResults') : t('contacts.noContacts')} />
        )}
        <View style={{ height: 40, flexDirection: 'row', alignItems: 'center', paddingHorizontal: 10 }}>
          <Hairline edge="top" />
          <ToolbarButton icon="+" iconSize={18} small title={t('contacts.newContact')} onClick={startNew} disabled={unavailable} />
          <Text style={{ flex: 1, fontSize: 11, color: th.textDim, textAlign: 'center', whiteSpace: 'nowrap' }}>{t('contacts.count', { count: list.length, n: formatNumber(list.length, 0) })}</Text>
          <View style={{ width: 22 }} />
        </View>
      </View>
      <View style={{ flex: 1 }}>
        {unavailable ? (
          <EmptyState mascot="confused" title={t('contacts.unavailable')} hint={t('contacts.unavailableHint')} />
        ) : draft ? (
          <Editor key={draft._new ? 'new' : keyOf(draft)} contact={draft} groups={groups} onCancel={() => setDraft(null)} onSave={saveDraft}
            onDelete={draft._new ? null : () => setSheet({ confirm: draft })} />
        ) : selected ? (
          <Card contact={selected} groups={groups.filter((g) => g.book === selected.book)} onEdit={() => setDraft(JSON.parse(JSON.stringify(selected)))} />
        ) : (
          <EmptyState icon={<PersonGlyph size={44} color={th.textDim} />} title={t('contacts.noSelection')} />
        )}
      </View>
      {sheet && (sheet.newGroup || sheet.group) ? (
        <GroupSheet group={sheet.group} onClose={() => setSheet(null)} onSave={async (name) => {
          if (sheet.group) await contacts.save(sheet.group.book, { ...sheet.group, fn: name }, {});
          else {
            const book = await targetBook();
            const uid = await contacts.save(book, { kind: 'group', fn: name, n: {}, members: [] }, {});
            if (uid) setSrc(`group:${book}/${uid}`);
          }
          setSheet(null);
        }} />
      ) : null}
      {sheet && sheet.pick ? <FilePicker title={t('contacts.importTitle')} filter={/\.vcf$/i} onClose={() => setSheet(null)} onPick={(p) => typeof p === 'string' && importVcf(p)} /> : null}
      {sheet && sheet.confirm ? (
        <Sheet width={360} title={t('contacts.deleteTitle', { name: contactName(sheet.confirm) || t('contacts.noName') })} footer={[
          <View key="sp" style={{ flex: 1 }} />,
          <Button key="c" label={t('contacts.cancel')} onClick={() => setSheet(null)} />,
          <Button key="d" primary destructive label={t('contacts.delete')} onClick={() => {
            const c = sheet.confirm;
            setSheet(null); setDraft(null);
            contacts.remove(c.book, c.uid).catch(() => {});
          }} />]}>
          <Text style={{ fontSize: 13, color: th.textDim }}>{t('contacts.deleteHint')}</Text>
        </Sheet>
      ) : null}
      {sheet && sheet.notice ? (
        <Sheet width={340} footer={[<View key="sp" style={{ flex: 1 }} />, <Button key="o" primary label={t('contacts.ok')} onClick={() => setSheet(null)} />]}>
          <Text style={{ fontSize: 13, color: th.text }}>{sheet.notice}</Text>
        </Sheet>
      ) : null}
      <Layers />
    </View>
  );
}

// ---- first column: All Contacts, books and their groups ----

function Sources({ books, groups, src, counts, onSrc, onNewGroup, onImport, onRenameGroup, onDeleteGroup }) {
  const th = useTheme();
  return (
    <View style={{ width: 212, backgroundColor: th.sidebarBg }}>
      <Hairline edge="end" />
      <View style={{ flex: 1, paddingHorizontal: 10, paddingTop: 12, gap: 2 }}>
        <SrcRow icon={<PersonGlyph size={14} />} label={t('contacts.allContacts')} count={counts.length} selected={src === 'all'} onClick={() => onSrc('all')} />
        {books.map((b) => (
          <View key={b.id} style={{ gap: 2, marginTop: 16 }}>
            <Text style={{ fontSize: 11, fontWeight: 600, color: th.textDim, marginHorizontal: 8, marginBottom: 2, whiteSpace: 'nowrap', overflow: 'hidden' }}>{b.acct ? b.acctName || b.acct : t('contacts.onThisComputer')}</Text>
            <SrcRow icon="▤" label={b.name} count={counts.filter((c) => c.book === b.id).length} selected={src === `book:${b.id}`} onClick={() => onSrc(`book:${b.id}`)} />
            {groups.filter((g) => g.book === b.id).map((g) => (
              <SrcRow key={g.uid} indent icon="▢" label={contactName(g) || t('contacts.untitledGroup')} count={(g.members || []).length}
                selected={src === `group:${keyOf(g)}`} onClick={() => onSrc(`group:${keyOf(g)}`)}
                menu={[{ label: t('contacts.renameGroup'), onClick: () => onRenameGroup(g) }, '-', { label: t('contacts.deleteGroup'), destructive: true, onClick: () => onDeleteGroup(g) }]} />
            ))}
          </View>
        ))}
        {/* synced address books (CardDAV, a provider): Mail's Add Account sheet, in Calendars & Contacts mode */}
        <Button small plain icon="⊕" label={t('contacts.addAccount')} onClick={() => openDavSetup()} style={{ alignSelf: 'flex-start', marginTop: 14 }} />
      </View>
      <View style={{ height: 40, flexDirection: 'row', alignItems: 'center', paddingHorizontal: 6, gap: 2 }}>
        <Hairline edge="top" />
        <Button small plain icon="⊕" label={t('contacts.newGroup')} onClick={onNewGroup} />
        <View style={{ flex: 1 }} />
        <Button small plain label={t('contacts.import')} onClick={onImport} />
      </View>
    </View>
  );
}

function SrcRow({ icon, label, count, selected, onClick, indent, menu }) {
  const th = useTheme();
  const ref = useRef(null);
  const [open, setOpen] = useState(false);
  const [hover, setHover] = useState(false);
  return (
    <View ref={ref} onClick={onClick} onContextMenu={menu ? () => setOpen(true) : undefined} onMouseEnter={() => setHover(true)} onMouseLeave={() => setHover(false)}
      style={{ height: 28, paddingHorizontal: indent ? 24 : 8, borderRadius: 6, flexDirection: 'row', alignItems: 'center', gap: 8,
        backgroundColor: selected ? th.accent : hover ? th.hover : 'transparent' }}>
      <View style={{ width: 16, alignItems: 'center' }}>
        {typeof icon === 'string' ? <Text style={{ fontSize: 13, color: selected ? '#fff' : th.accent }}>{icon}</Text>
          : <PersonGlyph size={14} color={selected ? '#fff' : th.accent} />}
      </View>
      <Text style={{ flex: 1, fontSize: 13, color: selected ? '#fff' : th.text, whiteSpace: 'nowrap', overflow: 'hidden' }}>{label}</Text>
      <Text style={{ fontSize: 11, color: selected ? 'rgba(255,255,255,0.8)' : th.textDim }}>{String(count)}</Text>
      {open && menu ? (
        <Popover anchor={ref} width={190} onClose={() => setOpen(false)}><MenuList items={menu} onDone={() => setOpen(false)} /></Popover>
      ) : null}
    </View>
  );
}

// ---- second column: the list, with letter headers, only the visible rows are drawn ----

function ContactList({ list, cur, onPick }) {
  const th = useTheme();
  const box = useRef(null);
  const [h, setH] = useState(400);
  const [off, setOff] = useState(0);
  const rows = useMemo(() => {
    const out = [];
    let last = null, y = 0;
    for (const c of list) {
      const l = letterOf(c);
      if (l !== last) { out.push({ head: l, top: y }); y += HEAD; last = l; }
      out.push({ c, top: y });
      y += ROW;
    }
    return { out, total: y };
  }, [list]);
  useEffect(() => {
    const id = setTimeout(() => { if (box.current) { const v = lord.getLayout(box.current.id)[3]; if (v && Math.abs(v - h) > 1) setH(v); } }, 30);
    return () => clearTimeout(id);
  });
  const max = Math.max(0, rows.total - h);
  const y = Math.min(off, max);
  // keep the selected row in view
  useEffect(() => {
    const r = rows.out.find((x) => x.c && keyOf(x.c) === cur);
    if (!r) return;
    if (r.top < y) setOff(Math.max(0, r.top - HEAD));
    else if (r.top + ROW > y + h) setOff(r.top + ROW - h);
  }, [cur]);
  const drag = useRef(null);
  useEffect(() => {
    const a = addListener('pointermove', (e) => { if (drag.current) setOff(Math.max(0, Math.min(max, drag.current.y0 + ((e.y - drag.current.py) * rows.total) / Math.max(1, h)))); });
    const b = addListener('pointerup', () => { drag.current = null; });
    return () => { a(); b(); };
  });
  // binary search for the first visible row
  let lo = 0, hi = rows.out.length - 1;
  while (lo < hi) { const mid = (lo + hi) >> 1; if (rows.out[mid].top + ROW < y) lo = mid + 1; else hi = mid; }
  const shown = [];
  for (let i = Math.max(0, lo - 1); i < rows.out.length && rows.out[i].top < y + h + ROW; i++) shown.push(rows.out[i]);
  const bar = rows.total > h ? Math.max(28, (h * h) / rows.total) : 0;
  const rtl = direction() === 'rtl';
  return (
    <View ref={box} onWheel={(e) => setOff((o) => Math.max(0, Math.min(max, Math.min(o, max) + e.deltaY)))} style={{ flex: 1, overflow: 'hidden' }}>
      {shown.map((r) => (r.head != null ? (
        <View key={`h${r.head}`} style={{ position: 'absolute', left: 0, right: 0, top: r.top - y, height: HEAD, justifyContent: 'flex-end', paddingBottom: 3, paddingHorizontal: 16 }}>
          <Text style={{ fontSize: 11, fontWeight: 600, color: th.textDim }}>{r.head}</Text>
          <Hairline edge="bottom" inset={14} />
        </View>
      ) : (
        <ContactRow key={keyOf(r.c)} c={r.c} top={r.top - y} selected={keyOf(r.c) === cur} onPick={onPick} />
      )))}
      {bar > 0 ? (
        <View onMouseDown={(e) => { drag.current = { py: e.y, y0: y }; }}
          style={{ position: 'absolute', [rtl ? 'left' : 'right']: 2, width: 6, borderRadius: 3, height: bar, top: (h - bar) * (y / (max || 1)),
            backgroundColor: th.mode === 'dark' ? 'rgba(255,255,255,0.3)' : 'rgba(0,0,0,0.28)' }} />
      ) : null}
    </View>
  );
}

function ContactRow({ c, top, selected, onPick }) {
  const th = useTheme();
  const n = c.n || {};
  const given = [n.prefix, n.given, n.middle].filter(Boolean).join(' ');
  const family = [n.family, n.suffix].filter(Boolean).join(' ');
  const fg = selected ? '#fff' : th.text;
  return (
    <View onClick={() => onPick(c)} style={{ position: 'absolute', left: 8, right: 8, top: top + 1, height: ROW - 2, borderRadius: 6, paddingHorizontal: 8,
      flexDirection: 'row', alignItems: 'center', gap: 4, backgroundColor: selected ? th.accent : 'transparent' }}>
      {given || family ? (
        <>
          {given ? <Text style={{ fontSize: 13, color: fg, whiteSpace: 'nowrap' }}>{given}</Text> : null}
          {family ? <Text style={{ fontSize: 13, fontWeight: 600, color: fg, whiteSpace: 'nowrap', overflow: 'hidden', flexShrink: 1 }}>{family}</Text> : null}
        </>
      ) : (
        <Text style={{ fontSize: 13, fontWeight: 600, color: fg, whiteSpace: 'nowrap', overflow: 'hidden', flexShrink: 1 }}>{contactName(c) || t('contacts.noName')}</Text>
      )}
    </View>
  );
}

// ---- new group / rename group ----

function GroupSheet({ group, onClose, onSave }) {
  const [name, setName] = useState(group ? contactName(group) : '');
  const [error, setError] = useState('');
  const save = async () => { if (!name.trim()) return; try { await onSave(name.trim()); } catch (e) { setError(t('contacts.saveFailed')); } };
  return (
    <Sheet width={360} title={group ? t('contacts.renameGroup') : t('contacts.newGroup')} footer={[
      <Text key="e" style={{ flex: 1, fontSize: 12, color: RED }}>{error}</Text>,
      <Button key="c" label={t('contacts.cancel')} onClick={onClose} />,
      <Button key="s" primary disabled={!name.trim()} label={t('contacts.save')} onClick={save} />]}>
      <TextInput value={name} onChange={setName} placeholder={t('contacts.groupName')} autoFocus onSubmit={save} onCancel={onClose} />
    </Sheet>
  );
}

// A person silhouette drawn with two shapes (the 👤 emoji is in color and does not follow the theme).
export function PersonGlyph({ size = 14, color }) {
  const th = useTheme();
  const c = color || th.accent;
  return (
    <View style={{ width: size, height: size, alignItems: 'center' }}>
      <View style={{ width: size * 0.44, height: size * 0.44, borderRadius: size * 0.22, backgroundColor: c }} />
      <View style={{ marginTop: size * 0.06, width: size * 0.86, height: size * 0.42, borderTopLeftRadius: size * 0.43, borderTopRightRadius: size * 0.43,
        borderBottomLeftRadius: size * 0.08, borderBottomRightRadius: size * 0.08, backgroundColor: c }} />
    </View>
  );
}
