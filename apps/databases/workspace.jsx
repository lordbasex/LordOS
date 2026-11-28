// A connection's tab (the approved mocks Structure, Content, Query, Table Info): the toolbar with the database pop-up,
// the panes and History; the tables, views and routines of the database with a filter and the selected table's
// summary; the pane. A production favorite opens read-only until it is unlocked; a closed connection says why and offers
// to reconnect.
import { View, Text, ScrollView, SearchField, MenuButton, ConfirmDialog, wm, windows, clipboard, useStore, t as tr,
  useWindowWidth, useSidebar, SidebarToggle, SidebarOverlay } from '@lordos/sdk';
import { useEffect, useState } from 'react';
import { usePal, Icons, ToolItem, Sep, IconBtn, Note, Btn, Later, Foot } from './ui.jsx';
import { store, run1, useDatabase, patchSession } from './conn.js';
import { useView, patchView } from './viewstate.js';
import { parseTables, tablesSql, routinesSql, statusSql, parseStatus, humanBytes } from './model.js';
import { errText, closedText, n } from './texts.js';
import { Structure } from './structure.jsx';
import { Content } from './content.jsx';
import { Query } from './query.jsx';
import { TableInfo } from './info.jsx';

// a small window (a Workspaces tile): below COMPACT_AT the toolbar shows icons without labels, below SIDE_AT the
// tables hide behind a toolbar button, below TIGHT_AT Relations, Triggers, Users, Processes and Console go in a ⋯ menu
const COMPACT_AT = 900, SIDE_AT = 760, TIGHT_AT = 640;

// opens (or brings forward) the connection's Console window
export function openConsole(s) {
  const instance = `console-${s.key}`;
  const id = `databases#${instance}`;
  const fresh = !windows.get().some((w) => w.id === id);
  wm.open('databases', { instance, args: { console: s.key } });
  if (fresh) { wm.resize(id, 900, 560); wm.center(id); }
}

