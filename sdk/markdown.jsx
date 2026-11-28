// markdown.jsx — our own <Markdown source />: renders markdown with LordOS nodes.
//
//   <Markdown source={text} base="/home/lord/Notes/x" width={420}
//     onToggleTask={(line) => …} onLink={(url) => …} onHeadingRef={(id, node) => …} />
//   fitWidth (optional): width is the exact width of the column (lays list items out at it)
//   breaks (optional): a single line break inside a paragraph is kept as a line break, as typed (Notes, so the
//     reading view breaks where the editor does; GitHub #169). Without it, plain markdown: the lines are joined.
//   resolveImage (optional): (target) → the file to draw, or null to show [alt] instead. Without it an image is a file
//     relative to base (never an address with a scheme). Notes passes one that allows only the note's own PNG and JPEG
//     files (GitHub #238).
//   taskStyle (optional): 'round' draws a checklist item as a circle (a ring when open; filled with taskTint, default
//     the accent, and a white ✓ when done; a 28 px hit target on the first line; the done text not dimmed), as Notes
//     does. Without it, the 16 px square box, done items dimmed (every other app).
//   markdownHeadings(text) → [{ level, text, id, line }]   (for a table of contents)
//
// Blocks: headings, paragraphs, quotes, lists (with levels), numbered lists, checkboxes (- [ ] / - [x]),
// code blocks, tables, separators. Inline: **bold**, *italic*, ~~strikethrough~~, `code`,
// [links](url), ![images](file.png) (relative to base) and :emoji: (with the emoji map).
//
// The engine has no runs with different styles inside a single <Text>: a paragraph is a
// row with flexWrap and one <Text> per word (the space sticks to the word), so it breaks lines
// between words and each word carries its own style.
import { View, Text, Image } from '@lordos/runtime';
import { useTheme } from './theme.js';

// ---- blocks ----

const slug = (s) => s.toLowerCase().replace(/[^\p{L}\p{N}]+/gu, '-').replace(/^-|-$/g, '') || 'titulo';

export function parseMarkdown(src, opts = {}) {
  const lines = String(src || '').replace(/\r\n/g, '\n').split('\n');
  const blocks = [];
  const ids = new Map();
  const uniq = (s) => { const b = slug(s); const n = (ids.get(b) || 0) + 1; ids.set(b, n); return n > 1 ? `${b}-${n}` : b; };
  let i = 0;
  const isListLine = (s) => /^\s*([-*+]|\d+[.)])\s+/.test(s);
  while (i < lines.length) {
    const s = lines[i];
    if (!s.trim()) { i++; continue; }
    let m;
    if ((m = /^(`{3,}|~{3,})\s*([\w+-]*)/.exec(s))) {             // code block
      const fence = m[1], start = i;
      const body = [];
      i++;
      while (i < lines.length && !lines[i].startsWith(fence)) body.push(lines[i++]);
      i++;
      blocks.push({ type: 'code', lang: m[2], text: body.join('\n'), line: start });
      continue;
    }
    if ((m = /^(#{1,6})\s+(.*?)\s*#*\s*$/.exec(s))) {
      blocks.push({ type: 'heading', level: m[1].length, text: m[2], id: uniq(m[2]), line: i });
      i++;
      continue;
    }
    if (/^\s*([-*_])(\s*\1){2,}\s*$/.test(s)) { blocks.push({ type: 'hr', line: i }); i++; continue; }
    if (/^\s*>/.test(s)) {                                         // quote
      const start = i;
      const body = [];
      while (i < lines.length && /^\s*>/.test(lines[i])) body.push(lines[i++].replace(/^\s*>\s?/, ''));
      blocks.push({ type: 'quote', blocks: parseMarkdown(body.join('\n'), opts), line: start });
      continue;
    }
    if (/^\s*\|.*\|\s*$/.test(s) && i + 1 < lines.length && /^\s*\|?\s*:?-{2,}/.test(lines[i + 1])) {   // table
      const cells = (row) => row.trim().replace(/^\||\|$/g, '').split('|').map((c) => c.trim());
      const header = cells(s);
      const align = cells(lines[i + 1]).map((c) => (/^:-+:$/.test(c) ? 'center' : /-:$/.test(c) ? 'end' : 'start'));
      const start = i;
      i += 2;
      const rows = [];
      while (i < lines.length && /^\s*\|.*\|\s*$/.test(lines[i])) rows.push(cells(lines[i++]));
      blocks.push({ type: 'table', header, align, rows, line: start });
      continue;
    }
    if (isListLine(s)) {                                           // lists (indentation = level)
      const items = [];
      const start = i;
      while (i < lines.length && (isListLine(lines[i]) || (lines[i].trim() && /^\s{2,}/.test(lines[i]) && items.length))) {
        const li = lines[i];
        const lm = /^(\s*)([-*+]|(\d+)[.)])\s+(\[([ xX])\]\s+)?(.*)$/.exec(li);
        if (lm) {
          items.push({ depth: Math.floor(lm[1].replace(/\t/g, '  ').length / 2), ordered: !!lm[3], num: lm[3] ? Number(lm[3]) : 0,
            task: lm[4] ? lm[5] !== ' ' : null, text: lm[6], line: i });
        } else items[items.length - 1].text += ` ${li.trim()}`;     // continuation
        i++;
      }
      blocks.push({ type: 'list', items, line: start });
      continue;
    }
    const start = i;                                              // paragraph
    const body = [];
    while (i < lines.length && lines[i].trim() && !/^(#{1,6}\s|>|`{3,}|~{3,})/.test(lines[i]) && !isListLine(lines[i])) body.push(lines[i++].trim());
    blocks.push({ type: 'paragraph', text: body.join(' '), ...(opts.breaks && body.length > 1 ? { lines: body } : null), line: start });
  }
  return blocks;
}

