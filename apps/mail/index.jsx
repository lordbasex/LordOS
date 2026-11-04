// Mail — a native mail app in the spirit of a classic three-pane client: accounts and folders,
// the message list with previews (conversations grouped), and the reading pane. Everything comes
// from the sandboxed helper lord-maild (docs/MAIL.md): IMAP with IDLE, SMTP, a local cache with
// an offline search index, and HTML turned into our own views without running anything.
import {
  View, Text, useTheme, useStore, useAppWindow, useKeyboard, Button, TextInput, ScrollView, t, wm, fs, monthName, formatDate, formatTime,
  Toolbar, ToolbarButton, ToolbarSpacer, ToolbarDivider, SearchField, Hairline, direction, textDirection,
  mailRequest, onMailEvent, mailStatus, mailConfig, mailAccounts, mailAvailable, addListener, clipboard, registerDockActions,
  trackMailStatus, isolatedPim, saveFile,
} from '@lordos/sdk';
import { useEffect, useRef, useState } from 'react';
import { HtmlView, TextView } from './HtmlView.jsx';
import { Compose } from './Compose.jsx';
import { AccountSetup, AccountsPanel, CertDialog, PasswordPrompt, Sheet } from './Setup.jsx';
import { CATEGORY_COLORS, categoryName, listDate, fullDate, folderName, addrShort, formatSize, openPath, revealPath } from './util.js';
import { quote } from './md.js';
import { safeName } from './util.js';

const ROW = 86;                 // sender, subject and two lines of preview, with room to breathe

// ---- icon: an envelope on a blue tile (our own drawing) ----

export function Icon({ size: s }) {
  return (
    <View style={{ width: s, height: s, borderRadius: s * 0.225, background: 'linear-gradient(165deg, #3fa9ff, #0a5fd0)', overflow: 'hidden',
      boxShadow: `0 ${s * 0.04}px ${s * 0.12}px rgba(0,0,0,0.35)` }}>
      <View style={{ position: 'absolute', left: s * 0.16, top: s * 0.27, width: s * 0.68, height: s * 0.46, borderRadius: s * 0.06, backgroundColor: '#ffffff',
        boxShadow: `0 ${s * 0.02}px ${s * 0.05}px rgba(0,0,0,0.25)` }} />
      {/* the flap: two stacked bands narrowing to a point */}
      {[0, 1, 2, 3, 4].map((i) => (
        <View key={i} style={{ position: 'absolute', left: s * (0.2 + i * 0.06), top: s * (0.31 + i * 0.045), width: s * (0.6 - i * 0.12), height: Math.max(1, s * 0.04),
          borderRadius: s * 0.02, backgroundColor: i === 4 ? '#0a5fd0' : '#cfe6ff' }} />
      ))}
    </View>
  );
}

// ---- monochrome glyphs (the folder emoji are in color and ignore the theme) ----

// Shapes drawn with views: an inbox tray, an archive box, a trash can and a folder.
export function Glyph({ kind, size = 16, color }) {
  const th = useTheme();
  const c = color || th.accent;
  const s = size, line = 1.5;
  if (kind === 'tray') {
    return (
      <View style={{ width: s, height: s, justifyContent: 'center', alignItems: 'center' }}>
        <View style={{ width: s * 0.86, height: s * 0.62, borderRadius: s * 0.14, border: `${line}px solid ${c}` }}>
          <View style={{ position: 'absolute', left: 0, right: 0, top: s * 0.22, height: line, backgroundColor: c }} />
        </View>
      </View>
    );
  }
  if (kind === 'archive') {
    return (
      <View style={{ width: s, height: s, justifyContent: 'center', alignItems: 'center', gap: 1 }}>
        <View style={{ width: s * 0.9, height: s * 0.2, borderRadius: 1.5, backgroundColor: c }} />
        <View style={{ width: s * 0.76, height: s * 0.48, borderBottomLeftRadius: s * 0.1, borderBottomRightRadius: s * 0.1, border: `${line}px solid ${c}`, alignItems: 'center' }}>
          <View style={{ marginTop: s * 0.1, width: s * 0.28, height: line, backgroundColor: c }} />
        </View>
      </View>
    );
  }
  if (kind === 'trash') {
    return (
      <View style={{ width: s, height: s, justifyContent: 'center', alignItems: 'center', gap: 1 }}>
        <View style={{ width: s * 0.3, height: s * 0.1, borderTopLeftRadius: 1.5, borderTopRightRadius: 1.5, backgroundColor: c }} />
        <View style={{ width: s * 0.8, height: line, backgroundColor: c }} />
        <View style={{ width: s * 0.6, height: s * 0.54, borderBottomLeftRadius: s * 0.1, borderBottomRightRadius: s * 0.1, border: `${line}px solid ${c}` }} />
      </View>
    );
  }
  if (kind === 'folder') {
    return (
      <View style={{ width: s, height: s, justifyContent: 'center', alignItems: 'center' }}>
        <View style={{ width: s * 0.86, height: s * 0.66 }}>
          <View style={{ width: s * 0.36, height: s * 0.16, borderTopLeftRadius: s * 0.08, borderTopRightRadius: s * 0.08, backgroundColor: c }} />
          <View style={{ flex: 1, borderRadius: s * 0.1, borderTopLeftRadius: 0, border: `${line}px solid ${c}` }} />
        </View>
      </View>
    );
  }
  if (kind === 'calendar') {
    return (
      <View style={{ width: s, height: s, justifyContent: 'center', alignItems: 'center' }}>
        <View style={{ width: s * 0.8, height: s * 0.74, borderRadius: s * 0.14, border: `${line}px solid ${c}`, overflow: 'hidden' }}>
          <View style={{ height: s * 0.18, backgroundColor: c }} />
        </View>
      </View>
    );
  }
  return null;
}

// The icon of a folder: a drawn shape or a monochrome glyph.
const FOLDER_GLYPHS = { inbox: 'tray', archive: 'archive', trash: 'trash' };
const FOLDER_CHARS = { drafts: '✎', sent: '➤', junk: '⚠', all: '≡', flagged: '⚑' };
function FolderIcon({ f, color }) {
  const th = useTheme();
  const c = color || th.accent;
  const g = FOLDER_GLYPHS[f.special];
  if (g) return <Glyph kind={g} size={16} color={c} />;
  const ch = FOLDER_CHARS[f.special];
  if (ch) return <Text style={{ fontSize: f.special === 'sent' ? 12 : 13, color: c, textAlign: 'center' }}>{ch}</Text>;
  return <Glyph kind="folder" size={16} color={c} />;
}

// ---- the list ----

function useSize(ref, every = 250) {
  const [sz, setSz] = useState({ w: 400, h: 400 });
  useEffect(() => {
    const id = setInterval(() => {
      if (!ref.current) return;
      const [, , w, h] = lord.getLayout(ref.current.id);
      setSz((o) => (Math.abs(o.w - w) > 1 || Math.abs(o.h - h) > 1 ? { w, h } : o));
    }, every);
    return () => clearInterval(id);
  }, []);
  return sz;
}

