// skins/files.jsx — Lord Bluebell's skin for Files, in the manner of a file explorer of 2001: a toolbar of big Back,
// Forward and Up buttons with Search and Views, an address bar with Go, a task pane on the side with collapsible boxes
// ("File and Folder Tasks", "Other Places", "Details"), the items as big icons or a details list, and a plain status
// bar (docs/THEMES-GUIDE.md, "App skins → Files").
//
// The skin only draws and asks: everything it does is a call to model.actions, and Files does it with its own checks
// (read-only folders, lord.fs's protected folders, confirmations, the Trash). It reads nothing from the disk itself.
import { View, Text, ScrollView, TextInput, addListener, useTheme, useThemeFont, PackIcon, formatBytes, formatDate, direction, t as tr } from '@lordos/sdk';
import { useEffect, useState } from 'react';
import { RoundArrow, Folder } from '../glyphs.jsx';

const fontOf = (font) => (font ? { fontFamily: font } : null);

// ---- toolbar ----

function BarButton({ t, font, icon, label, onClick, disabled, title, pressed }) {
  const [hover, setHover] = useState(false);
  return (
    <View onClick={disabled ? undefined : onClick} onMouseEnter={() => setHover(true)} onMouseLeave={() => setHover(false)} accessibilityLabel={title || label}
      style={{ height: 34, flexDirection: 'row', alignItems: 'center', gap: 5, paddingHorizontal: 5, borderRadius: 3, opacity: disabled ? 0.45 : 1,
        border: `1px solid ${pressed ? t.stateBorders.pressed : hover && !disabled ? t.barHoverEdge : 'transparent'}`,
        background: pressed ? t.barPressed : hover && !disabled ? t.barHover : 'transparent' }}>
      {icon}
      {label ? <Text style={{ fontSize: 12, color: t.text, whiteSpace: 'nowrap', ...fontOf(font) }}>{label}</Text> : null}
    </View>
  );
}
const Sep = ({ t }) => <View style={{ width: 1, height: 26, marginHorizontal: 3, backgroundColor: t.separator }} />;

export function Toolbar({ model }) {
  const t = useTheme();
  const font = useThemeFont();
  const a = model.actions;
  const rtl = direction() === 'rtl';
  const [searching, setSearching] = useState(!!model.query);
  const views = model.view === 'list' || model.view === 'columns' ? 'list' : 'icons';
  return (
    <View style={{ height: 40, flexDirection: 'row', alignItems: 'center', gap: 2, paddingHorizontal: 4, background: t.toolbarBg, borderBottom: `1px solid ${t.separator}` }}>
      <BarButton t={t} font={font} icon={<RoundArrow size={26} dir={rtl ? 'right' : 'left'} />} label={tr('files.menu.back')} onClick={a.back} disabled={!model.history.back} />
      <BarButton t={t} font={font} icon={<RoundArrow size={26} dir={rtl ? 'left' : 'right'} />} title={tr('files.menu.forward')} onClick={a.forward} disabled={!model.history.forward} />
      <BarButton t={t} font={font} icon={<PackIcon group="toolbar" name="up" size={26} fallback={<Folder size={24} />} />} title={tr('files.menu.up')} onClick={a.up} disabled={!model.history.up} />
      <Sep t={t} />
      <BarButton t={t} font={font} icon={<PackIcon group="toolbar" name="search" size={24} />} label={tr('files.search')} pressed={searching}
        onClick={() => { if (searching) a.search(''); setSearching(!searching); }} />
      {searching ? (
        <View style={{ width: 170 }}><TextInput value={model.query} onChange={a.search} placeholder={tr('files.search')} autoFocus onCancel={() => { a.search(''); setSearching(false); }} size={12} /></View>
      ) : null}
      <Sep t={t} />
      <BarButton t={t} font={font} icon={<PackIcon group="toolbar" name="views" size={22} />} title={tr('files.menu.view')}
        label={tr(views === 'list' ? 'files.view.list' : 'files.view.icons')} onClick={() => a.setView(views === 'list' ? 'icons' : 'list')} />
      <View style={{ flex: 1 }} />
      {model.location.writable ? <BarButton t={t} font={font} icon={<PackIcon group="toolbar" name="newFolder" size={22} />} title={tr('files.menu.newFolder')} onClick={a.newFolder} /> : null}
    </View>
  );
}

