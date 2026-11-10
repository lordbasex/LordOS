// panels.jsx — Quick Preview, Get Info and confirm (the context menu is the SDK's, sdk/contextmenu.jsx).
// All are drawn inside the window, with coordinates relative to it.
import { View, Text, TextInput, Button, ToolbarButton, Hairline, ScrollView, useTheme, formatBytes, formatDate, formatTime, direction, typeScale, spacing, t as tr, openMarkup, imageEdit, fs, ConfirmDialog } from '@lordos/sdk';
export { ContextMenu } from '@lordos/sdk';
import { useEffect, useState } from 'react';
import { Preview, kindLabel } from './views.jsx';
import { EntryIcon, TagDots, tagTint } from './glyphs.jsx';
import { tagsOf, commentOf, setComment } from './ops.js';
import { QuickContent } from './quicklook.jsx';

// Quick Preview (Space): the image, the text or the file's card, large, in a rounded panel. A picture
// (PNG, JPEG, WebP…) has Markup: the markup editor (crop, resize, redact, arrows, text; issue #30).
export function QuickLook({ e, onClose, index, count }) {
  const t = useTheme();
  if (!e) return null;
  const markup = !e.dir && !e.arc && imageEdit.canMarkup(e.name);
  return (
    <View onMouseDown={onClose} style={{ position: 'absolute', left: 0, top: 0, right: 0, bottom: 0, backgroundColor: 'rgba(0,0,0,0.35)', alignItems: 'center', justifyContent: 'center' }}>
      <View onMouseDown={(ev) => ev.stopPropagation()} style={{ width: '84%', height: '88%', borderRadius: 12, backgroundColor: t.windowBg,
        border: `1px solid ${t.separator}`, boxShadow: '0 18px 44px rgba(0,0,0,0.45)', overflow: 'hidden' }}>
        <View style={{ height: 44, flexDirection: 'row', alignItems: 'center', gap: spacing.s, paddingHorizontal: spacing.m }}>
          <Hairline edge="bottom" />
          <ToolbarButton icon="✕" iconSize={12} small title={tr('files.close')} onClick={onClose} />
          <View style={{ flex: 1, minWidth: 0 }}>
            <Text style={{ textAlign: 'center', fontSize: typeScale.headline, fontWeight: 600, color: t.text, whiteSpace: 'nowrap', overflow: 'hidden' }}>{e.name}</Text>
          </View>
          <Text style={{ minWidth: 22, fontSize: typeScale.caption, color: t.textDim, textAlign: 'end' }}>{count > 1 ? `${index + 1} / ${count}` : ''}</Text>
          {markup ? <ToolbarButton icon="✐" small label={tr('files.markup')} title={tr('files.markupHint')} onClick={() => { openMarkup(e.path); onClose(); }} /> : null}
        </View>
        <View style={{ flex: 1, padding: 8 }}>
          <QuickContent e={e} />
        </View>
      </View>
    </View>
  );
}

const fmt = (ms) => { if (!ms) return '—'; const d = new Date(ms); return `${formatDate(d, 'long')} ${formatTime(d)}`; };

function Row({ k, v, t }) {
  return (
    <View style={{ flexDirection: 'row', gap: spacing.m, paddingVertical: 4 }}>
      <Text style={{ width: 96, fontSize: typeScale.callout, color: t.textDim, textAlign: 'end' }}>{k}</Text>
      <Text style={{ flex: 1, fontSize: typeScale.callout, color: t.text, textAlign: 'start' }}>{v}</Text>
    </View>
  );
}