function Row({ it, selected, onClick, onRightClick, cfg }) {
  const th = useTheme();
  if (!it) return <View style={{ height: ROW }} />;
  const unread = it.unreadN !== undefined ? it.unreadN > 0 : !it.seen;
  const fg = selected ? '#fff' : th.text, dim = selected ? 'rgba(255,255,255,0.8)' : th.textDim;
  return (
    <View onClick={onClick} onContextMenu={onRightClick} style={{ height: ROW, paddingHorizontal: 6, paddingVertical: 2 }}>
      <View style={{ flex: 1, flexDirection: 'row', paddingHorizontal: 8, paddingVertical: 8, gap: 6, borderRadius: 7,
        backgroundColor: selected ? th.accent : 'transparent' }}>
        <View style={{ width: 10, alignItems: 'center', paddingTop: 5 }}>
          {unread ? <View style={{ width: 8, height: 8, borderRadius: 4, backgroundColor: selected ? '#fff' : th.accent }} /> : null}
        </View>
        <View style={{ flex: 1, gap: 1, overflow: 'hidden' }}>
          <View style={{ flexDirection: 'row', alignItems: 'center', gap: 6, height: 18 }}>
            <Text style={{ flex: 1, fontSize: 13, fontWeight: unread ? 600 : 500, color: fg, whiteSpace: 'nowrap', overflow: 'hidden', textOverflow: 'ellipsis' }}>{it.from || it.email || t('mail.unknownSender')}</Text>
            {it.n > 1 ? <View style={{ minWidth: 18, height: 15, paddingHorizontal: 5, borderRadius: 8, alignItems: 'center', justifyContent: 'center', backgroundColor: selected ? 'rgba(255,255,255,0.22)' : th.control }}><Text style={{ fontSize: 10, fontWeight: 600, color: fg }}>{String(it.n)}</Text></View> : null}
            {it.att ? <Text style={{ fontSize: 12, color: dim }}>⧉</Text> : null}
            {(it.anyFlag || it.flagged) ? <Text style={{ fontSize: 11, color: selected ? '#fff' : '#ff9f0a' }}>⚑</Text> : null}
            <Text style={{ fontSize: 11, color: dim, whiteSpace: 'nowrap' }}>{listDate(it.date)}</Text>
          </View>
          <View style={{ flexDirection: 'row', alignItems: 'center', gap: 4, height: 17 }}>
            {(it.labels || []).map((l) => <View key={l} style={{ width: 8, height: 8, borderRadius: 4, backgroundColor: CATEGORY_COLORS[l - 1], border: selected ? '1px solid #fff' : 'none' }} />)}
            {it.answered ? <Text style={{ fontSize: 11, color: dim }}>↩</Text> : null}
            <Text style={{ flex: 1, fontSize: 12, fontWeight: unread ? 600 : 400, color: fg, whiteSpace: 'nowrap', overflow: 'hidden', textOverflow: 'ellipsis' }}>{it.subject || t('mail.noSubject')}</Text>
          </View>
          <Text style={{ fontSize: 12, color: dim, overflow: 'hidden', height: 32, lineHeight: 16, numberOfLines: 2 }}>{it.preview}</Text>
        </View>
      </View>
      {cfg ? null : null}
    </View>
  );
}

// A virtual list: only the visible rows exist; rows load in windows from lord-maild as you scroll.
function MessageList({ src, rev, selected, onSelect, onMenu, cfg }) {
  const th = useTheme();
  const box = useRef(null);
  const { h } = useSize(box);
  const [y, setY] = useState(0);
  const [data, setData] = useState({ total: 0, items: new Map(), ver: null, loading: true });
  const want = useRef(null);
  const lastRev = useRef(rev);
  const srcKey = JSON.stringify(src);
  const lastSrc = useRef('');
  useEffect(() => { setY(0); }, [srcKey]);
  const first = Math.max(0, Math.floor(y / ROW) - 10), last = Math.ceil((y + h) / ROW) + 20;
  useEffect(() => {
    let live = true;
    const key = `${srcKey}|${first}|${rev}`;
    want.current = key;
    const run = async () => {
      try {
        let r;
        if (src.kind === 'search') r = await mailRequest('search', { q: src.q, count: 1000 });
        else r = await mailRequest('list', { ...(src.kind === 'unified' ? { folder: '*inbox' } : { acct: src.acct, folder: src.folder }), threaded: true,
          offset: first, count: last - first, filter: src.filter || 'all', ...(src.label ? { label: src.label } : {}) });
        if (!live || want.current !== key) return;
        setData((old) => {
          const items = src.kind === 'search' || old.ver !== r.version || old.src !== srcKey ? new Map() : new Map(old.items);
          const off = src.kind === 'search' ? 0 : first;
          r.items.forEach((it, k) => items.set(off + k, it));
          return { total: r.total, items, ver: r.version, loading: false, src: srcKey };
        });
      } catch (e) { if (live) setData((o) => ({ ...o, loading: false, error: e.code })); }
    };
    const fresh = lastSrc.current !== srcKey;
    lastSrc.current = srcKey;
    const missing = fresh || data.src !== srcKey || (() => { for (let i = first; i < Math.min(last, data.total || last); i++) if (!data.items.has(i)) return true; return data.ver === null; })();
    if (missing || rev !== lastRev.current) { lastRev.current = rev; run(); }
    return () => { live = false; };
  }, [srcKey, first, rev]);
  const total = data.src === srcKey ? data.total : 0;
  const max = Math.max(0, total * ROW - h);
  const yy = Math.min(y, max);
  const rows = [];
  for (let i = Math.max(0, Math.floor(yy / ROW) - 2); i < Math.min(total, Math.ceil((yy + h) / ROW) + 2); i++) {
    const it = data.src === srcKey ? data.items.get(i) : null;
    const key = it ? `${it.acct}|${it.folder}|${it.uid}` : `ph${i}`;
    rows.push(
      <View key={key} style={{ position: 'absolute', left: 0, right: 0, top: i * ROW - yy }}>
        <Row it={it} cfg={cfg} selected={!!it && selected === key} onClick={() => it && onSelect(it, i)} onRightClick={(e) => it && onMenu(it, e)} />
        <View style={{ position: 'absolute', bottom: 0, height: 1, [direction() === 'rtl' ? 'right' : 'left']: 30, [direction() === 'rtl' ? 'left' : 'right']: 14, backgroundColor: th.separator }} />
      </View>,
    );
  }
  const bar = total * ROW > h ? Math.max(28, (h * h) / (total * ROW)) : 0;
  const drag = useRef(null);
  useEffect(() => {
    const offMove = addListener('pointermove', (e) => { if (drag.current) setY(Math.max(0, Math.min(max, drag.current.y0 + ((e.y - drag.current.py) * total * ROW) / Math.max(1, h)))); });
    const offUp = addListener('pointerup', () => { drag.current = null; });
    return () => { offMove(); offUp(); };
  });
  // keyboard selection needs the neighbours
  MessageList.neighbour = (key, d) => {
    for (const [i, it] of data.items) if (`${it.acct}|${it.folder}|${it.uid}` === key) {
      const n = data.items.get(i + d);
      if (n) { const ny = (i + d) * ROW; if (ny < yy) setY(ny); else if (ny + ROW > yy + h) setY(ny + ROW - h); }
      return n ? { it: n, i: i + d } : null;
    }
    return null;
  };
  return (
    <View ref={box} onWheel={(e) => setY((v) => Math.max(0, Math.min(max, v + e.deltaY)))} style={{ flex: 1, overflow: 'hidden' }}>
      {rows}
      {!total && !data.loading ? (
        <View style={{ flex: 1, alignItems: 'center', justifyContent: 'center', padding: 20 }}>
          <Text style={{ fontSize: 13, color: th.textDim, textAlign: 'center' }}>{src.kind === 'search' ? t('mail.search.none') : t('mail.empty')}</Text>
        </View>
      ) : null}
      {bar > 0 && <View onMouseDown={(e) => { drag.current = { py: e.y, y0: yy }; }} style={{ position: 'absolute', [direction() === 'rtl' ? 'left' : 'right']: 2, width: 6, borderRadius: 3, height: bar,
        top: (h - bar) * (yy / (max || 1)), backgroundColor: th.mode === 'dark' ? 'rgba(255,255,255,0.3)' : 'rgba(0,0,0,0.28)' }} />}
    </View>
  );
}