// ---- address bar ----

export function AddressBar({ model }) {
  const t = useTheme();
  const font = useThemeFont();
  const shown = model.location.path || model.location.title;
  const [text, setText] = useState(shown);
  useEffect(() => { setText(shown); }, [shown]);
  const go = () => { if (!model.actions.goAddress(text)) setText(shown); };
  return (
    <View style={{ height: 30, flexDirection: 'row', alignItems: 'center', gap: 6, paddingHorizontal: 6, backgroundColor: t.addressBg, borderBottom: `1px solid ${t.separator}` }}>
      <Text style={{ fontSize: 12, color: t.textDim, ...fontOf(font) }}>{tr('files.skin.address')}</Text>
      <View style={{ flex: 1, height: 22, flexDirection: 'row', alignItems: 'center', gap: 4, paddingStart: 3, backgroundColor: t.inputBg, border: '1px solid #7f9db9' }}>
        <Folder size={16} />
        <View style={{ flex: 1 }}><TextInput value={text} onChange={setText} onSubmit={go} size={12} style={{ height: 20, borderWidth: 0, backgroundColor: 'transparent' }} /></View>
      </View>
      <View onClick={go} style={{ height: 22, flexDirection: 'row', alignItems: 'center', gap: 4, paddingHorizontal: 6 }}>
        <RoundArrow size={18} dir={direction() === 'rtl' ? 'left' : 'right'} />
        <Text style={{ fontSize: 12, color: t.text, ...fontOf(font) }}>{tr('files.menu.go')}</Text>
      </View>
    </View>
  );
}

// ---- task pane ----

function PaneBox({ t, font, title, children, first }) {
  const [open, setOpen] = useState(true);
  return (
    <View style={{ marginTop: first ? 12 : 14, marginHorizontal: 12, borderTopLeftRadius: 4, borderTopRightRadius: 4, overflow: 'hidden', boxShadow: '0 1px 3px rgba(0,0,0,0.18)' }}>
      <View onClick={() => setOpen(!open)} style={{ height: 25, flexDirection: 'row', alignItems: 'center', paddingStart: 10, paddingEnd: 4, background: t.paneHead }}>
        <Text style={{ flex: 1, fontSize: 12, fontWeight: 700, color: t.paneHeadText, whiteSpace: 'nowrap', overflow: 'hidden', textOverflow: 'ellipsis', ...fontOf(font) }}>{title}</Text>
        <View style={{ width: 18, height: 18, borderRadius: 9, backgroundColor: '#ffffff', border: '1px solid #b6c5ec', alignItems: 'center', justifyContent: 'center' }}>
          <View style={{ width: 6, height: 6, marginTop: open ? 3 : -3, borderTopWidth: 2, borderLeftWidth: 2, borderColor: t.paneHeadText, transform: `rotate(${open ? 45 : 225}deg)` }} />
        </View>
      </View>
      {open ? <View style={{ paddingHorizontal: 10, paddingVertical: 8, gap: 5, backgroundColor: t.paneBody, borderWidth: 1, borderTopWidth: 0, borderColor: t.paneEdge }}>{children}</View> : null}
    </View>
  );
}