export function Workspace({ s, tabs, onReconnect }) {
  const p = usePal();
  useStore(store);
  const v = useView(s.key);
  const [dbs, setDbs] = useState([]);
  const [list, setList] = useState({ tables: [], views: [], routines: [] });
  const [filter, setFilter] = useState('');
  const [status, setStatus] = useState(null);       // the selected table's SHOW TABLE STATUS
  const [error, setError] = useState('');
  const [unlock, setUnlock] = useState(false);
  const db = s.db;
  const winW = useWindowWidth();
  const compact = winW > 0 && winW < COMPACT_AT, tight = winW > 0 && winW < TIGHT_AT;
  const side = useSidebar(SIDE_AT, 'tables');

  const loadDbs = () => run1(s.key, 'SHOW DATABASES').then((r) => setDbs(r.rows.map((x) => x[0]))).catch((e) => setError(errText(e)));
  const loadTables = async () => {
    if (!db) { setList({ tables: [], views: [], routines: [] }); return; }
    try {
      const t = parseTables(await run1(s.key, tablesSql(db)));
      let routines = [];
      try { routines = (await run1(s.key, routinesSql(db))).rows.map((r) => ({ name: r[0], type: r[1] })); } catch (e) { routines = []; }
      setList({ ...t, routines });
      setError('');
    } catch (e) { setError(errText(e)); }
  };
  useEffect(() => { loadDbs(); }, [s.key]);
  useEffect(() => { loadTables(); }, [s.key, db]);
  useEffect(() => {
    setStatus(null);
    if (!db || !v.table) return;
    run1(s.key, statusSql(db, v.table)).then((r) => setStatus(parseStatus(r))).catch(() => {});
  }, [s.key, db, v.table]);

  // where the tab goes: { db, table, kind, pane }, kept in History
  const go = (to, { record = true } = {}) => {
    const next = { db, table: v.table, kind: v.kind, pane: v.pane, ...to };
    const patch = { table: next.table, kind: next.kind, pane: next.pane };
    if (record) {
      const nav = [...v.nav.slice(0, v.navAt + 1), next].slice(-50);
      patch.nav = nav; patch.navAt = nav.length - 1;
    }
    patchView(s.key, patch);
    if (next.db !== db) useDatabase(s.key, next.db).catch((e) => setError(errText(e)));
  };
  const pickDb = (name) => go({ db: name, table: '', kind: 'table' });
  const back = () => { if (v.navAt > 0) { const to = v.nav[v.navAt - 1]; patchView(s.key, { navAt: v.navAt - 1 }); go(to, { record: false }); } };
  const fwd = () => { if (v.navAt < v.nav.length - 1) { const to = v.nav[v.navAt + 1]; patchView(s.key, { navAt: v.navAt + 1 }); go(to, { record: false }); } };
  const pane = (id) => go({ pane: id });

  const q = filter.trim().toLowerCase();
  const match = (name) => !q || name.toLowerCase().includes(q);
  const tableItems = list.tables.filter(match), viewItems = list.views.filter(match), routineItems = list.routines.filter((r) => match(r.name));
  const tableAsks = !!v.table;
  const PANES = [
    ['structure', tr('databases.pane.structure'), Icons.table, true],
    ['content', tr('databases.pane.content'), Icons.grid, true],
    ['relations', tr('databases.pane.relations'), Icons.link, false],
    ['triggers', tr('databases.pane.triggers'), Icons.bolt, false],
    ['info', tr('databases.pane.info'), Icons.info, true],
    ['query', tr('databases.pane.query'), Icons.doc, true],
  ];

  let body = null;
  if (v.pane === 'query') body = <Query s={s} db={db} tables={[...list.tables, ...list.views]} />;
  else if (v.pane === 'users' || v.pane === 'processes' || v.pane === 'relations' || v.pane === 'triggers') body = <Later label={tr(`databases.pane.${v.pane}`)} />;
  else if (!db) body = <Empty text={tr('databases.pickDb')} />;
  else if (!v.table) body = <Empty text={tr('databases.pickTable')} />;
  else if (v.pane === 'structure') body = <Structure key={`${db}.${v.table}`} s={s} db={db} table={v.table} view={v.kind === 'view'} onChanged={loadTables} />;
  else if (v.pane === 'content') body = <Content key={`${db}.${v.table}`} s={s} db={db} table={v.table} view={v.kind === 'view'} status={status} />;
  else if (v.pane === 'info') body = <TableInfo key={`${db}.${v.table}`} s={s} db={db} table={v.table} view={v.kind === 'view'} status={status} />;

  const inMenu = (id) => tight && (id === 'relations' || id === 'triggers');
  const lockedNote = s.locked ? (
    <View style={{ flexDirection: 'row', alignItems: 'center', gap: 6, paddingHorizontal: 10, height: 26, borderRadius: 13, backgroundColor: p.warnBg }}>
      {Icons.lock(p.warn, 11)}
      <Text style={{ fontSize: 12, color: p.text }}>{tr('databases.lock.readOnly')}</Text>
      {!s.fav.readOnly ? <Btn small link label={tr('databases.lock.unlock')} onClick={() => setUnlock(true)} /> : null}
    </View>
  ) : null;

  // the tables, views and routines (a small window: over the content, picking one closes it)
  const pick = (to) => { go(to); if (side.narrow) side.close(); };
  const tablesSide = (
        <View style={{ width: 230, flex: side.narrow ? 1 : undefined, backgroundColor: p.side }}>
          <View style={{ position: 'absolute', right: 0, top: 0, bottom: 0, width: 1, backgroundColor: p.line }} />
          <View style={{ paddingHorizontal: 10, paddingTop: 10, paddingBottom: 4 }}><SearchField value={filter} onChange={setFilter} placeholder={tr('databases.filter')} /></View>
          <ScrollView style={{ flex: 1 }} contentStyle={{ paddingBottom: 8 }}>
            <Group label={tr('databases.tables')} />
            {tableItems.map((name) => <Item key={`t${name}`} name={name} icon={Icons.table(v.table === name && v.kind === 'table' ? '#fff' : p.dim, 14)} on={v.table === name && v.kind === 'table'}
              onClick={() => pick({ table: name, kind: 'table', pane: v.pane === 'query' || v.pane === 'users' || v.pane === 'processes' ? 'structure' : v.pane })} />)}
            {!tableItems.length ? <Text style={{ paddingHorizontal: 16, fontSize: 12, color: p.dim, textAlign: 'start' }}>{db ? tr('databases.noTables') : tr('databases.pickDb')}</Text> : null}
            {viewItems.length ? <Group label={tr('databases.views')} /> : null}
            {viewItems.map((name) => <Item key={`v${name}`} name={name} icon={Icons.eye(v.table === name && v.kind === 'view' ? '#fff' : p.view, 14)} on={v.table === name && v.kind === 'view'}
              onClick={() => pick({ table: name, kind: 'view', pane: v.pane === 'query' || v.pane === 'users' || v.pane === 'processes' ? 'structure' : v.pane })} />)}
            {routineItems.length ? <Group label={tr('databases.routines')} /> : null}
            {routineItems.map((r) => <Item key={`r${r.name}`} name={r.name} icon={Icons.fx(p.fx)} on={false} title={r.type} onClick={() => {}} />)}
          </ScrollView>
          {status && v.table ? (
            <View style={{ paddingHorizontal: 14, paddingVertical: 10, gap: 3 }}>
              <View style={{ position: 'absolute', left: 0, right: 0, top: 0, height: 1, backgroundColor: p.line }} />
              {(status.view ? [[tr('databases.info.kind'), tr('databases.info.view')]] : [
                [tr('databases.info.engine'), status.engine || '—'],
                [tr('databases.info.rows'), status.rows === null ? '—' : `~${n(status.rows)}`],
                [tr('databases.info.size'), humanBytes((status.data || 0) + (status.index || 0))],
                [tr('databases.info.encoding'), status.charset || '—'],
                [tr('databases.info.created'), String(status.created || '—').slice(0, 10)],
              ]).map(([k, val]) => (
                <View key={k} style={{ flexDirection: 'row', gap: 10 }}>
                  <Text style={{ width: 70, fontSize: 11.5, color: p.dim, textAlign: 'start' }}>{k}</Text>
                  <Text style={{ flex: 1, fontSize: 11.5, color: p.text, whiteSpace: 'nowrap', overflow: 'hidden', textAlign: 'start' }}>{val}</Text>
                </View>
              ))}
            </View>
          ) : null}
          <Foot style={{ backgroundColor: p.bar }}>
            <IconBtn glyph="+" size={17} title={tr('databases.addTableLater')} disabled onClick={() => {}} />
            <MenuButton icon="⋯" small title={tr('databases.tableActions')} items={[
              { label: tr('databases.copyName'), disabled: !v.table, onClick: () => clipboard.set(v.table) },
              { label: tr('databases.showCreate'), disabled: !v.table, onClick: () => pane('info') },
              { label: tr('databases.queryTable'), disabled: !v.table, onClick: () => { patchView(s.key, { queryInsert: `SELECT * FROM \`${v.table.replace(/`/g, '``')}\` LIMIT 100;` }); pane('query'); } },
            ]} />
            <View style={{ flex: 1 }} />
            <IconBtn glyph="↻" title={tr('databases.refreshTables')} onClick={loadTables} />
          </Foot>
        </View>
  );

  return (
    <View style={{ flex: 1, minHeight: 0 }}>
      {/* toolbar */}
      <View style={{ height: 62, flexDirection: 'row', alignItems: 'center', gap: 2, paddingHorizontal: compact ? 6 : 10, backgroundColor: p.bar }}>
        {side.narrow ? <SidebarToggle shown={side.shown} onClick={side.toggle} /> : null}
        <MenuButton minWidth={220} items={[...dbs.map((d) => ({ label: d, checked: d === db, onClick: () => pickDb(d) })), '-', { label: tr('databases.refreshDbs'), onClick: loadDbs }]}
          trigger={(open) => (
            <View style={{ flexDirection: 'row', alignItems: 'center', gap: 8, height: 30, paddingHorizontal: 10, borderRadius: 7, backgroundColor: p.control, border: `1px solid ${open ? p.accent : 'transparent'}`, maxWidth: tight ? 120 : compact ? 150 : 200 }}>
              {Icons.db(p.text, 16)}
              <Text style={{ fontSize: 13, fontWeight: 500, color: db ? p.text : p.dim, whiteSpace: 'nowrap', overflow: 'hidden' }}>{db || tr('databases.chooseDb')}</Text>
              <Text style={{ fontSize: 9, color: p.dim }}>▼</Text>
            </View>
          )} />
        <Sep gap={compact ? 4 : 8} />
        {PANES.filter(([id]) => !inMenu(id)).map(([id, label, icon, needsTable]) => (
          <ToolItem key={id} compact={compact} icon={icon} label={label} on={v.pane === id} disabled={needsTable && !tableAsks && id !== 'query'} onClick={() => pane(id)} />
        ))}
        <Sep gap={compact ? 4 : 8} />
        <View style={{ alignItems: 'center', gap: 4 }}>
          <View style={{ flexDirection: 'row', borderRadius: 7, overflow: 'hidden', backgroundColor: p.control, direction: 'ltr' }}>
            <View onClick={back} title={tr('databases.back')} style={{ width: 30, height: 26, alignItems: 'center', justifyContent: 'center', opacity: v.navAt > 0 ? 1 : 0.4 }}><Text style={{ fontSize: 13, color: p.text }}>‹</Text></View>
            <View onClick={fwd} title={tr('databases.forward')} style={{ width: 30, height: 26, alignItems: 'center', justifyContent: 'center', opacity: v.navAt < v.nav.length - 1 ? 1 : 0.4 }}><Text style={{ fontSize: 13, color: p.text }}>›</Text></View>
          </View>
          {compact ? null : <Text style={{ fontSize: 11, color: p.dim }}>{tr('databases.history')}</Text>}
        </View>
        <View style={{ flex: 1 }} />
        {compact ? null : lockedNote}
        {tight ? (
          <MenuButton title={tr('disks.more')} items={[
            { label: tr('databases.pane.relations'), checked: v.pane === 'relations', onClick: () => pane('relations') },
            { label: tr('databases.pane.triggers'), checked: v.pane === 'triggers', onClick: () => pane('triggers') }, '-',
            { label: tr('databases.pane.users'), checked: v.pane === 'users', onClick: () => pane('users') },
            { label: tr('databases.pane.processes'), checked: v.pane === 'processes', onClick: () => pane('processes') },
            { label: tr('databases.pane.console'), onClick: () => openConsole(s) },
          ]} />
        ) : (
          <>
            <ToolItem compact={compact} icon={Icons.users} label={tr('databases.pane.users')} on={v.pane === 'users'} onClick={() => pane('users')} />
            <ToolItem compact={compact} icon={Icons.pulse} label={tr('databases.pane.processes')} on={v.pane === 'processes'} onClick={() => pane('processes')} />
            <ToolItem compact={compact} icon={Icons.console} label={tr('databases.pane.console')} onClick={() => openConsole(s)} />
          </>
        )}
      </View>
      {compact && lockedNote ? <View style={{ flexDirection: 'row', paddingHorizontal: 8, paddingBottom: 6, backgroundColor: p.bar }}>{lockedNote}</View> : null}
      {tabs}
      {s.closed ? (
        <Note kind="bad" style={{ borderRadius: 0, borderLeftWidth: 0, borderRightWidth: 0 }} action={<Btn small primary label={tr('databases.reconnect')} onClick={onReconnect} />}>{closedText(s.closed)}</Note>
      ) : null}
      {error ? <Note kind="bad" style={{ borderRadius: 0 }} action={<Btn small label={tr('databases.dismiss')} onClick={() => setError('')} />}>{error}</Note> : null}
      <View style={{ flex: 1, flexDirection: 'row', minHeight: 0 }}>
        {side.narrow ? null : tablesSide}
        <View style={{ flex: 1, minWidth: 0, minHeight: 0 }}>{body}</View>
        {side.narrow && side.shown ? <SidebarOverlay width={230} onClose={side.close}>{tablesSide}</SidebarOverlay> : null}
      </View>
      {unlock ? (
        <ConfirmDialog title={tr('databases.lock.title', { name: s.fav.name })} detail={tr('databases.lock.detail')} destructive
          okLabel={tr('databases.lock.unlock')} cancelLabel={tr('databases.cancel')}
          onOk={() => { patchSession(s.key, { locked: false }); setUnlock(false); }} onCancel={() => setUnlock(false)} />
      ) : null}
    </View>
  );
}

function Group({ label }) {
  const p = usePal();
  return <Text style={{ paddingHorizontal: 16, paddingTop: 10, paddingBottom: 4, fontSize: 11, fontWeight: 600, color: p.dim, letterSpacing: 0.5, textAlign: 'start' }}>{label}</Text>;
}
function Item({ name, icon, on, onClick, title }) {
  const p = usePal();
  const [hover, setHover] = useState(false);
  return (
    <View onClick={onClick} title={title} onMouseEnter={() => setHover(true)} onMouseLeave={() => setHover(false)}
      style={{ flexDirection: 'row', alignItems: 'center', gap: 8, height: 24, marginHorizontal: 8, paddingHorizontal: 8, borderRadius: 6, backgroundColor: on ? p.accent : hover ? p.hover : 'transparent' }}>
      {icon}
      <Text style={{ flex: 1, fontSize: 12.5, color: on ? '#fff' : p.text, whiteSpace: 'nowrap', overflow: 'hidden', textAlign: 'start' }}>{name}</Text>
    </View>
  );
}
function Empty({ text }) {
  const p = usePal();
  return <View style={{ flex: 1, alignItems: 'center', justifyContent: 'center' }}><Text style={{ fontSize: 13, color: p.dim }}>{text}</Text></View>;
}