// ---- the reading pane ----

function Banner({ color, children }) {
  return <View style={{ flexDirection: 'row', alignItems: 'center', gap: 8, padding: 8, paddingHorizontal: 12, borderRadius: 8, backgroundColor: color }}>{children}</View>;
}

function Invite({ msg }) {
  const th = useTheme();
  const [inv, setInv] = useState(null);
  const [done, setDone] = useState('');
  const [busy, setBusy] = useState(false);
  useEffect(() => {
    let live = true;
    setInv(null); setDone('');
    mailRequest('invite', { acct: msg.acct, folder: msg.folder, uid: msg.uid }).then((r) => { if (live) setInv(r); }).catch(() => {});
    return () => { live = false; };
  }, [msg.uid, msg.folder]);
  if (!inv) return null;
  const ev = inv.event;
  const when = ev.allDay ? formatDate(new Date(ev.start), 'long') : `${fullDate(ev.start)} – ${formatTime(new Date(ev.end))}`;
  const answer = async (ps) => {
    setBusy(true);
    try { await mailRequest('invite-respond', { acct: msg.acct, folder: msg.folder, uid: msg.uid, partstat: ps }); setDone(ps); } catch (e) { setDone(`error:${e.message}`); }
    setBusy(false);
  };
  const status = done || inv.myStatus || '';
  const isReply = inv.method === 'REPLY', isCancel = inv.method === 'CANCEL';
  return (
    <View style={{ padding: 12, borderRadius: 10, backgroundColor: th.control, gap: 6 }}>
      <View style={{ flexDirection: 'row', alignItems: 'center', gap: 10 }}>
        <View style={{ width: 44, height: 48, borderRadius: 8, backgroundColor: '#fff', overflow: 'hidden', alignItems: 'center' }}>
          <View style={{ height: 14, alignSelf: 'stretch', backgroundColor: '#ff453a', alignItems: 'center', justifyContent: 'center' }}>
            <Text style={{ fontSize: 9, color: '#fff', fontWeight: 700 }}>{monthName(new Date(ev.start).getMonth(), true).toUpperCase()}</Text>
          </View>
          <Text style={{ fontSize: 20, fontWeight: 700, color: '#222' }}>{String(new Date(ev.start).getDate())}</Text>
        </View>
        <View style={{ flex: 1, gap: 2 }}>
          <Text style={{ fontSize: 14, fontWeight: 600, color: th.text }}>{ev.title || t('mail.invite.untitled')}</Text>
          <Text style={{ fontSize: 12, color: th.textDim }}>{when}</Text>
          {ev.loc ? <Text style={{ fontSize: 12, color: th.textDim }}>{ev.loc}</Text> : null}
          {ev.org ? <Text style={{ fontSize: 12, color: th.textDim }}>{t('mail.invite.organizer', { name: ev.org.name || ev.org.email })}</Text> : null}
        </View>
      </View>
      {isReply ? <Text style={{ fontSize: 12, color: th.textDim }}>{t('mail.invite.reply')}</Text>
        : isCancel ? <Text style={{ fontSize: 12, color: '#ff453a' }}>{t('mail.invite.cancelled')}</Text>
          : (
            <View style={{ flexDirection: 'row', alignItems: 'center', gap: 8 }}>
              <Button primary={status !== 'ACCEPTED'} label={status === 'ACCEPTED' ? `✓ ${t('mail.invite.accepted')}` : t('mail.invite.accept')} disabled={busy} onClick={() => answer('ACCEPTED')} />
              <Button label={status === 'TENTATIVE' ? `✓ ${t('mail.invite.maybe')}` : t('mail.invite.maybe')} disabled={busy} onClick={() => answer('TENTATIVE')} />
              <Button destructive label={status === 'DECLINED' ? `✓ ${t('mail.invite.declined')}` : t('mail.invite.decline')} disabled={busy} onClick={() => answer('DECLINED')} />
              {inv.inCalendar || done === 'ACCEPTED' || done === 'TENTATIVE' ? <Text onClick={() => wm.open('calendar', { args: { at: ev.start, uid: ev.uid, nonce: Date.now() } })} style={{ fontSize: 12, color: th.accent }}>{t('mail.invite.show')}</Text> : null}
            </View>
          )}
      {done.startsWith('error:') ? <Text style={{ fontSize: 12, color: '#ff453a' }}>{done.slice(6)}</Text> : null}
    </View>
  );
}

function Attachments({ msg }) {
  const th = useTheme();
  const [busy, setBusy] = useState(null);
  const atts = (msg.atts || []).filter((a) => !(a.inline && a.cid && msg.html));
  if (!atts.length) return null;
  const rel = async (a) => (await mailRequest('attachment', { acct: msg.acct, folder: msg.folder, uid: msg.uid, idx: a.idx })).path;
  const fetch = async (a) => `${lord.mail.cacheDir()}/${await rel(a)}`;
  const open = async (a) => {
    setBusy(a.idx);
    // in its own process Mail cannot open files: lord-server checks the cached attachment and the
    // session opens it with the user's apps
    try { if (isolatedPim()) lord.mail.openAttachment(await rel(a), a.name); else openPath(await fetch(a), a.name); } catch (e) { /* shown in the row */ }
    setBusy(null);
  };
  const save = async (a) => {
    setBusy(a.idx);
    if (isolatedPim()) {
      // a read-only fd of the cached attachment, then the system's save dialog
      try {
        const fd = lord.mail.attachmentFd(await rel(a));
        if (fd >= 0) { const data = lord.broker.fdRead(fd); lord.broker.fdClose(fd); await saveFile(safeName(a.name), data); }
      } catch (e) { /* cancelled or restricted */ }
      setBusy(null);
      return;
    }
    try {
      const p = await fetch(a);
      const dir = ['Descargas', 'Downloads', 'Transferências'].map((d) => `${fs.home()}/${d}`).find((d) => { try { return fs.stat(d); } catch (e) { return false; } }) || fs.home();
      const name = safeName(a.name);
      let dst = `${dir}/${name}`;
      for (let k = 2; k < 100; k++) { let ex = false; try { ex = !!fs.stat(dst); } catch (e) { ex = false; } if (!ex) break; dst = `${dir}/${name.replace(/(\.[^.]*)?$/, ` (${k})$1`)}`; }
      if (fs.parent(dst) !== dir) return;              /* never outside the downloads folder */
      fs.copy(p, dst);
      setTimeout(() => revealPath(dst), 300);
    } catch (e) { /* ignore */ }
    setBusy(null);
  };
  return (
    <View style={{ flexDirection: 'row', flexWrap: 'wrap', gap: 8 }}>
      {atts.map((a) => (
        <View key={a.idx} style={{ flexDirection: 'row', alignItems: 'center', gap: 8, paddingVertical: 6, paddingHorizontal: 8, borderRadius: 8, backgroundColor: th.control,
          border: `1px solid ${th.separator}`, maxWidth: 360, flexShrink: 1, minWidth: 0 }}>
          <DocGlyph name={a.name} type={a.type} />
          <View style={{ flex: 1, minWidth: 40 }}>
            <Text onClick={() => open(a)} style={{ fontSize: 12, fontWeight: 500, color: th.text, whiteSpace: 'nowrap', overflow: 'hidden' }}>{safeName(a.name)}</Text>
            <Text style={{ fontSize: 11, color: th.textDim }}>{busy === a.idx ? t('mail.working') : formatSize(a.size)}</Text>
          </View>
          <Button small plain label={t('mail.att.open')} onClick={() => open(a)} />
          <Button small plain label={t('mail.att.save')} onClick={() => save(a)} />
        </View>
      ))}
    </View>
  );
}