function Task({ t, font, icon, label, onClick }) {
  const [hover, setHover] = useState(false);
  return (
    <View onClick={onClick} onMouseEnter={() => setHover(true)} onMouseLeave={() => setHover(false)} style={{ flexDirection: 'row', alignItems: 'center', gap: 6, minHeight: 18 }}>
      <View style={{ width: 16, alignItems: 'center' }}>{icon}</View>
      <View style={{ flexShrink: 1 }}>
        <Text style={{ fontSize: 12, color: hover ? t.accent : t.paneLink, textAlign: 'start', ...fontOf(font) }}>{label}</Text>
        {hover ? <View style={{ height: 1, backgroundColor: t.accent }} /> : null}
      </View>
    </View>
  );
}

const dot = (c) => <View style={{ width: 8, height: 8, borderRadius: 2, backgroundColor: c }} />;

export function TaskPane({ model }) {
  const t = useTheme();
  const font = useThemeFont();
  const a = model.actions;
  const one = model.selection.count === 1;
  const any = model.selection.count > 0;
  const w = model.location.writable;
  const d = model.details;
  const placeIcon = (p) => (p.kind === 'folder' ? <Folder size={16} /> : dot(p.kind === 'trash' ? '#6b7b93' : p.kind === 'network' ? '#2a95b0' : '#3a74c9'));
  return (
    <View style={{ flex: 1, background: t.paneBg }}>
      <ScrollView style={{ flex: 1 }}>
        <View style={{ paddingBottom: 14 }}>
          <PaneBox t={t} font={font} first title={tr('files.skin.tasks')}>
            {model.location.type === 'trash' ? <Task t={t} font={font} icon={dot('#c8302a')} label={tr('files.menu.emptyTrash')} onClick={a.emptyTrash} /> : null}
            {w ? <Task t={t} font={font} icon={<Folder size={16} />} label={tr('files.menu.newFolder')} onClick={a.newFolder} /> : null}
            {any ? <Task t={t} font={font} icon={dot('#3a74c9')} label={tr('files.menu.open')} onClick={a.openSelection} /> : null}
            {one && w ? <Task t={t} font={font} icon={dot('#e0a020')} label={tr('files.menu.rename')} onClick={() => a.rename()} /> : null}
            {any ? <Task t={t} font={font} icon={dot('#2f8a25')} label={tr('files.menu.copy')} onClick={a.copy} /> : null}
            {w && model.canPaste ? <Task t={t} font={font} icon={dot('#2f8a25')} label={tr('files.menu.paste')} onClick={a.paste} /> : null}
            {any && w ? <Task t={t} font={font} icon={dot('#c8302a')} label={tr('files.menu.trash')} onClick={a.trash} /> : null}
            {model.location.path ? <Task t={t} font={font} icon={dot(t.text)} label={tr('files.menu.openTerminal')} onClick={a.terminal} /> : null}
          </PaneBox>
          <PaneBox t={t} font={font} title={tr('files.skin.otherPlaces')}>
            {model.places.filter((p) => !p.selected && ['home', 'desktop', 'documents', 'computer', 'network', 'trash'].includes(p.id)).map((p) => (
              <Task key={p.id} t={t} font={font} icon={placeIcon(p)} label={p.label} onClick={() => a.goPlace(p.id)} />
            ))}
          </PaneBox>
          {d ? (
            <PaneBox t={t} font={font} title={tr('files.skin.details')}>
              <Text style={{ fontSize: 12, fontWeight: 700, color: t.text, textAlign: 'start', ...fontOf(font) }}>{d.name}</Text>
              <Text style={{ fontSize: 11, color: t.text, textAlign: 'start', ...fontOf(font) }}>{tr(`files.kind.${d.kind}`)}</Text>
              {!d.dir && d.size ? <Text style={{ fontSize: 11, color: t.text, textAlign: 'start', ...fontOf(font) }}>{`${tr('files.info.size')}: ${formatBytes(d.size)}`}</Text> : null}
              {d.mtime ? <Text style={{ fontSize: 11, color: t.text, textAlign: 'start', ...fontOf(font) }}>{`${tr('files.info.modified')}: ${formatDate(new Date(d.mtime * 1000))}`}</Text> : null}
            </PaneBox>
          ) : null}
        </View>
      </ScrollView>
    </View>
  );
}

