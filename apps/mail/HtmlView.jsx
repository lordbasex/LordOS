// HtmlView — draws the safe tree that axia-maild's sanitizer made from a message's HTML
// (server/src/mail/html.h) with our own views: blocks, layout tables, text runs, images and
// links. Nothing here ever interprets HTML or runs anything; images are decoded in axia-maild
// and remote ones are requested only after the user chose to load them.
import { View, Text, Image, useTheme, mailRequest } from '@lordos/sdk';
import { useEffect, useRef, useState } from 'react';

const MAX_WORDS = 6000;           // beyond this a paragraph keeps whole runs (wrapping is coarser)

function rgba(hex, fallback) {
  if (!hex || hex[0] !== '#') return fallback;
  if (hex.length === 9) {
    const a = parseInt(hex.slice(7), 16) / 255;
    const n = parseInt(hex.slice(1, 7), 16);
    return `rgba(${n >> 16},${(n >> 8) & 255},${n & 255},${a.toFixed(2)})`;
  }
  return hex;
}

// ---- images ----

function MailImage({ node, ctx }) {
  const ref = useRef(null);
  const [img, setImg] = useState(null);         // { image, w, h } | { error }
  const remote = node.src.startsWith('r:');
  const allowed = !remote || ctx.remote;
  const maxw = Math.max(16, Math.round(Math.min(ctx.w, node.w || ctx.w)));
  useEffect(() => {
    if (!allowed) return undefined;
    let live = true;
    mailRequest('image', { ...ctx.msg, src: node.src, maxw: Math.min(1600, maxw * 2) })
      .then((r) => { if (live) setImg(r); }).catch((e) => { if (live) setImg({ error: e.code || 'error' }); });
    return () => { live = false; };
  }, [node.src, allowed, ctx.msg.uid]);
  useEffect(() => {
    if (ref.current && img?.image) { lord.mail.bind(ref.current.id, img.image); lord.mail.release(img.image); }
  }, [img?.image]);
  // size: the HTML's width/height, or the picture's own, scaled to fit
  let w = node.wp ? (ctx.w * node.wp) / 100 : node.w || (img?.w ? img.w / 2 : Math.min(ctx.w, 200));
  let h = node.h || (img?.h && img?.w ? (img.h * w) / img.w : node.w ? node.w * 0.5 : 60);
  if (!node.h && node.w && img?.w) h = (img.h * node.w) / img.w;
  if (w > ctx.w) { h = (h * ctx.w) / w; w = ctx.w; }
  const box = { width: Math.max(1, w), height: Math.max(1, h) };
  const click = node.l >= 0 && node.l !== undefined ? () => ctx.onLink(ctx.links[node.l]) : undefined;
  if (img?.image) return <Image ref={ref} onClick={click} style={{ ...box, objectFit: 'contain' }} />;
  return (
    <View onClick={click} style={{ ...box, borderRadius: 4, borderWidth: 1, borderColor: ctx.dim, backgroundColor: 'rgba(127,127,127,0.10)',
      alignItems: 'center', justifyContent: 'center', overflow: 'hidden' }}>
      {h > 18 && <Text style={{ fontSize: 13, color: ctx.dim }}>{allowed && !img?.error ? '⋯' : '🖼'}</Text>}
      {h > 36 && node.alt ? <Text style={{ fontSize: 11, color: ctx.dim, textAlign: 'center', paddingHorizontal: 4 }}>{node.alt}</Text> : null}
    </View>
  );
}

// ---- text ----

function runStyle(s, ctx, link) {
  const st = { fontSize: s?.fs || ctx.fs, color: link ? (link.w ? '#e5383b' : ctx.link) : rgba(s?.c, ctx.fg) };
  if (s?.b) st.fontWeight = 700;
  if (s?.i && !s?.c && !link) st.color = ctx.italic;          // the engine has no italic face: a tint, like Notes
  if (s?.mono) st.fontFamily = 'monospace';
  if (s?.bg) st.backgroundColor = rgba(s.bg, undefined);
  return st;
}

function Word({ text, s, ctx, link }) {
  const st = runStyle(s, ctx, link);
  const click = link ? () => ctx.onLink(link) : undefined;
  const underline = s?.u || link, strike = s?.st;
  if (!underline && !strike) return <Text onClick={click} style={st}>{text}</Text>;
  return (
    <View onClick={click} style={{ flexDirection: 'column' }}>
      <Text style={st}>{text}</Text>
      {underline ? <View style={{ height: 1, marginTop: -2, backgroundColor: st.color, opacity: link ? 0.6 : 1 }} /> : null}
      {strike ? <View style={{ position: 'absolute', left: 0, right: 0, top: (st.fontSize * 1.3) / 2, height: 1, backgroundColor: st.color }} /> : null}
    </View>
  );
}