// A document page with its extension on a colored band (instead of the color emoji).
function DocGlyph({ name, type }) {
  const ext = (/\.([a-z0-9]{1,4})$/i.exec(name || '') || [])[1] || '';
  const color = /pdf/.test(type) ? '#e5483f' : /^image/.test(type) ? '#30a46c' : /calendar|ics/.test(`${type}${name}`) ? '#f2463d' : /zip|compress/.test(type) ? '#8e8e93' : '#2f7cf6';
  return (
    <View style={{ width: 24, height: 30, borderRadius: 3, backgroundColor: '#ffffff', border: '1px solid rgba(0,0,0,0.15)', justifyContent: 'flex-end', overflow: 'hidden' }}>
      <View style={{ height: 11, backgroundColor: color, alignItems: 'center', justifyContent: 'center' }}>
        <Text style={{ fontSize: 7, fontWeight: 700, color: '#fff' }}>{ext.toUpperCase()}</Text>
      </View>
    </View>
  );
}

function MessageView({ msg, width, onLink, cfg, onTrustSender, collapsed, onExpand }) {
  const th = useTheme();
  const [remote, setRemote] = useState(false);
  const sender = msg.from?.[0]?.email?.toLowerCase() || '';
  const trusted = (cfg.trustedSenders || []).includes(sender);
  useEffect(() => { setRemote(false); }, [msg.uid, msg.folder]);
  const showRemote = remote || trusted;
  // the grant has to exist before the images ask for their hosts (children's effects run first)
  const granted = useRef('');
  const gkey = `${msg.acct}|${msg.folder}|${msg.uid}`;
  if (showRemote && msg.html?.remote?.length && granted.current !== gkey) { lord.mail.allowImages(msg.html.remote); granted.current = gkey; }
  const from = msg.from?.[0] || {};
  const nRemote = msg.html?.stats?.remote || 0, nTrackers = msg.html?.stats?.trackers || 0;
  const warns = (msg.html?.links || []).filter((l) => l.w);
  if (collapsed) {
    return (
      <View onClick={onExpand} style={{ paddingVertical: 9, paddingHorizontal: 12, borderRadius: 8, backgroundColor: th.control, flexDirection: 'row', alignItems: 'center', gap: 10 }}>
        <Text style={{ fontSize: 13, fontWeight: 600, color: th.text, whiteSpace: 'nowrap' }}>{from.name || from.email}</Text>
        <Text style={{ flex: 1, fontSize: 12, color: th.textDim, whiteSpace: 'nowrap', overflow: 'hidden', direction: textDirection(msg.text || '') }}>{(msg.text || '').slice(0, 140).replace(/\s+/g, ' ')}</Text>
        <Text style={{ fontSize: 11, color: th.textDim }}>{listDate(msg.date)}</Text>
      </View>
    );
  }
  return (
    <View style={{ gap: 12 }}>
      <View style={{ flexDirection: 'row', gap: 12, alignItems: 'flex-start' }}>
        <View style={{ width: 40, height: 40, borderRadius: 20, background: 'linear-gradient(180deg, #a5a5ad, #85858d)', alignItems: 'center', justifyContent: 'center' }}>
          <Text style={{ fontSize: 16, fontWeight: 600, color: '#fff' }}>{([...(from.name || from.email || '?')][0] || '?').toUpperCase()}</Text>
        </View>
        <View style={{ flex: 1, gap: 2, overflow: 'hidden' }}>
          <View style={{ flexDirection: 'row', alignItems: 'center', gap: 10 }}>
            <Text style={{ flex: 1, fontSize: 14, fontWeight: 600, color: th.text, whiteSpace: 'nowrap', overflow: 'hidden' }}>{from.name || from.email}</Text>
            {width > 420 ? <Text style={{ fontSize: 11, color: th.textDim, whiteSpace: 'nowrap' }}>{fullDate(msg.date)}</Text> : null}
          </View>
          {from.name ? <Text style={{ fontSize: 12, color: th.textDim, whiteSpace: 'nowrap', overflow: 'hidden' }}>{from.email}</Text> : null}
          <Text style={{ fontSize: 12, color: th.textDim }}>{`${t('mail.to')}: ${(msg.to || []).map(addrShort).join(', ')}`}</Text>
          {msg.cc?.length ? <Text style={{ fontSize: 12, color: th.textDim }}>{`${t('mail.cc')}: ${msg.cc.map(addrShort).join(', ')}`}</Text> : null}
          {width > 420 ? null : <Text style={{ fontSize: 11, color: th.textDim }}>{fullDate(msg.date)}</Text>}
        </View>
      </View>
      <View style={{ height: 1, backgroundColor: th.separator }} />
      {nRemote > 0 && !showRemote && (
        <Banner color={th.control}>
          <Text style={{ fontSize: 12, color: th.text, flex: 1 }}>{t('mail.images.blocked', { count: nRemote })}{nTrackers ? ` ${t('mail.images.trackers', { count: nTrackers })}` : ''}</Text>
          <Button small label={t('mail.images.load')} onClick={() => setRemote(true)} />
          <Button small plain label={t('mail.images.always')} onClick={() => { onTrustSender(sender); setRemote(true); }} />
        </Banner>
      )}
      {warns.length > 0 && (
        <Banner color="#ff9f0a33">
          <Text style={{ fontSize: 12, color: th.text, flex: 1 }}>{`⚠ ${t('mail.links.warning', { count: warns.length })}`}</Text>
        </Banner>
      )}
      {msg.invite ? <Invite msg={msg} /> : null}
      <Attachments msg={msg} />
      {msg.html ? <HtmlView doc={msg.html} msg={{ acct: msg.acct, folder: msg.folder, uid: msg.uid }} remote={showRemote} width={width} onLink={onLink} />
        : <TextView text={msg.text} onLink={onLink} width={width} />}
      {msg.unsubscribe ? <Text style={{ fontSize: 11, color: th.textDim }}>{t('mail.unsubscribeHint')}</Text> : null}
    </View>
  );
}

// ---- the window ----

export default function Mail() {
  const { args } = useAppWindow();
  trackMailStatus();                      // only in its own process (lord.mail.appMode)
  if (args?.compose) return <Compose draft={typeof args.compose === 'object' ? args.compose : {}} />;
  return <MailMain />;
}

// The Dock's quick action: New Message (a compose window; sdk/dock.js).
registerDockActions('mail', () => [{ id: 'compose', label: 'mail.compose.newMessage', args: { compose: true }, new: true }]);

function compose(draft) { wm.open('mail', { new: true, args: { compose: draft || {} } }); }