// Get Info (❖I): the file's card; folder sizes are computed with a bound.
// extra: what another part adds between the header and the facts, which then leave out Size and Where (Cloud's
// status, account, sizes and Keep switch: cloud.jsx CloudInfo)
export function InfoPanel({ e, onClose, openers, extra }) {
  const t = useTheme();
  const [st, setSt] = useState(null);
  const [du, setDu] = useState(null);
  const [comment, setCommentText] = useState('');
  useEffect(() => { setCommentText(e ? commentOf(e.path) : ''); }, [e?.path]);
  useEffect(() => {
    setSt(null); setDu(null);
    if (!e || e.app) return undefined;
    if (fs.isRemote(e.path)) {
      // a Cloud or network item: asked off lord-server's thread (fs.statAsync), the size is the listing's
      let live = true;
      fs.statAsync(e.path).then((s) => { if (live) setSt(s); }, (err) => { if (live) setSt({ error: err.message }); });
      if (e.dir) setDu({ remote: true });
      return () => { live = false; };
    }
    try { setSt(lord.fs.stat(e.path)); } catch (err) { setSt({ error: err.message }); }
    if (!e.dir) return undefined;
    // a cloud or network mount answers { remote: true } without walking it: the listing's size, or nothing
    const id = setTimeout(() => { try { setDu(lord.fs.du(e.path, 900) || { remote: true }); } catch (err) { setDu({ failed: true }); } }, 30);
    return () => clearTimeout(id);
  }, [e?.path]);
  if (!e) return null;
  const size = e.dir ? (!du ? tr('files.info.calculating') : du.remote || du.failed || du.size == null ? (e.size > 0 ? formatBytes(e.size) : '—')
    : `${formatBytes(du.size)}${du.complete ? '' : '+'} · ${tr('files.info.items', { count: du.files + du.dirs })}`) : formatBytes(st?.size ?? e.size);
  return (
    // a cloud item's card is taller (status, account, sizes, Keep): it takes the window's height and its body scrolls
    <View style={{ position: 'absolute', [direction() === 'rtl' ? 'left' : 'right']: 16, top: 56, ...(extra ? { bottom: 16 } : null), width: 360, borderRadius: 12,
      padding: spacing.l, gap: 10, backgroundColor: t.windowBg, border: `1px solid ${t.separator}`, boxShadow: '0 14px 36px rgba(0,0,0,0.4)' }}>
      <View style={{ flexDirection: 'row', alignItems: 'center', gap: spacing.m }}>
        <EntryIcon e={e} size={48} dark={t.mode === 'dark'} tint={tagTint(tagsOf(e.path))} />
        <View style={{ flex: 1, minWidth: 0, gap: 2 }}>
          <Text style={{ fontSize: typeScale.headline, fontWeight: 600, color: t.text, whiteSpace: 'nowrap', overflow: 'hidden', textAlign: 'start' }}>{e.name}</Text>
          <Text style={{ fontSize: typeScale.callout, color: t.textDim, textAlign: 'start' }}>{`${kindLabel(e)} · ${size}`}</Text>
        </View>
        <ToolbarButton icon="✕" iconSize={12} small title={tr('files.close')} onClick={onClose} />
      </View>
      {(() => { const body = (<>
      <TagDots tags={tagsOf(e.path)} size={9} />
      <View style={{ height: 1, backgroundColor: t.separator }} />
      {extra || null}
      {st?.error ? <Text style={{ fontSize: typeScale.callout, color: '#ff453a' }}>{st.error}</Text> : (
        <View>
          {extra ? null : <Row t={t} k={tr('files.info.size')} v={size} />}
          {extra ? null : <Row t={t} k={tr('files.info.where')} v={e.trash ? e.original : (st?.path || e.path).split('/').slice(0, -1).join('/') || '/'} />}
          {st?.link && <Row t={t} k={tr('files.info.link')} v={st.target} />}
          {st?.btime ? <Row t={t} k={tr('files.info.created')} v={fmt(st.btime)} /> : null}
          <Row t={t} k={tr('files.info.modified')} v={fmt(st?.mtime)} />
          <Row t={t} k={tr('files.info.opened')} v={fmt(st?.atime)} />
          {st && <Row t={t} k={tr('files.info.perms')} v={`${st.mode} · ${st.user}:${st.group}${st.writable ? '' : ` · ${tr('files.info.readonly')}`}`} />}
          {e.trash && <Row t={t} k={tr('files.info.deleted')} v={(e.deleted || '').replace('T', ' ')} />}
        </View>
      )}
      {!e.trash && !e.app && !e.arc ? (
        <View style={{ gap: 3 }}>
          <Text style={{ fontSize: typeScale.caption, fontWeight: 600, color: t.textDim, textAlign: 'start' }}>{tr('files.col.comments')}</Text>
          <TextInput value={comment} onChange={(v) => { setCommentText(v); setComment(e.path, v); }} placeholder={tr('files.info.addComment')} />
        </View>
      ) : null}
      {openers?.length ? (
        <View style={{ gap: 4 }}>
          <View style={{ height: 1, backgroundColor: t.separator }} />
          <Text style={{ fontSize: typeScale.caption, fontWeight: 600, color: t.textDim, marginTop: 4 }}>{tr('files.menu.openWith')}</Text>
          <View style={{ flexDirection: 'row', flexWrap: 'wrap', gap: 6 }}>
            {openers.map((o) => <Button key={o.id} label={o.label} onClick={o.run} />)}
          </View>
        </View>
      ) : null}
      </>);
      return extra ? <ScrollView style={{ flex: 1 }}><View style={{ gap: 10, paddingBottom: 4 }}>{body}</View></ScrollView> : body; })()}
    </View>
  );
}

// The SDK's ConfirmDialog over the window: Escape cancels, Enter does the default, and only while this
// window is focused. destructive (an action that can't be undone or deletes elsewhere): the OK button
// is red and Enter cancels.
export function Confirm({ text, detail, okLabel, onOk, onCancel, destructive, cautious }) {
  return <ConfirmDialog title={text} detail={detail} okLabel={okLabel} cancelLabel={tr('files.cancel')} onOk={onOk} onCancel={onCancel} destructive={destructive} cautious={cautious} />;
}
