// chartsvg.mjs — small horizontal bar charts ([[chart:<id>]] in a page, web/content/charts/<id>.json) as inline
// SVG, without dependencies. Real text in the site's font and colour tokens (var(--…) from site.css), so a chart
// follows light and dark like the architecture figures (archsvg.mjs); it reuses their frame (.arch-fig) and
// their two layouts (.arch-wide, .arch-narrow: labels beside the bars, or above them on a phone).
//
// The JSON: { id, note, title{en,es,pt}, summary{…}?, unit, bars: [{ label{…}, value, tone? }], refs?: [{ value,
// label{…} }], legend?: [{ tone, label{…} }], footnote{…}?, text: { en: [...] }? }. tone is "base" (default),
// "hi" (highlighted) or "file" (dashed: file-backed pages). Every bar starts at zero.
import { esc } from './md.mjs';

const loc = (v, l) => (v == null ? '' : typeof v === 'string' ? v : v[l] ?? v.en ?? '');
const num = (v, l) => Number(v).toLocaleString(l === 'en' ? 'en-US' : l === 'es' ? 'es-AR' : 'pt-BR', { maximumFractionDigits: 1 });

function wrap(text, width, fs, k = 0.55) {
  const max = Math.max(6, Math.floor(width / (fs * k)));
  const out = [];
  let line = '';
  for (const w of String(text).split(/\s+/)) {
    if (!w) continue;
    if (line && (line + ' ' + w).length > max) { out.push(line); line = w; } else line = line ? line + ' ' + w : w;
  }
  if (line) out.push(line);
  return out;
}

const TONES = {
  base: 'style="fill:var(--t-reused-bg);stroke:var(--subtle)" stroke-width="1.5"',
  hi: 'style="fill:var(--t-ours-bd);stroke:var(--t-ours-bd)" stroke-width="1.5"',
  file: 'style="fill:var(--surface);stroke:var(--subtle)" stroke-width="1.5" stroke-dasharray="5 4"',
};

function text(x, y, s, { fs = 13, weight = 500, color = 'var(--ink-2)', anchor = 'start' } = {}) {
  return `<text x="${x.toFixed(1)}" y="${y.toFixed(1)}" text-anchor="${anchor}" style="font-family:var(--ff);font-size:${fs}px;font-weight:${weight};fill:${color}">${esc(s)}</text>`;
}

function layout(c, l, narrow) {
  const W = narrow ? 340 : 720, fs = 13.5, barH = narrow ? 18 : 22, gap = narrow ? 14 : 12;
  const unit = loc(c.unit, l);
  const valW = narrow ? 70 : 84;
  const labW = narrow ? 0 : 230;
  const x0 = narrow ? 0 : labW + 12;
  const span = W - x0 - valW;
  const max = Math.max(...c.bars.map((b) => b.value), ...(c.refs || []).map((r) => r.value));
  const sx = (v) => x0 + (v / max) * span;
  const parts = [];
  const refs = c.refs || [];
  let y = refs.length ? (narrow ? 8 : 26) : 4;
  const top = y;
  const rows = [];
  for (const b of c.bars) {
    const tone = TONES[b.tone] ? b.tone : 'base';
    const lines = wrap(loc(b.label, l), narrow ? W - 4 : labW, fs);
    let by;
    if (narrow) {
      lines.forEach((s, i) => parts.push(text(0, y + fs + i * fs * 1.3, s, { fs, weight: tone === 'hi' ? 700 : 600 })));
      by = y + lines.length * fs * 1.3 + 6;
    } else {
      const lh = lines.length * fs * 1.3;
      by = y + Math.max(0, (lh - barH) / 2);
      const ty = by + barH / 2 - (lines.length - 1) * fs * 0.65 + fs * 0.35;
      lines.forEach((s, i) => parts.push(text(labW, ty + i * fs * 1.3, s, { fs, weight: tone === 'hi' ? 700 : 600, anchor: 'end' })));
    }
    const w = Math.max(2, sx(b.value) - x0);
    rows.push(by);
    parts.push(`<rect x="${x0}" y="${by.toFixed(1)}" width="${w.toFixed(1)}" height="${barH}" rx="4" ${TONES[tone]}/>`);
    parts.push(text(x0 + w + 8, by + barH / 2 + fs * 0.36, `${num(b.value, l)} ${unit}`,
      { fs, weight: 800, color: tone === 'hi' ? 'var(--t-ours-fg)' : 'var(--ink)' }));
    y = Math.max(by + barH, narrow ? 0 : y + lines.length * fs * 1.3) + gap;
  }
  const bottom = y - gap + 6;
  // the zero line
  parts.push(`<line x1="${x0}" y1="${top - 4}" x2="${x0}" y2="${bottom}" style="stroke:var(--line-2)" stroke-width="2"/>`);
  // reference lines (a value to compare with), labelled above the bars (wide) or below them (narrow)
  let H = bottom + 4;
  refs.forEach((r, i) => {
    const x = sx(r.value);
    // on a phone the labels sit above the bars: the line crosses the bars only, not the labels
    const seg = (y1, y2) => parts.push(`<line x1="${x.toFixed(1)}" y1="${y1.toFixed(1)}" x2="${x.toFixed(1)}" y2="${y2.toFixed(1)}" style="stroke:var(--muted)" stroke-width="1.5" stroke-dasharray="3 4"/>`);
    if (narrow) rows.forEach((by) => seg(by - 3, by + barH + 3)); else seg(top - 6, bottom);
    const s = `${loc(r.label, l)}: ${num(r.value, l)} ${unit}`;
    if (narrow) {
      const lines = wrap('┆ ' + s, W, 11.5);
      lines.forEach((t, j) => parts.push(text(0, H + 14 + j * 15, t, { fs: 11.5, weight: 600, color: 'var(--muted)' })));
      H += lines.length * 15 + 6;
    } else {
      const anchor = x > W - 160 ? 'end' : 'middle';
      parts.push(text(anchor === 'end' ? x + 4 : x, top - 12 - i * 16, s, { fs: 12, weight: 600, color: 'var(--muted)', anchor }));
    }
  });
  // legend
  if (c.legend && c.legend.length) {
    let lx = 0, ly = H + 20;
    for (const it of c.legend) {
      const lines = wrap(loc(it.label, l), W - 24, 11.5, 0.6), w = 22 + Math.max(...lines.map((t) => t.length)) * 11.5 * 0.6 + 18;
      if (lx && (narrow || lx + w > W)) { lx = 0; ly += 20; }
      parts.push(`<rect x="${lx}" y="${ly - 11}" width="14" height="14" rx="3" ${TONES[it.tone] || TONES.base}/>`);
      lines.forEach((t, j) => parts.push(text(lx + 20, ly + j * 15, t, { fs: 11.5, weight: 600, color: 'var(--muted)' })));
      ly += (lines.length - 1) * 15;
      lx += w;
    }
    H = ly + 8;
  }
  return { W, H: Math.ceil(H), body: parts.join('') };
}