function replyDraft(msg, mode) {
  const acct = msg.acct;
  const me = (mailAccounts.get(acct)?.email || '').toLowerCase();
  const from = msg.replyTo?.length ? msg.replyTo : msg.from || [];
  const subj = msg.subject || '';
  const quoteHead = t('mail.quoteHead', { date: fullDate(msg.date), name: msg.from?.[0]?.name || msg.from?.[0]?.email || '' });
  const body = `\n\n${quoteHead}\n${quote(msg.text || '')}`;
  if (mode === 'forward') {
    const head = [`---------- ${t('mail.forwarded')} ----------`, `${t('mail.from')}: ${(msg.from || []).map((a) => (a.name ? `${a.name} <${a.email}>` : a.email)).join(', ')}`,
      `${t('mail.date')}: ${fullDate(msg.date)}`, `${t('mail.subject')}: ${subj}`, `${t('mail.to')}: ${(msg.to || []).map((a) => a.email).join(', ')}`].join('\n');
    return { acct, subject: /^(fwd?|rv|tr):/i.test(subj) ? subj : `Fwd: ${subj}`, body: `\n\n${head}\n\n${msg.text || ''}`, original: { acct, folder: msg.folder, uid: msg.uid, forward: true }, forwardAtts: msg };
  }
  const to = from.filter((a) => a.email.toLowerCase() !== me);
  const cc = mode === 'all' ? [...(msg.to || []), ...(msg.cc || [])].filter((a) => a.email.toLowerCase() !== me && !to.some((x) => x.email === a.email)) : [];
  const refs = [msg.refs, msg.msgid].filter(Boolean).join(' ').trim();
  return { acct, to: to.length ? to : from, cc, subject: /^re:/i.test(subj) ? subj : `Re: ${subj}`, body, inReplyTo: msg.msgid, references: refs,
    original: { acct, folder: msg.folder, uid: msg.uid } };
}