// ---- items ----

const mods = (ev) => ({ add: !!(ev.ctrl || ev.meta), range: !!ev.shift });

function RenameField({ model }) {
  const a = model.actions;
  return <TextInput value={model.renaming.value} onChange={a.renameChange} onSubmit={a.renameCommit} onCancel={a.renameCancel} autoFocus selectAll size={12} />;
}

function IconCell({ t, font, it, model }) {
  const a = model.actions;
  const Icon = model.FileIcon;
  return (
    <View onMouseDown={(ev) => { ev.stopPropagation(); if (ev.button === 2) return; a.select(it.key, mods(ev)); }} onDoubleClick={() => a.open(it.key)}
      onContextMenu={(ev) => { ev.stopPropagation?.(); if (!it.selected) a.select(it.key); a.menu(it.key, ev); }}
      style={{ width: 92, alignItems: 'center', gap: 3, paddingVertical: 4, opacity: it.cut ? 0.5 : 1 }}>
      <View style={{ padding: 2, borderRadius: 2, backgroundColor: it.selected ? 'rgba(49,106,197,0.35)' : 'transparent' }}><Icon item={it} size={48} /></View>
      {it.renaming && model.renaming ? <View style={{ width: 88 }}><RenameField model={model} /></View> : (
        <View style={{ maxWidth: 88, paddingHorizontal: 2, backgroundColor: it.selected ? t.selectBg : 'transparent' }}>
          <Text style={{ fontSize: 12, color: it.selected ? t.selectText : t.text, textAlign: 'center', ...fontOf(font) }}>{it.label}</Text>
        </View>
      )}
    </View>
  );
}

const COLS = [['name', 'files.col.name', 0], ['size', 'files.col.size', 90], ['kind', 'files.col.kind', 140], ['date', 'files.col.date', 150]];

function DetailsRow({ t, font, it, model }) {
  const a = model.actions;
  const Icon = model.FileIcon;
  const fg = it.selected ? t.selectText : t.text;
  const cell = (w, s) => <Text style={{ width: w, fontSize: 12, color: t.textDim, whiteSpace: 'nowrap', overflow: 'hidden', textOverflow: 'ellipsis', textAlign: 'start', ...fontOf(font) }}>{s}</Text>;
  return (
    <View onMouseDown={(ev) => { ev.stopPropagation(); if (ev.button === 2) return; a.select(it.key, mods(ev)); }} onDoubleClick={() => a.open(it.key)}
      onContextMenu={(ev) => { ev.stopPropagation?.(); if (!it.selected) a.select(it.key); a.menu(it.key, ev); }}
      style={{ height: 20, flexDirection: 'row', alignItems: 'center', paddingHorizontal: 6, opacity: it.cut ? 0.5 : 1 }}>
      <View style={{ flex: 1, minWidth: 0, flexDirection: 'row', alignItems: 'center', gap: 5 }}>
        <Icon item={it} size={16} />
        {it.renaming && model.renaming ? <View style={{ flex: 1 }}><RenameField model={model} /></View> : (
          <View style={{ flexShrink: 1, minWidth: 0, paddingHorizontal: 2, backgroundColor: it.selected ? t.selectBg : 'transparent' }}>
            <Text style={{ fontSize: 12, color: fg, whiteSpace: 'nowrap', overflow: 'hidden', textOverflow: 'ellipsis', textAlign: 'start', ...fontOf(font) }}>{it.label}</Text>
          </View>
        )}
      </View>
      {cell(90, it.dir ? '' : formatBytes(it.size))}
      {cell(140, tr(`files.kind.${it.kind}`))}
      {cell(150, it.mtime ? formatDate(new Date(it.mtime * 1000)) : '')}
    </View>
  );
}