export function markdownHeadings(src) {
  const out = [];
  const walk = (bs) => { for (const b of bs) { if (b.type === 'heading') out.push({ level: b.level, text: stripInline(b.text), id: b.id, line: b.line }); if (b.type === 'quote') walk(b.blocks); } };
  walk(parseMarkdown(src));
  return out;
}

// ---- inline ----

const INLINE = [
  ['code', /`([^`]+)`/y],
  ['image', /!\[([^\]]*)\]\(([^)\s]+)(?:\s+"[^"]*")?\)/y],
  ['link', /\[([^\]]+)\]\(([^)\s]+)(?:\s+"[^"]*")?\)/y],
  ['bold', /\*\*(.+?)\*\*/y], ['bold', /__(.+?)__/y],
  ['strike', /~~(.+?)~~/y],
  ['italic', /\*([^*\s][^*]*?)\*/y], ['italic', /(?<![\p{L}\p{N}])_([^_\s][^_]*?)_(?![\p{L}\p{N}])/uy],
  ['emoji', /:([a-z0-9_+-]{2,32}):/y],
  ['url', /https?:\/\/[^\s<>()]+[^\s<>().,;:!?]/y],
];

export function parseInline(text, style = {}, emoji = null) {
  const out = [];
  let buf = '';
  const flush = () => { if (buf) { out.push({ text: buf, ...style }); buf = ''; } };
  for (let i = 0; i < text.length;) {
    let hit = null;
    if ('`![*_~:h'.includes(text[i])) {
      for (const [kind, re] of INLINE) {
        re.lastIndex = i;
        const m = re.exec(text);
        if (m) { hit = [kind, m]; break; }
      }
    }
    if (!hit) {
      if (text[i] === '\\' && i + 1 < text.length) { buf += text[i + 1]; i += 2; continue; }
      buf += text[i++];
      continue;
    }
    const [kind, m] = hit;
    if (kind === 'emoji' && !(emoji && emoji(m[1]))) { buf += text[i++]; continue; }
    flush();
    if (kind === 'code') out.push({ text: m[1], ...style, code: true });
    else if (kind === 'image') out.push({ image: m[2], alt: m[1] });
    else if (kind === 'link') out.push(...parseInline(m[1], { ...style, link: m[2] }, emoji));
    else if (kind === 'url') out.push({ text: m[0], ...style, link: m[0] });
    else if (kind === 'emoji') out.push({ text: emoji(m[1]), ...style });
    else out.push(...parseInline(m[1], { ...style, [kind]: true }, emoji));
    i += m[0].length;
  }
  flush();
  return out;
}

export const stripInline = (s) => parseInline(s).map((x) => x.text || x.alt || '').join('');

// ---- drawing ----

const join = (base, p) => (p.startsWith('/') ? p : `${base}/${p}`.replace(/\/+/g, '/'));