function MailMain() {
  const th = useTheme();
  const { id: wid, args, focused } = useAppWindow();
  const st = useStore(mailStatus);
  const [folders, setFolders] = useState([]);
  const [src, setSrc] = useState({ kind: 'unified' });
  const [listRev, setListRev] = useState(0);
  const [sel, setSel] = useState(null);                 // list item
  const [msg, setMsg] = useState(null);
  const [thread, setThread] = useState([]);
  const [expanded, setExpanded] = useState({});
  const [loadErr, setLoadErr] = useState('');
  const [q, setQ] = useState('');
  const [sheet, setSheet] = useState(null);             // 'setup' | 'accounts' | { cert } | { password } | { link } | { menu }
  const [cfg, setCfg] = useState(mailConfig.get());
  const readingRef = useRef(null);
  const { w: readW } = useSize(readingRef, 400);
  const rootRef = useRef(null);
  const { w: rootW } = useSize(rootRef, 400);
  const compact = rootW > 100 && rootW < 1000;      // narrower panes in small windows
  const sideW = compact ? 200 : 220, listW = compact ? 290 : 330;
  const accounts = cfg.accounts.filter((a) => a.kind === 'mail');

  useEffect(() => { setLoadErr(''); }, [JSON.stringify(src)]);
  const loadFolders = () => mailRequest('folders').then((r) => setFolders(r.accounts || [])).catch(() => {});
  useEffect(() => {
    loadFolders();
    let timer = null;
    const off = onMailEvent((ev) => {
      if (ev.ev === 'changed' || ev.ev === 'folders' || ev.ev === 'status' || ev.ev === 'ready') {
        if (!timer) timer = setTimeout(() => { timer = null; loadFolders(); setListRev((v) => v + 1); }, 400);
      }
    });
    return () => { off(); if (timer) clearTimeout(timer); };
  }, []);
  useEffect(() => mailStatus.subscribe(() => setCfg(mailConfig.get())), []);

  // opened from a notification or another app
  useEffect(() => {
    if (args?.acct && args?.folder) {
      setSrc({ kind: 'folder', acct: args.acct, folder: args.folder });
      if (args.uid) openItem({ acct: args.acct, folder: args.folder, uid: args.uid });
    }
  }, [args?.nonce]);

  const openItem = async (it) => {
    setSel(it);
    setLoadErr('');
    setExpanded({});
    try {
      const m = await mailRequest('open', { acct: it.acct, folder: it.folder, uid: it.uid });
      setMsg(m);
      if (it.n > 1 && it.tk) {
        const r = await mailRequest('thread', { acct: it.acct, tk: it.tk }).catch(() => ({ items: [] }));
        setThread(r.items || []);
      } else setThread([]);
      // marked as read after a moment, like other mail apps
      if (!m.seen) setTimeout(() => mailRequest('flag', { acct: it.acct, folder: it.folder, uids: [it.uid], add: ['seen'] }).catch(() => {}), 1200);
    } catch (e) { setMsg(null); setLoadErr(e.code === 'offline' ? t('mail.offlineNotCached') : e.code === 'too-big' ? t('mail.openTooBig')
      : e.code === 'bad-json' || e.code === 'memory' ? t('mail.openFailed', { error: e.code }) : e.message); }
  };

  const selKey = sel ? `${sel.acct}|${sel.folder}|${sel.uid}` : null;
  const targets = () => {
    if (!sel) return null;
    const list = sel.thread?.length ? sel.thread : [{ acct: sel.acct, folder: sel.folder, uid: sel.uid }];
    return { acct: sel.acct, folder: sel.folder, uids: list.filter((x) => x.folder === sel.folder && x.acct === sel.acct).map((x) => x.uid) };
  };
  const specialOf = (acct, sp) => folders.find((a) => a.id === acct)?.folders.find((f) => f.special === sp)?.name;
  const act = {
    reply: () => msg && compose(replyDraft(msg, 'reply')),
    replyAll: () => msg && compose(replyDraft(msg, 'all')),
    forward: async () => {
      if (!msg) return;
      const d = replyDraft(msg, 'forward');
      // the original's attachments go along (saved from the cache)
      const paths = [];
      for (const a of msg.atts || []) {
        try {
          const r = await mailRequest('attachment', { acct: msg.acct, folder: msg.folder, uid: msg.uid, idx: a.idx });
          if (isolatedPim()) {
            const fd = lord.mail.attachmentFd(r.path);
            if (fd >= 0) paths.push({ fd, name: safeName(a.name), size: a.size || 0 });
          } else paths.push(`${lord.mail.cacheDir()}/${r.path}`);
        } catch (e) { /* skip */ }
      }
      delete d.forwardAtts;
      compose({ ...d, attachments: paths, noSignature: false });
    },
    archive: async () => {
      const tg = targets(); if (!tg) return;
      let to = specialOf(tg.acct, 'archive');
      if (!to) { await mailRequest('create-folder', { acct: tg.acct, name: 'Archive' }).catch(() => {}); to = 'Archive'; }
      if (to === tg.folder) return;
      await mailRequest('move', { ...tg, to }).catch(() => {});
      setSel(null); setMsg(null); setListRev((v) => v + 1);
    },
    remove: async () => {
      const tg = targets(); if (!tg) return;
      const next = MessageList.neighbour?.(selKey, 1);
      await mailRequest('delete', tg).catch(() => {});
      setListRev((v) => v + 1);
      if (next) openItem(next.it); else { setSel(null); setMsg(null); }
    },
    flag: async () => {
      const tg = targets(); if (!tg) return;
      const on = !(sel.anyFlag || sel.flagged);
      await mailRequest('flag', { ...tg, ...(on ? { add: ['flagged'] } : { remove: ['flagged'] }) }).catch(() => {});
      setSel({ ...sel, anyFlag: on, flagged: on });
    },
    unread: async () => {
      const tg = targets(); if (!tg) return;
      const unread = sel.unreadN !== undefined ? sel.unreadN > 0 : !sel.seen;
      await mailRequest('flag', { ...tg, ...(unread ? { add: ['seen'] } : { remove: ['seen'] }) }).catch(() => {});
    },
    label: async (l) => {
      const tg = targets(); if (!tg) return;
      const has = (sel.labels || []).includes(l);
      await mailRequest('flag', { ...tg, ...(has ? { remove: [`label${l}`] } : { add: [`label${l}`] }) }).catch(() => {});
      setSel({ ...sel, labels: has ? sel.labels.filter((x) => x !== l) : [...(sel.labels || []), l] });
    },
    junk: async () => {
      const tg = targets(); if (!tg) return;
      const to = specialOf(tg.acct, 'junk');
      if (to && to !== tg.folder) { await mailRequest('move', { ...tg, to }).catch(() => {}); setSel(null); setMsg(null); setListRev((v) => v + 1); }
    },
  };

  const onLink = (link) => {
    if (!link?.h) return;
    if (/^mailto:/i.test(link.h)) {
      const [addr, qs] = link.h.slice(7).split('?');
      const subj = /(?:^|&)subject=([^&]*)/i.exec(qs || '');
      compose({ to: [{ name: '', email: decodeURIComponent(addr) }], subject: subj ? decodeURIComponent(subj[1]) : '' });
      return;
    }
    setSheet({ link });
  };

  const trustSender = (email) => {
    if (!email) return;
    const c = mailConfig.get();
    mailConfig.save({ ...c, trustedSenders: [...new Set([...(c.trustedSenders || []), email])] });
    setCfg(mailConfig.get());
  };

  useKeyboard((e) => {
    if (!focused) return false;
    const m = e.ctrl || e.meta;
    const b = (e.base || '').toLowerCase();
    if (m && b === 'n') { compose(); return true; }
    if (m && b === 'r' && !e.shift) { act.reply(); return true; }
    if (m && b === 'r' && e.shift) { act.replyAll(); return true; }
    if (m && b === 'j') { act.forward(); return true; }
    if (m && b === 'e') { act.archive(); return true; }
    if (!m && (e.key === 'Delete' || (e.key === 'Backspace' && e.meta))) { act.remove(); return true; }
    if (!m && (e.key === 'ArrowDown' || e.key === 'ArrowUp') && selKey) { const n = MessageList.neighbour?.(selKey, e.key === 'ArrowDown' ? 1 : -1); if (n) openItem(n.it); return true; }
    return false;
  });

  useEffect(() => {
    wm.setMenus(wid, [
      { label: t('mail.menu.file'), items: [
        { label: t('mail.new'), hint: '❖N', onClick: () => compose() },
        { label: t('mail.menu.addAccount'), onClick: () => setSheet('setup') },
        { label: t('mail.menu.accounts'), onClick: () => setSheet('accounts') }, '-',
        { label: t('mail.menu.sync'), onClick: () => mailRequest('sync', {}).catch(() => {}) },
      ] },
      { label: t('mail.menu.message'), items: [
        { label: t('mail.reply'), hint: '❖R', onClick: act.reply, disabled: !msg },
        { label: t('mail.replyAll'), hint: '❖⇧R', onClick: act.replyAll, disabled: !msg },
        { label: t('mail.forward'), hint: '❖J', onClick: act.forward, disabled: !msg }, '-',
        { label: t('mail.archive'), hint: '❖E', onClick: act.archive, disabled: !sel },
        { label: t('mail.delete'), hint: '⌦', onClick: act.remove, disabled: !sel },
        { label: t('mail.junk'), onClick: act.junk, disabled: !sel },
        { label: t('mail.markUnread'), onClick: act.unread, disabled: !sel },
        { label: t('mail.flag'), onClick: act.flag, disabled: !sel },
      ] },
    ]);
  });

  // ---- sidebar ----
  const sidebar = (
    <View style={{ width: sideW, backgroundColor: th.sidebarBg }}>
      <Hairline edge="end" />
      <ScrollView style={{ flex: 1 }}>
        <View style={{ paddingHorizontal: 10, paddingTop: 10, paddingBottom: 10, gap: 1 }}>
          <Caption first>{t('mail.favorites')}</Caption>
          <SideRow icon={{ kind: 'tray' }} label={t('mail.unifiedInboxShort')} count={st.unread} selected={src.kind === 'unified' && !src.filter && !src.label} onClick={() => { setSrc({ kind: 'unified' }); setQ(''); }} />
          <SideRow icon="●" label={t('mail.unread')} selected={src.kind === 'unified' && src.filter === 'unread'} onClick={() => setSrc({ kind: 'unified', filter: 'unread' })} />
          <SideRow icon="⚑" label={t('mail.flagged')} selected={src.kind === 'unified' && src.filter === 'flagged'} onClick={() => setSrc({ kind: 'unified', filter: 'flagged' })} />
          {folders.map((a) => {
            const status = st.accounts[a.id]?.status || a.status;
            const bad = st.authFailed[a.id] || status === 'auth' || status === 'cert' || status === 'offline';
            return (
              <View key={a.id} style={{ gap: 1 }}>
                <View style={{ flexDirection: 'row', alignItems: 'center', marginHorizontal: 8, marginTop: 16, marginBottom: 4, gap: 6 }}>
                  <Text style={{ flex: 1, fontSize: 11, fontWeight: 600, color: th.textDim, whiteSpace: 'nowrap', overflow: 'hidden' }}>{a.name}</Text>
                  {status === 'sync' || status === 'connecting' ? <Text style={{ fontSize: 10, color: th.textDim }}>⟳</Text> : null}
                  {bad ? <Text onClick={() => setSheet(st.authFailed[a.id] || status === 'auth' ? { password: a.id } : st.certs.find((c) => c.acct === a.id) ? { cert: st.certs.find((c) => c.acct === a.id) } : 'accounts')}
                    style={{ fontSize: 11, color: '#ff9f0a' }}>⚠</Text> : null}
                </View>
                {a.folders.filter((f) => !f.noselect).map((f) => (
                  <SideRow key={f.name} icon={{ folder: f }} label={folderName(f)} count={f.special === 'drafts' ? f.total : f.special === 'sent' || f.special === 'trash' ? 0 : f.unread}
                    indent={f.delim && f.name.split(f.delim).length > 1 && f.special === '' ? 1 : 0}
                    selected={src.kind === 'folder' && src.acct === a.id && src.folder === f.name} onClick={() => { setSrc({ kind: 'folder', acct: a.id, folder: f.name }); setQ(''); }} />
                ))}
              </View>
            );
          })}
          {cfg.accounts.filter((a) => a.kind === 'dav').map((a) => {
            const s2 = st.accounts[a.id]?.status;
            const bad = st.authFailed[a.id] || s2 === 'auth' || s2 === 'cert' || s2 === 'offline';
            return (
              <View key={a.id} style={{ flexDirection: 'row', alignItems: 'center', gap: 6, marginHorizontal: 8, marginTop: 16, marginBottom: 4 }}>
                <Glyph kind="calendar" size={12} color={th.textDim} />
                <Text style={{ flex: 1, fontSize: 11, fontWeight: 600, color: th.textDim, whiteSpace: 'nowrap', overflow: 'hidden' }}>{a.name}</Text>
                {s2 === 'sync' ? <Text style={{ fontSize: 10, color: th.textDim }}>⟳</Text> : null}
                {bad ? <Text onClick={() => setSheet(st.certs.find((c) => c.acct === a.id) ? { cert: st.certs.find((c) => c.acct === a.id) } : { password: a.id })} style={{ fontSize: 11, color: '#ff9f0a' }}>⚠</Text> : null}
              </View>
            );
          })}
          <Caption>{t('mail.categories')}</Caption>
          {CATEGORY_COLORS.map((c, i) => (
            <SideRow key={c} icon={<View style={{ width: 10, height: 10, borderRadius: 5, backgroundColor: c }} />} label={categoryName(i + 1, cfg)}
              selected={src.kind === 'unified' && src.label === i + 1} onClick={() => setSrc({ kind: 'unified', label: i + 1 })} />
          ))}
        </View>
      </ScrollView>
      <View style={{ height: 40, flexDirection: 'row', alignItems: 'center', gap: 4, paddingHorizontal: 8 }}>
        <Hairline edge="top" />
        <Hairline edge="end" />
        <Button small plain icon="⊕" label={t('mail.addAccount')} onClick={() => setSheet('setup')} />
        <View style={{ flex: 1 }} />
        <ToolbarButton small icon="⚙" title={t('mail.menu.accounts')} onClick={() => setSheet('accounts')} />
      </View>
    </View>
  );

  // toolbar: icon-only items with tooltips, as in Mail; the ones that need a message are dimmed
  const glyphColor = () => th.text;            // ToolbarButton dims disabled items itself
  const toolbar = (
    <Toolbar>
      <ToolbarButton icon="✎" title={t('mail.new')} onClick={() => compose()} />
      <ToolbarDivider />
      <ToolbarButton icon="↩" title={t('mail.reply')} disabled={!msg} onClick={act.reply} />
      <ToolbarButton icon="⇇" title={t('mail.replyAll')} disabled={!msg} onClick={act.replyAll} />
      <ToolbarButton icon="↪" title={t('mail.forward')} disabled={!msg} onClick={act.forward} />
      <ToolbarDivider />
      <ToolbarButton icon={<Glyph kind="archive" size={16} color={glyphColor(sel)} />} title={t('mail.archive')} disabled={!sel} onClick={act.archive} />
      <ToolbarButton icon={<Glyph kind="trash" size={16} color={glyphColor(sel)} />} title={t('mail.delete')} disabled={!sel} onClick={act.remove} />
      <ToolbarButton icon="⚑" title={t('mail.flag')} disabled={!sel} onClick={act.flag} />
      <ToolbarButton icon={<CategoryDots />} title={t('mail.category')} disabled={!sel} onClick={() => setSheet({ menu: 'labels' })} />
      <ToolbarSpacer />
    </Toolbar>
  );
  const search = (
    <View style={{ paddingHorizontal: 12, paddingBottom: 10 }}>
      <SearchField value={q} onChange={(v) => { setQ(v); if (v.trim().length >= 2) setSrc({ kind: 'search', q: v.trim() }); else if (!v.trim() && src.kind === 'search') setSrc({ kind: 'unified' }); }}
        placeholder={t('mail.search.placeholder')} onCancel={() => { setQ(''); setSrc({ kind: 'unified' }); }} />
    </View>
  );

  const listTitle = src.kind === 'search' ? t('mail.search.results', { q: src.q }) : src.kind === 'unified'
    ? (src.label ? categoryName(src.label, cfg) : src.filter === 'unread' ? t('mail.unread') : src.filter === 'flagged' ? t('mail.flagged') : t('mail.unifiedInbox'))
    : folderName(folders.find((a) => a.id === src.acct)?.folders.find((f) => f.name === src.folder) || { display: src.folder || '' });

  const noAccounts = !accounts.length;
  return (
    <View ref={rootRef} style={{ flex: 1, flexDirection: 'row', backgroundColor: th.windowBg }}>
      {sidebar}
      <View style={{ flex: 1 }}>
        {toolbar}
        <View style={{ flex: 1, flexDirection: 'row' }}>
          <View style={{ width: listW }}>
            <Hairline edge="end" />
            <View style={{ flexDirection: 'row', alignItems: 'center', paddingHorizontal: 14, paddingTop: 10, height: 40 }}>
              <Text style={{ flex: 1, fontSize: 15, fontWeight: 600, color: th.text, whiteSpace: 'nowrap', overflow: 'hidden' }}>{listTitle}</Text>
            </View>
            {search}
            <View style={{ height: 1, backgroundColor: th.separator }} />
            {!mailAvailable() ? <Text style={{ margin: 16, fontSize: 13, color: th.textDim }}>{t('mail.unavailable')}</Text>
              : noAccounts ? (
                <View style={{ margin: 18, gap: 10 }}>
                  <Text style={{ fontSize: 13, color: th.textDim }}>{t('mail.noAccounts')}</Text>
                  <Button primary label={t('mail.addAccount')} onClick={() => setSheet('setup')} />
                </View>
              ) : <MessageList src={src} rev={listRev} selected={selKey} onSelect={openItem} onMenu={() => setSheet({ menu: 'labels' })} cfg={cfg} />}
          </View>
          <View ref={readingRef} style={{ flex: 1 }}>
            {msg ? (
              <ScrollView key={`${msg.acct}|${msg.folder}|${msg.uid}`} style={{ flex: 1 }}>
                <View style={{ paddingHorizontal: 20, paddingTop: 16, paddingBottom: 24, gap: 12 }}>
                  <Text style={{ fontSize: 20, fontWeight: 600, color: th.text, lineHeight: 26, direction: textDirection(msg.subject || '') }}>{msg.subject || t('mail.noSubject')}</Text>
                  {(sel?.labels || []).length ? (
                    <View style={{ flexDirection: 'row', gap: 6 }}>
                      {sel.labels.map((l) => <View key={l} style={{ paddingHorizontal: 8, height: 18, borderRadius: 9, justifyContent: 'center', backgroundColor: `${CATEGORY_COLORS[l - 1]}44` }}><Text style={{ fontSize: 11, color: th.text }}>{categoryName(l, cfg)}</Text></View>)}
                    </View>
                  ) : null}
                  {thread.length > 1 ? thread.filter((x) => !(x.folder === msg.folder && x.uid === msg.uid)).map((x) => (
                    <ThreadItem key={`${x.folder}|${x.uid}`} it={x} expanded={expanded[`${x.folder}|${x.uid}`]} onToggle={() => setExpanded((e) => ({ ...e, [`${x.folder}|${x.uid}`]: !e[`${x.folder}|${x.uid}`] }))}
                      width={readW - 40} onLink={onLink} cfg={cfg} onTrustSender={trustSender} />
                  )) : null}
                  <MessageView msg={msg} width={readW - 40} onLink={onLink} cfg={cfg} onTrustSender={trustSender} />
                </View>
              </ScrollView>
            ) : (
              <View style={{ flex: 1, alignItems: 'center', justifyContent: 'center', gap: 8, padding: 24 }}>
                <Text style={{ fontSize: 40, color: th.textDim, opacity: 0.6 }}>✉</Text>
                <Text style={{ fontSize: 13, color: th.textDim, textAlign: 'center' }}>{loadErr || t('mail.noSelection')}</Text>
              </View>
            )}
          </View>
        </View>
      </View>
      {sheet === 'setup' && <AccountSetup onClose={() => { setSheet(null); setCfg(mailConfig.get()); loadFolders(); }} />}
      {sheet === 'accounts' && <AccountsPanel onClose={() => { setSheet(null); setCfg(mailConfig.get()); loadFolders(); }} onAdd={() => setSheet('setup')} onPassword={(id) => setSheet({ password: id })} />}
      {typeof sheet === 'object' && sheet?.cert && <CertDialog cert={sheet.cert} onClose={() => setSheet(null)} />}
      {!sheet && st.certs[0] && <CertDialog cert={st.certs[0]} onClose={() => mailStatus.set((s) => ({ ...s, certs: s.certs.slice(1) }))} />}
      {typeof sheet === 'object' && sheet?.password && <PasswordPrompt acct={sheet.password} onClose={() => setSheet(null)} />}
      {typeof sheet === 'object' && sheet?.link && <LinkSheet link={sheet.link} onClose={() => setSheet(null)} />}
      {typeof sheet === 'object' && sheet?.menu === 'labels' && (
        <Sheet width={280}>
          <Text style={{ fontSize: 14, fontWeight: 700, color: th.text }}>{t('mail.category')}</Text>
          {CATEGORY_COLORS.map((c, i) => (
            <View key={c} onClick={() => { act.label(i + 1); setSheet(null); }} style={{ flexDirection: 'row', alignItems: 'center', gap: 8, padding: 6, borderRadius: 6 }}>
              <View style={{ width: 12, height: 12, borderRadius: 6, backgroundColor: c }} />
              <Text style={{ flex: 1, fontSize: 13, color: th.text }}>{categoryName(i + 1, cfg)}</Text>
              {(sel?.labels || []).includes(i + 1) ? <Text style={{ color: th.accent }}>✓</Text> : null}
            </View>
          ))}
          <View style={{ flexDirection: 'row', justifyContent: 'flex-end' }}><Button label={t('mail.close')} onClick={() => setSheet(null)} /></View>
        </Sheet>
      )}
    </View>
  );
}