export function ItemView({ model }) {
  const t = useTheme();
  const font = useThemeFont();
  const a = model.actions;
  const details = model.view === 'list' || model.view === 'columns';
  const bg = (ev) => { if (ev.button === 2) return; a.clearSelection(); };
  return (
    <View style={{ flex: 1, backgroundColor: t.windowBg }} onMouseDown={bg} onContextMenu={(ev) => a.menu(null, ev)}>
      {details ? (
        <View style={{ height: 22, flexDirection: 'row', alignItems: 'center', paddingHorizontal: 6, background: t.listHead, borderBottom: `1px solid ${t.separator}` }}>
          {COLS.map(([k, label, w]) => (
            <View key={k} onClick={() => a.sortBy(k)} style={{ ...(w ? { width: w } : { flex: 1 }), height: 22, justifyContent: 'center', borderEndWidth: 1, borderEndColor: t.separator, paddingStart: 4 }}>
              <Text style={{ fontSize: 12, color: t.text, textAlign: 'start', whiteSpace: 'nowrap', ...fontOf(font) }}>{tr(label)}{model.sort.by === k ? (model.sort.desc ? ' ▾' : ' ▴') : ''}</Text>
            </View>
          ))}
        </View>
      ) : null}
      <ScrollView style={{ flex: 1 }}>
        {model.items.length ? (details
          ? <View style={{ paddingVertical: 2 }}>{model.items.map((it) => <DetailsRow key={it.key} t={t} font={font} it={it} model={model} />)}</View>
          : <View style={{ flexDirection: 'row', flexWrap: 'wrap', padding: 10, gap: 6 }}>{model.items.map((it) => <IconCell key={it.key} t={t} font={font} it={it} model={model} />)}</View>)
          : <Text style={{ margin: 20, fontSize: 12, color: t.textDim, textAlign: 'center', ...fontOf(font) }}>{model.empty || ''}</Text>}
      </ScrollView>
    </View>
  );
}

// ---- status bar ----

export function StatusBar({ model }) {
  const t = useTheme();
  const font = useThemeFont();
  const job = model.status.job;
  return (
    <View style={{ height: 22, flexDirection: 'row', alignItems: 'center', gap: 8, paddingHorizontal: 6, backgroundColor: t.statusBg, borderTop: `1px solid ${t.separator}` }}>
      {job ? (
        <View style={{ flex: 1, flexDirection: 'row', alignItems: 'center', gap: 8 }}>
          <Text style={{ fontSize: 11, color: t.text, whiteSpace: 'nowrap', ...fontOf(font) }}>{job.label}</Text>
          <View style={{ width: 140, height: 12, border: `1px solid ${t.separator}`, backgroundColor: t.inputBg, padding: 1, flexDirection: 'row', gap: 2, overflow: 'hidden', direction: 'ltr' }}>
            {Array.from({ length: Math.round(job.fraction * 17) }, (_, i) => <View key={i} style={{ width: 6, height: 8, backgroundColor: '#2fb32f' }} />)}
          </View>
        </View>
      ) : (
        <Text style={{ flex: 1, fontSize: 11, color: model.status.message ? t.accent : t.text, whiteSpace: 'nowrap', overflow: 'hidden', textOverflow: 'ellipsis', textAlign: 'start', ...fontOf(font) }}>
          {model.status.text}
        </Text>
      )}
      {model.location.readOnly ? <Text style={{ fontSize: 11, color: t.textDim, ...fontOf(font) }}>{tr('files.info.readonly')}</Text> : null}
    </View>
  );
}

// ---- context menu ----