function Inline({ text, size, color, weight = 400, width, base, onLink, emoji, mono, resolveImage }) {
  const t = useTheme();
  const segs = parseInline(text, {}, emoji);
  const out = [];
  segs.forEach((sg, si) => {
    if (sg.image) {
      let src = null;
      if (resolveImage) { try { src = resolveImage(sg.image) || null; } catch (e) { src = null; } }
      else src = /^[a-z]+:/i.test(sg.image) ? null : join(base || '', sg.image);
      if (!src) { out.push(<Text key={`i${si}`} style={{ fontSize: size, color: t.textDim }}>{`[${sg.alt || sg.image}]`}</Text>); return; }
      const nat = typeof lord.image !== 'undefined' ? lord.image.size(src) : null;
      const max = Math.max(40, (width || 400) - 8);
      const w = nat ? Math.min(nat.width, max) : max;
      const h = nat ? Math.round(w * nat.height / Math.max(1, nat.width)) : Math.round(w * 0.6);
      out.push(<Image key={`i${si}`} src={src} style={{ width: w, height: h, borderRadius: 6, marginVertical: 4, objectFit: 'contain' }} />);
      return;
    }
    const st = {
      fontSize: sg.code ? size - 1 : size, color: sg.link ? t.accent : color, fontWeight: sg.bold ? 700 : weight,
      fontFamily: sg.code || mono ? 'monospace' : undefined, whiteSpace: 'nowrap',
    };
    if (sg.code) { st.backgroundColor = t.mode === 'dark' ? 'rgba(255,255,255,0.1)' : 'rgba(0,0,0,0.07)'; st.paddingHorizontal = 3; st.borderRadius = 4; }
    if (sg.italic) st.color = sg.link ? t.accent : t.mode === 'dark' ? '#e9e3ff' : '#4b3b8f';
    if (sg.strike) st.opacity = 0.55;
    const words = sg.code ? [sg.text] : sg.text.split(/(?<=\s)/);
    words.forEach((w, wi) => {
      if (!w) return;
      const click = sg.link ? (e) => { e.stopPropagation?.(); onLink?.(sg.link); } : undefined;
      // Combining U+0336 is missing from most fonts (it drew boxes): strike-through is a hairline
      // across the word instead.
      if (sg.strike) {
        out.push(<View key={`${si}.${wi}`}>
          <Text onClick={click} style={st}>{w}</Text>
          {w.trim() ? <View style={{ position: 'absolute', left: 0, right: w.endsWith(' ') ? st.fontSize * 0.28 : 0, top: Math.round(st.fontSize * 0.68), height: 1, backgroundColor: st.color, opacity: 0.55 }} /> : null}
        </View>);
        return;
      }
      out.push(<Text key={`${si}.${wi}`} onClick={click} style={st}>{w}</Text>);
    });
  });
  return <View style={{ flexDirection: 'row', flexWrap: 'wrap', alignItems: 'flex-end' }}>{out}</View>;
}

const H_SIZE = [0, 22, 18, 16, 14, 13, 13];