function Caption({ children, first }) {
  const th = useTheme();
  return <Text style={{ fontSize: 11, fontWeight: 600, color: th.textDim, marginHorizontal: 8, marginTop: first ? 4 : 16, marginBottom: 4 }}>{children}</Text>;
}

// Three small dots in the category colors: the toolbar item for categories.
function CategoryDots() {
  return (
    <View style={{ width: 16, height: 16, alignItems: 'center', justifyContent: 'center', gap: 1 }}>
      <View style={{ width: 6, height: 6, borderRadius: 3, backgroundColor: CATEGORY_COLORS[0] }} />
      <View style={{ flexDirection: 'row', gap: 1 }}>
        <View style={{ width: 6, height: 6, borderRadius: 3, backgroundColor: CATEGORY_COLORS[3] }} />
        <View style={{ width: 6, height: 6, borderRadius: 3, backgroundColor: CATEGORY_COLORS[4] }} />
      </View>
    </View>
  );
}

// Source-list row. icon: a glyph (drawn in the accent color), { kind } for a drawn shape,
// { folder } for a mail folder, or an element.
function SideRow({ icon, label, count, selected, onClick, indent = 0 }) {
  const th = useTheme();
  const [hover, setHover] = useState(false);
  const c = selected ? '#fff' : th.accent;
  let ic;
  if (typeof icon === 'string') ic = <Text style={{ fontSize: icon === '●' ? 10 : 13, color: c, textAlign: 'center' }}>{icon}</Text>;
  else if (icon && icon.kind) ic = <Glyph kind={icon.kind} size={16} color={c} />;
  else if (icon && icon.folder) ic = <FolderIcon f={icon.folder} color={c} />;
  else ic = icon;
  const rtl = direction() === 'rtl';
  return (
    <View onClick={onClick} onMouseEnter={() => setHover(true)} onMouseLeave={() => setHover(false)}
      style={{ flexDirection: 'row', alignItems: 'center', gap: 8, height: 28, [rtl ? 'paddingRight' : 'paddingLeft']: 8 + indent * 14, [rtl ? 'paddingLeft' : 'paddingRight']: 8, borderRadius: 6,
        backgroundColor: selected ? th.accent : hover ? th.hover : 'transparent' }}>
      <View style={{ width: 18, alignItems: 'center', justifyContent: 'center' }}>{ic}</View>
      <Text style={{ flex: 1, fontSize: 13, color: selected ? '#fff' : th.text, whiteSpace: 'nowrap', overflow: 'hidden' }}>{label}</Text>
      {count ? <Text style={{ fontSize: 11, fontWeight: 500, color: selected ? 'rgba(255,255,255,0.85)' : th.textDim }}>{String(count)}</Text> : null}
    </View>
  );
}