// A paragraph: inline runs in a wrapping row (one <Text> per word, as the engine has no styled
// spans inside one Text); a single plain run stays one Text that wraps by itself.
function Para({ items, ctx, align }) {
  const justify = align === 'c' ? 'center' : align === 'e' ? 'flex-end' : 'flex-start';
  const textAlign = align === 'c' ? 'center' : align === 'e' ? 'end' : 'start';
  if (items.length === 1 && items[0].t === 'x' && !(items[0].l >= 0) && !items[0].s?.u && !items[0].s?.st) {
    const n = items[0];
    return <Text style={{ ...runStyle(n.s, ctx, null), textAlign, lineHeight: (n.s?.fs || ctx.fs) * 1.4 }}>{n.x}</Text>;
  }
  const out = [];
  let words = 0;
  items.forEach((n, i) => {
    if (n.t === 'br') { out.push(<View key={`b${i}`} style={{ width: '100%', height: 0 }} />); return; }
    if (n.t === 'img') { out.push(<MailImage key={`i${i}`} node={n} ctx={ctx} />); return; }
    const link = n.l >= 0 ? ctx.links[n.l] : null;
    const parts = words > MAX_WORDS ? [n.x] : n.x.split(/(?<= )/);
    if (link?.w && (i === 0 || items[i - 1].l !== n.l)) out.push(<Text key={`w${i}`} style={{ fontSize: n.s?.fs || ctx.fs, color: '#e5383b' }}>⚠ </Text>);
    words += parts.length;
    parts.forEach((p, k) => out.push(<Word key={`${i}.${k}`} text={p} s={n.s} ctx={ctx} link={link} />));
  });
  return <View style={{ flexDirection: 'row', flexWrap: 'wrap', alignItems: 'flex-end', justifyContent: justify }}>{out}</View>;
}

// ---- blocks ----

const pad = (s) => (s?.p ? { paddingTop: s.p[0], paddingRight: s.p[1], paddingBottom: s.p[2], paddingLeft: s.p[3] } : {});
const padX = (s) => (s?.p ? s.p[1] + s.p[3] : 0);

function blockWidth(s, avail) {
  let w = s?.wp ? (avail * s.wp) / 100 : s?.w || null;
  if (s?.mw && (!w || w > s.mw)) w = s.mw;
  if (w && w > avail) w = avail;
  return w;
}

function Children({ nodes, ctx, align }) {
  const out = [];
  let inline = [];
  const flush = (key) => { if (inline.length) { out.push(<Para key={`p${key}`} items={inline} ctx={ctx} align={align} />); inline = []; } };
  (nodes || []).forEach((n, i) => {
    if (n.t === 'x' || n.t === 'br' || (n.t === 'img' && !n.a)) { inline.push(n); return; }
    flush(i);
    out.push(<Node key={i} n={n} ctx={ctx} align={align} />);
  });
  flush('end');
  return out;
}

function Table({ n, ctx }) {
  const s = n.s || {};
  const w = blockWidth(s, ctx.w) || ctx.w;
  const cp = n.cp || 0;
  const border = s.bw ? { borderWidth: s.bw, borderColor: rgba(s.bc, ctx.dim) } : {};
  const rows = (n.c || []).filter((r) => r.t === 'tr' && r.c?.length);
  return (
    <View style={{ width: w, alignSelf: s.ac ? 'center' : 'flex-start', backgroundColor: rgba(s.bg, undefined), borderRadius: s.r || 0, overflow: s.r ? 'hidden' : 'visible',
      marginTop: s.mt || 0, marginBottom: s.mb || 0, ...border }}>
      {rows.map((r, ri) => {
        const cells = r.c.filter((c) => c.t === 'td');
        // fixed widths first, the rest share what is left
        let fixed = 0, flex = 0;
        const ws = cells.map((c) => { const cw = blockWidth(c.s, w); if (cw) fixed += cw; else flex += c.cs || 1; return cw; });
        const scale = fixed > w ? w / fixed : 1;
        const rest = Math.max(0, w - fixed * scale);
        return (
          <View key={ri} style={{ flexDirection: 'row', backgroundColor: rgba(r.s?.bg, undefined) }}>
            {cells.map((c, ci) => {
              const cw = ws[ci] ? ws[ci] * scale : flex ? (rest * (c.cs || 1)) / flex : 0;
              const cs = c.s || {};
              const inner = Math.max(10, cw - (cs.p ? padX(cs) : cp * 2));
              const va = cs.va === 'm' ? 'center' : cs.va === 'b' ? 'flex-end' : 'flex-start';
              return (
                <View key={ci} style={{ width: cw, backgroundColor: rgba(cs.bg, undefined), justifyContent: va, ...(cs.p ? pad(cs) : { paddingTop: cp, paddingBottom: cp, paddingLeft: cp, paddingRight: cp }),
                  ...(s.bw ? { borderWidth: 0.5, borderColor: rgba(s.bc, ctx.dim) } : {}) }}>
                  <Children nodes={c.c} ctx={{ ...ctx, w: inner }} align={cs.a} />
                </View>
              );
            })}
          </View>
        );
      })}
    </View>
  );
}