function MenuRow({ t, font, it, onDone }) {
  const [hover, setHover] = useState(false);
  if (it === '-') return <View style={{ height: 1, marginVertical: 3, marginHorizontal: 2, backgroundColor: t.separator }} />;
  if (it.header) return <Text style={{ fontSize: 11, fontWeight: 700, color: t.textDim, paddingHorizontal: 22, paddingVertical: 3, textAlign: 'start', ...fontOf(font) }}>{it.header}</Text>;
  if (it.note) return <Text style={{ fontSize: 11, color: t.textDim, paddingHorizontal: 22, paddingVertical: 3, textAlign: 'start', ...fontOf(font) }}>{it.note}</Text>;
  if (it.swatches) {
    return (
      <View style={{ flexDirection: 'row', gap: 6, paddingHorizontal: 22, paddingVertical: 4 }}>
        {it.swatches.map((sw) => <View key={sw.color} onClick={() => { sw.onClick(); onDone(); }} style={{ width: 13, height: 13, borderRadius: 7, backgroundColor: sw.hex, border: sw.on ? `2px solid ${t.text}` : '1px solid rgba(0,0,0,0.3)' }} />)}
      </View>
    );
  }
  const lit = hover && !it.disabled;
  const sub = Array.isArray(it.submenu) ? it.submenu : null;
  return (
    <View onMouseEnter={() => setHover(true)} onMouseLeave={() => setHover(false)}
      onClick={() => { if (it.disabled || sub) return; onDone(); it.onClick?.(); }}
      style={{ height: 22, flexDirection: 'row', alignItems: 'center', paddingStart: 22, paddingEnd: 10, gap: 12, backgroundColor: lit ? t.accent : 'transparent', opacity: it.disabled ? 0.45 : 1 }}>
      {it.checked ? <Text style={{ position: 'absolute', left: 7, fontSize: 11, color: lit ? '#fff' : t.text }}>✓</Text> : null}
      <Text style={{ flex: 1, fontSize: 12, color: lit ? '#ffffff' : it.danger ? '#c8302a' : t.text, whiteSpace: 'nowrap', textAlign: 'start', ...fontOf(font) }}>{it.label}</Text>
      {sub ? <Text style={{ fontSize: 9, color: lit ? '#fff' : t.text }}>▶</Text> : it.hint ? <Text style={{ fontSize: 11, color: lit ? '#fff' : t.textDim, ...fontOf(font) }}>{it.hint}</Text> : null}
      {sub && hover ? (
        <View style={{ position: 'absolute', insetInlineStart: 186, top: -3 }}><MenuBox t={t} font={font} items={sub} onDone={onDone} /></View>
      ) : null}
    </View>
  );
}

function MenuBox({ t, font, items, onDone }) {
  return (
    <View style={{ minWidth: 190, paddingVertical: 2, backgroundColor: t.popupBg, border: '1px solid #868686', boxShadow: '2px 2px 3px rgba(0,0,0,0.3)' }}>
      {items.map((it, i) => <MenuRow key={i} t={t} font={font} it={it} onDone={onDone} />)}
    </View>
  );
}

export function ContextMenu({ model }) {
  const t = useTheme();
  const font = useThemeFont();
  useEffect(() => addListener('keydown', (e) => { if (e.key === 'Escape') model.close(); }), [model.close]);
  const b = model.bounds || { w: 800, h: 600 };
  const rows = model.items.reduce((h, it) => h + (it === '-' ? 7 : it.swatches ? 22 : 22), 6);
  const x = Math.max(0, Math.min(model.x, b.w - 200)), y = Math.max(0, Math.min(model.y, b.h - rows));
  return (
    <View style={{ position: 'absolute', left: 0, top: 0, right: 0, bottom: 0 }} onMouseDown={model.close} onContextMenu={model.close}>
      <View style={{ position: 'absolute', left: x, top: y }} onMouseDown={(ev) => ev.stopPropagation()}>
        <MenuBox t={t} font={font} items={model.items} onDone={model.close} />
      </View>
    </View>
  );
}

export const filesSkin = { toolbar: Toolbar, addressBar: AddressBar, sidebar: TaskPane, itemView: ItemView, statusBar: StatusBar, contextMenu: ContextMenu };