function ThreadItem({ it, expanded, onToggle, width, onLink, cfg, onTrustSender }) {
  const th = useTheme();
  const [m, setM] = useState(null);
  useEffect(() => { if (expanded && !m) mailRequest('open', { acct: it.acct, folder: it.folder, uid: it.uid }).then(setM).catch(() => {}); }, [expanded]);
  if (expanded && m) return <View style={{ gap: 6 }}><Text onClick={onToggle} style={{ fontSize: 11, color: th.accent }}>{t('mail.thread.collapse')}</Text><MessageView msg={m} width={width} onLink={onLink} cfg={cfg} onTrustSender={onTrustSender} /></View>;
  return (
    <View onClick={onToggle} style={{ paddingVertical: 9, paddingHorizontal: 12, borderRadius: 8, backgroundColor: th.control, flexDirection: 'row', alignItems: 'center', gap: 10 }}>
      <Text style={{ fontSize: 13, fontWeight: 600, color: th.text, whiteSpace: 'nowrap' }}>{it.from || it.email}</Text>
      <Text style={{ flex: 1, fontSize: 12, color: th.textDim, whiteSpace: 'nowrap', overflow: 'hidden', direction: textDirection(it.preview || '') }}>{it.preview}</Text>
      <Text style={{ fontSize: 11, color: th.textDim }}>{listDate(it.date)}</Text>
    </View>
  );
}

// A link: there is no web engine in LordOS, so the address is shown and can be copied. A link
// whose visible text names another site is called out.
function LinkSheet({ link, onClose }) {
  const th = useTheme();
  let host = '';
  try { host = link.h.replace(/^[a-z]+:\/\//i, '').split(/[/?#]/)[0]; } catch (e) { host = ''; }
  return (
    <Sheet width={500}>
      <Text style={{ fontSize: 16, fontWeight: 700, color: link.w ? '#ff453a' : th.text }}>{link.w ? t('mail.links.deceptiveTitle') : t('mail.links.title')}</Text>
      {link.w ? <Text style={{ fontSize: 13, color: th.text }}>{t('mail.links.deceptiveBody', { shown: (link.txt || '').trim(), host })}</Text> : null}
      <View style={{ padding: 10, borderRadius: 8, backgroundColor: th.control }}><Text style={{ fontSize: 12, color: th.text, fontFamily: 'monospace' }}>{link.h}</Text></View>
      <Text style={{ fontSize: 12, color: th.textDim }}>{t('mail.links.noBrowser')}</Text>
      <View style={{ flexDirection: 'row', justifyContent: 'flex-end', gap: 8 }}>
        <Button label={t('mail.close')} onClick={onClose} />
        <Button primary={!link.w} label={t('mail.links.copy')} onClick={() => { clipboard.set(link.h); onClose(); }} />
      </View>
    </Sheet>
  );
}

