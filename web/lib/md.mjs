// Small Markdown renderer for the site's content (no dependencies). Supports what the content
// uses and escapes everything else:
//   front matter (--- key: value ---), # headings (with ids), paragraphs, - / 1. lists (nested by
//   indentation), > blockquotes, ``` fenced code, | tables |, ---, **bold**, *italic*, `code`,
//   [links](url), ![images](repo/path.png "caption") and block directives on their own line:
//   [[diagram:<id>]]  [[shot:<n>]]
// Raw HTML is never passed through; HTML comments (<!-- … -->) are dropped.

export const esc = (s) => String(s).replace(/&/g, '&amp;').replace(/</g, '&lt;').replace(/>/g, '&gt;').replace(/"/g, '&quot;');

export function slugify(s) {
  return s.toLowerCase().normalize('NFD').replace(/[̀-ͯ]/g, '').replace(/[^a-z0-9]+/g, '-').replace(/^-|-$/g, '') || 'section';
}

export function frontMatter(src) {
  const m = src.match(/^---\r?\n([\s\S]*?)\r?\n---\r?\n?/);
  if (!m) return { data: {}, body: src };
  const data = {};
  for (const line of m[1].split(/\r?\n/)) {
    const kv = line.match(/^([\w-]+):\s*(.*)$/);
    if (kv) data[kv[1]] = kv[2].replace(/^["']|["']$/g, '');
  }
  return { data, body: src.slice(m[0].length) };
}

// inline: code spans first (verbatim), then images, links, bold, italic
export function inline(text, opts = {}) {
  const codes = [];
  let s = text.replace(/`([^`]+)`/g, (_, c) => { codes.push(`<code>${esc(c)}</code>`); return `\u0000${codes.length - 1}\u0000`; });
  s = esc(s);
  s = s.replace(/!\[([^\]]*)\]\(([^)\s]+)(?:\s+&quot;([^&]*)&quot;)?\)/g, (_, alt, url, title) =>
    opts.image ? opts.image(url, alt, title) : `<img src="${url}" alt="${alt}">`);
  s = s.replace(/\[([^\]]+)\]\(([^)\s]+)\)/g, (_, label, url) => {
    const href = opts.link ? opts.link(url) : url;
    if (!/^(https?:|mailto:|\/|#|\.)/.test(href)) return label; // no javascript: or other schemes
    const ext = /^https?:/.test(href) ? ' rel="noopener"' : '';
    return `<a href="${href}"${ext}>${label}</a>`;
  });
  s = s.replace(/\*\*([^*]+)\*\*/g, '<strong>$1</strong>').replace(/(^|[^\w*])\*([^*\n]+)\*(?!\w)/g, '$1<em>$2</em>')
    .replace(/(^|[^\w])_([^_\n]+)_(?!\w)/g, '$1<em>$2</em>');
  return s.replace(/\u0000(\d+)\u0000/g, (_, i) => codes[+i]);
}

const isTableSep = (l) => /^\s*\|?\s*:?-{2,}:?\s*(\|\s*:?-{2,}:?\s*)*\|?\s*$/.test(l);
const cells = (l) => l.trim().replace(/^\|/, '').replace(/\|$/, '').split('|').map((c) => c.trim());

// Returns { html, headings: [{level, id, text}] }
export function markdown(src, opts = {}) {
  // HTML comments are notes for editors (e.g. `<!-- PI-RESULT: … -->` where a measurement goes): never
  // rendered, so one left behind publishes nothing. A comment alone on its line takes the line with it.
  const lines = src.replace(/\r\n/g, '\n').replace(/^[ \t]*<!--[\s\S]*?-->[ \t]*\n/gm, '')
    .replace(/[ \t]?<!--[\s\S]*?-->/g, '').split('\n');
  const out = [];
  const headings = [];
  const used = new Set();
  const block = opts.block || (() => '');
  let i = 0;
  const para = [];
  const flush = () => { if (para.length) { out.push(`<p>${inline(para.join(' '), opts)}</p>`); para.length = 0; } };

  const list = (start) => {
    // collects a (possibly nested) list starting at line `start`; returns [html, nextLine]
    const ind = (l) => l.match(/^(\s*)/)[1].length;
    const base = ind(lines[start]);
    const ordered = /^\s*\d+[.)]\s/.test(lines[start]);
    const items = [];
    let j = start;
    while (j < lines.length) {
      const l = lines[j];
      if (!l.trim()) { if (j + 1 < lines.length && /^\s*([-*+]|\d+[.)])\s/.test(lines[j + 1]) && ind(lines[j + 1]) >= base) { j++; continue; } break; }
      const m = l.match(/^(\s*)([-*+]|\d+[.)])\s+(.*)$/);
      if (m && m[1].length === base) { items.push({ text: m[3], sub: '' }); j++; continue; }
      if (m && m[1].length > base && items.length) { const [h, n] = list(j); items[items.length - 1].sub += h; j = n; continue; }
      if (!m && ind(l) > base && items.length) { items[items.length - 1].text += ' ' + l.trim(); j++; continue; }
      break;
    }
    const tag = ordered ? 'ol' : 'ul';
    return [`<${tag}>${items.map((it) => `<li>${inline(it.text, opts)}${it.sub}</li>`).join('')}</${tag}>`, j];
  };

  while (i < lines.length) {
    const line = lines[i];
    const t = line.trim();
    if (!t) { flush(); i++; continue; }
    let m;
    if ((m = t.match(/^(?:\[\[|\{\{)\s*([\w-]+)(?::([\w./-]+))?\s*(?:\]\]|\}\})$/))) { flush(); out.push(block(m[1], m[2])); i++; continue; }
    if ((m = t.match(/^```\s*([\w-]*)/))) {
      flush();
      const lang = m[1];
      const buf = [];
      i++;
      while (i < lines.length && !lines[i].trim().startsWith('```')) buf.push(lines[i++]);
      i++;
      out.push(`<pre class="code"${lang ? ` data-lang="${esc(lang)}"` : ''}><code>${esc(buf.join('\n'))}</code></pre>`);
      continue;
    }
    if ((m = t.match(/^(#{1,6})\s+(.*?)\s*#*$/))) {
      flush();
      const level = m[1].length;
      const text = m[2];
      let id = slugify(text.replace(/[`*_]/g, ''));
      while (used.has(id)) id += '-2';
      used.add(id);
      headings.push({ level, id, text: text.replace(/[`*_]/g, '') });
      const anchor = level >= 2 && level <= 3 ? `<a class="anchor" href="#${id}" aria-label="${esc(opts.anchorLabel || 'Link to this section')}">#</a>` : '';
      out.push(`<h${level} id="${id}">${inline(text, opts)}${anchor}</h${level}>`);
      i++;
      continue;
    }
    if (/^(-{3,}|\*{3,})$/.test(t)) { flush(); out.push('<hr>'); i++; continue; }
    if (t.startsWith('>')) {
      flush();
      const buf = [];
      while (i < lines.length && lines[i].trim().startsWith('>')) buf.push(lines[i++].trim().replace(/^>\s?/, ''));
      out.push(`<blockquote>${markdown(buf.join('\n'), { ...opts, block: () => '' }).html}</blockquote>`);
      continue;
    }
    if (t.includes('|') && i + 1 < lines.length && isTableSep(lines[i + 1])) {
      flush();
      const head = cells(line);
      const align = cells(lines[i + 1]).map((c) => (c.startsWith(':') && c.endsWith(':') ? 'center' : c.endsWith(':') ? 'right' : ''));
      i += 2;
      const rows = [];
      while (i < lines.length && lines[i].includes('|') && lines[i].trim()) rows.push(cells(lines[i++]));
      const td = (tag, c, k) => `<${tag}${align[k] ? ` class="ta-${align[k]}"` : ''}>${inline(c, opts)}</${tag}>`;
      out.push(`<div class="table"><table><thead><tr>${head.map((c, k) => td('th', c, k)).join('')}</tr></thead><tbody>${rows.map((r) => `<tr>${r.map((c, k) => td('td', c, k)).join('')}</tr>`).join('')}</tbody></table></div>`);
      continue;
    }
    if (/^\s*([-*+]|\d+[.)])\s+/.test(line)) { flush(); const [h, n] = list(i); out.push(h); i = n; continue; }
    para.push(t);
    i++;
  }
  flush();
  return { html: out.join('\n'), headings };
}