// What a screen reader gets (the SVG's <desc>) and what the Markdown copy prints: every value, in order.
function described(c, l) {
  const unit = loc(c.unit, l);
  const bars = c.bars.map((b) => `${loc(b.label, l)}: ${num(b.value, l)} ${unit}`).join('; ');
  const refs = (c.refs || []).map((r) => `${loc(r.label, l)}: ${num(r.value, l)} ${unit}`).join('; ');
  return [loc(c.summary, l), bars + '.', refs ? refs + '.' : ''].filter(Boolean).join(' ');
}

export function chartFigure(c, l) {
  const title = loc(c.title, l), foot = loc(c.footnote, l), sum = loc(c.summary, l);
  const one = (lay, cls, n) => {
    const tid = `chart-${c.id}-${n}-t`, did = `chart-${c.id}-${n}-d`;
    return `<div class="${cls}"><svg viewBox="0 0 ${lay.W} ${lay.H}" width="100%" role="img" aria-labelledby="${tid} ${did}" xmlns="http://www.w3.org/2000/svg"><title id="${tid}">${esc(title)}</title><desc id="${did}">${esc(described(c, l))}</desc>${lay.body}</svg></div>`;
  };
  return `<figure class="arch-fig chart-fig" id="chart-${esc(c.id)}"><p class="arch-title">${esc(title)}</p>${sum ? `<p class="chart-sum">${esc(sum)}</p>` : ''}${one(layout(c, l, false), 'arch-wide', 'w')}${one(layout(c, l, true), 'arch-narrow', 'n')}${foot ? `<figcaption>${esc(foot)}</figcaption>` : ''}</figure>`;
}

// For docs/PAPER.md: the chart as a Markdown table (English).
// Highlighted bars are bold, reference lines italic, and a bar in a legend's tone other than the default says which.
export function chartText(c) {
  const unit = loc(c.unit, 'en');
  const md = (s) => String(s).replace(/</g, '&lt;').replace(/>/g, '&gt;').replace(/\|/g, '\\|');
  const tone = (b) => { const it = b.tone && b.tone !== 'base' && b.tone !== 'hi' && (c.legend || []).find((x) => x.tone === b.tone); return it ? ` (${md(loc(it.label, 'en'))})` : ''; };
  const rows = c.bars.map((b) => {
    const label = md(loc(b.label, 'en')) + tone(b), value = `${num(b.value, 'en')} ${unit}`;
    return b.tone === 'hi' ? `| **${label}** | **${value}** |` : `| ${label} | ${value} |`;
  });
  const refs = (c.refs || []).map((r) => `| *${md(loc(r.label, 'en'))}* | *${num(r.value, 'en')} ${unit}* |`);
  return [`**Chart: ${md(loc(c.title, 'en'))}.**${c.summary ? ' ' + md(loc(c.summary, 'en')) : ''}`, '', '| | |', '|---|---:|', ...rows, ...refs,
    ...(c.footnote ? ['', `*${md(loc(c.footnote, 'en'))}*`] : [])].join('\n');
}