function Node({ n, ctx, align }) {
  if (n.t === 'hr') return <View style={{ height: 1, backgroundColor: ctx.dim, opacity: 0.4, marginTop: 8, marginBottom: 8 }} />;
  if (n.t === 'img') return <View style={{ alignItems: n.a === 'c' ? 'center' : n.a === 'e' ? 'flex-end' : 'flex-start' }}><MailImage node={n} ctx={ctx} /></View>;
  if (n.t === 'tbl') return <Table n={n} ctx={ctx} />;
  if (n.t === 'tr' || n.t === 'td') return <Children nodes={n.c} ctx={ctx} align={align} />;
  const s = n.s || {};
  const w = blockWidth(s, ctx.w);
  const inner = Math.max(10, (w || ctx.w) - padX(s) - (s.bl || 0) - (n.mk ? 18 : 0));
  const body = <Children nodes={n.c} ctx={{ ...ctx, w: inner }} align={s.a || align} />;
  const style = {
    ...pad(s), marginTop: s.mt || 0, marginBottom: s.mb || 0, backgroundColor: rgba(s.bg, undefined), borderRadius: s.r || 0,
    ...(w ? { width: w } : {}), ...(s.ac ? { alignSelf: 'center' } : {}),
    ...(s.bw ? { borderWidth: s.bw, borderColor: rgba(s.bc, ctx.dim) } : {}),
  };
  if (s.bl) {
    return (
      <View style={{ ...style, flexDirection: 'row' }}>
        <View style={{ width: s.bl, backgroundColor: rgba(s.blc, ctx.dim), marginRight: 10, borderRadius: 1 }} />
        <View style={{ flex: 1 }}>{body}</View>
      </View>
    );
  }
  if (n.mk) {
    return (
      <View style={{ ...style, flexDirection: 'row', gap: 6 }}>
        <Text style={{ fontSize: ctx.fs, color: ctx.fg, width: 14, textAlign: 'end' }}>{n.mk}</Text>
        <View style={{ flex: 1 }}>{body}</View>
      </View>
    );
  }
  return <View style={style}>{body}</View>;
}

// doc: the sanitizer's JSON; msg: { acct, folder, uid }; remote: load remote images
export function HtmlView({ doc, msg, remote, width: hint, onLink }) {
  const th = useTheme();
  // our own width (the pane can change size): measured, not guessed
  const box = useRef(null);
  const [width, setWidth] = useState(hint);
  useEffect(() => {
    const read = () => { if (box.current) { const w = lord.getLayout(box.current.id)[2]; if (w > 40) setWidth((o) => (Math.abs(o - w) > 2 ? w : o)); } };
    read();
    const id = setInterval(read, 300);
    return () => clearInterval(id);
  }, []);
  // the sender chose colors: show it on its own paper (like other mail apps do in dark mode)
  const paper = !!doc.colors || !!doc.bg || th.mode === 'light';
  const ctx = {
    msg, remote, onLink, links: doc.links || [], fs: 14,
    w: Math.max(80, width - 28),
    fg: paper ? '#1f1f1f' : th.text,
    dim: paper ? '#8a8a8a' : th.textDim,
    link: paper ? '#0a5fd0' : th.accent,
    italic: paper ? '#4b3b8f' : '#e9e3ff',
  };
  return (
    <View ref={box} style={{ backgroundColor: paper ? rgba(doc.bg, '#ffffff') : 'transparent', padding: 14, borderRadius: 10, direction: 'ltr' }}>
      <Children nodes={doc.blocks} ctx={ctx} />
    </View>
  );
}

// Plain text with clickable links and dimmed quoted lines.
export function TextView({ text, onLink, width }) {
  const th = useTheme();
  const lines = String(text || '').replace(/\r\n/g, '\n').split('\n');
  const url = /(https?:\/\/[^\s<>()]+[^\s<>().,;:!?]|mailto:[^\s<>()]+)/g;
  return (
    <View style={{ padding: 4, gap: 2, maxWidth: width }}>
      {lines.slice(0, 5000).map((ln, i) => {
        const quoted = /^\s*>/.test(ln);
        const color = quoted ? th.textDim : th.text;
        if (!url.test(ln)) return <Text key={i} style={{ fontSize: 14, color, minHeight: 18 }}>{ln || ' '}</Text>;
        url.lastIndex = 0;
        const parts = ln.split(url);
        return (
          <View key={i} style={{ flexDirection: 'row', flexWrap: 'wrap' }}>
            {parts.map((p, k) => (k % 2 ? <Text key={k} onClick={() => onLink({ h: p })} style={{ fontSize: 14, color: th.accent }}>{p}</Text>
              : p.split(/(?<= )/).map((w, j) => <Text key={`${k}.${j}`} style={{ fontSize: 14, color }}>{w}</Text>)))}
          </View>
        );
      })}
    </View>
  );
}