function Block({ b, props }) {
  const t = useTheme();
  const { size = 14, color = t.text, width, base, onLink, onToggleTask, onHeadingRef, emoji, fitWidth, resolveImage, taskStyle, taskTint } = props;
  const inl = (text, extra = {}) => <Inline text={text} size={size} color={color} width={width} base={base} onLink={onLink} emoji={emoji} resolveImage={resolveImage} {...extra} />;
  switch (b.type) {
    case 'heading':
      return (
        <View ref={(n) => onHeadingRef?.(b.id, n)} style={{ marginTop: b.level <= 2 ? 10 : 6, marginBottom: 4 }}>
          {inl(b.text, { size: H_SIZE[b.level] || size, weight: 700 })}
          {b.level === 1 && <View style={{ height: 1, marginTop: 4, backgroundColor: t.separator }} />}
        </View>
      );
    case 'paragraph':
      // breaks: one row per typed line (each one still wraps at the width)
      return <View style={{ marginVertical: 3 }}>{b.lines ? b.lines.map((l, i) => <View key={i}>{inl(l)}</View>) : inl(b.text)}</View>;
    case 'hr': return <View style={{ height: 1, marginVertical: 8, backgroundColor: t.separator }} />;
    case 'code':
      return (
        <View style={{ marginVertical: 4, padding: 8, borderRadius: 7, backgroundColor: t.mode === 'dark' ? 'rgba(0,0,0,0.35)' : 'rgba(0,0,0,0.06)' }}>
          {b.text.split('\n').map((l, i) => (
            <Text key={i} style={{ fontFamily: 'monospace', fontSize: size - 1, color, whiteSpace: 'nowrap' }}>{l || ' '}</Text>
          ))}
        </View>
      );
    case 'quote':
      return (
        <View style={{ flexDirection: 'row', marginVertical: 4 }}>
          <View style={{ width: 3, borderRadius: 2, backgroundColor: t.accent, marginRight: 10 }} />
          <View style={{ flex: 1, opacity: 0.85 }}>{b.blocks.map((x, i) => <Block key={i} b={x} props={props} />)}</View>
        </View>
      );
    case 'list': {
      const counters = [];
      return (
        <View style={{ marginVertical: 3, gap: 2 }}>
          {b.items.map((it, i) => {
            counters[it.depth] = it.ordered ? (counters[it.depth] || it.num || 1) : 0;
            const n = counters[it.depth]++;
            counters.length = it.depth + 1;
            const mark = it.task !== null ? null : it.ordered ? `${n}.` : ['•', '◦', '▪'][it.depth % 3];
            if (it.task !== null && taskStyle === 'round') {
              // round (Notes, like a phone's notes app): a 21 px circle, a ring when open, filled with the tint and a white
              // ✓ when done, in a 28 px hit target centred on the item's first line; the done text stays readable
              const lineH = Math.round(size * 1.3), D = 21, HIT = 28;
              return (
                <View key={i} style={{ flexDirection: 'row', alignItems: 'flex-start', marginStart: it.depth * 18 }}>
                  <View onClick={(e) => { e.stopPropagation?.(); onToggleTask?.(it.line); }}
                    style={{ width: HIT, height: HIT, marginTop: Math.round((lineH - HIT) / 2), marginEnd: 4, alignItems: 'center', justifyContent: 'center' }}>
                    <View style={{ width: D, height: D, borderRadius: D / 2, alignItems: 'center', justifyContent: 'center',
                      backgroundColor: it.task ? taskTint || t.accent : 'transparent', border: it.task ? `1.5px solid ${taskTint || t.accent}` : `1.5px solid ${t.textDim}` }}>
                      {it.task && <Text style={{ fontSize: 13, fontWeight: 700, color: '#ffffff' }}>✓</Text>}
                    </View>
                  </View>
                  <View style={fitWidth && width ? { width: width - 18 * it.depth - HIT - 4 } : { flex: 1 }}>{inl(it.text)}</View>
                </View>
              );
            }
            return (
              <View key={i} style={{ flexDirection: 'row', alignItems: 'flex-start', marginLeft: it.depth * 18 }}>
                {it.task !== null ? (
                  <View onClick={(e) => { e.stopPropagation?.(); onToggleTask?.(it.line); }}
                    style={{ width: 16, height: 16, borderRadius: 4, marginTop: 2, marginRight: 8, alignItems: 'center', justifyContent: 'center',
                      backgroundColor: it.task ? t.accent : 'transparent', border: `1.5px solid ${it.task ? t.accent : t.textDim}` }}>
                    {it.task && <Text style={{ fontSize: 11, fontWeight: 700, color: '#fff' }}>✓</Text>}
                  </View>
                ) : <Text style={{ width: 18, fontSize: size, color: t.textDim }}>{mark}</Text>}
                {/* fitWidth: the text of a list item gets an explicit width, so the engine measures its wrapped
                    height at the width it is drawn at (a long item no longer overlaps the next one) */}
                <View style={[fitWidth && width ? { width: width - 18 * (it.depth + 1) } : { flex: 1 }, { opacity: it.task ? 0.6 : 1 }]}>{inl(it.text)}</View>
              </View>
            );
          })}
        </View>
      );
    }
    case 'table':
      return (
        <View style={{ marginVertical: 6, borderRadius: 6, border: `1px solid ${t.separator}`, overflow: 'hidden' }}>
          {[b.header, ...b.rows].map((row, r) => (
            <View key={r} style={{ flexDirection: 'row', backgroundColor: r === 0 ? t.hover : r % 2 ? 'transparent' : (t.mode === 'dark' ? 'rgba(255,255,255,0.03)' : 'rgba(0,0,0,0.02)'),
              borderTop: r ? `1px solid ${t.separator}` : '0 solid transparent' }}>
              {b.header.map((_, c) => (
                <View key={c} style={{ flex: 1, padding: 6, alignItems: b.align[c] === 'center' ? 'center' : b.align[c] === 'end' ? 'flex-end' : 'flex-start' }}>
                  {inl(row[c] || '', { weight: r === 0 ? 700 : 400 })}
                </View>
              ))}
            </View>
          ))}
        </View>
      );
    default: return null;
  }
}

export function Markdown(props) {
  const blocks = parseMarkdown(props.source, { breaks: !!props.breaks });
  return <View style={[{ gap: 2 }, props.style]}>{blocks.map((b, i) => <Block key={`${b.line}:${i}`} b={b} props={props} />)}</View>;
}

// Toggles [ ] ↔ [x] on the given line of the text (for checkboxes ticked from reading view).
export function toggleTask(src, line) {
  const lines = String(src).split('\n');
  if (line >= 0 && line < lines.length) lines[line] = lines[line].replace(/^(\s*([-*+]|\d+[.)])\s+)\[([ xX])\]/, (m, a, b, c) => `${a}[${c === ' ' ? 'x' : ' '}]`);
  return lines.join('\n');
}
