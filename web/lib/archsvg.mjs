// archsvg.mjs — the LordOS Web Runtime's layer figure (web/content/figures/web-runtime.json) as inline SVG.
// Real text in the site's font and colour tokens (var(--t-*) from site.css), so it follows light and dark
// like the HTML diagrams. Two layouts: wide (the apps side by side) and narrow (stacked, for phones); the
// page shows one of them with CSS (.arch-wide / .arch-narrow in doc.css).
import { esc } from './md.mjs';

const loc = (v, l) => (v == null ? '' : typeof v === 'string' ? v : v[l] ?? v.en ?? '');
const fill = (s, vars) => s.replace(/\{(\w+)\}/g, (m, k) => (k in vars ? vars[k] : m));

// Greedy wrap by an estimate of the font's advance (Plus Jakarta Sans is about 0.56 em per character).
function wrap(text, width, fs, k = 0.53) {
  const max = Math.max(8, Math.floor(width / (fs * k)));
  const out = [];
  let line = '';
  for (const w of String(text).split(/\s+/)) {
    if (!w) continue;
    if (line && (line + ' ' + w).length > max) { out.push(line); line = w; } else line = line ? line + ' ' + w : w;
  }
  if (line) out.push(line);
  return out;
}

const TONE = {
  ours: ['var(--t-ours-bg)', 'var(--t-ours-bd)', 'var(--t-ours-fg)'],
  reused: ['var(--t-reused-bg)', 'var(--t-reused-bd)', 'var(--t-reused-fg)'],
  kernel: ['var(--t-kernel-bg)', 'var(--t-kernel-bd)', 'var(--t-kernel-fg)'],
  user: ['var(--t-user-bg)', 'var(--t-user-bd)', 'var(--t-user-fg)'],
};

class Svg {
  constructor() { this.parts = []; }
  rect(x, y, w, h, tone, { dash = false, r = 12, sw = 2, fillOverride } = {}) {
    const [bg, bd] = TONE[tone];
    this.parts.push(`<rect x="${x}" y="${y}" width="${w}" height="${h}" rx="${r}" style="fill:${fillOverride || bg};stroke:${bd}" stroke-width="${sw}"${dash ? ' stroke-dasharray="6 5"' : ''}/>`);
  }
  // lines of text from (x, y) (baseline of the first line), returns the y after the block
  text(x, y, lines, { fs = 13, lh = 1.35, weight = 500, color = 'var(--ink)', anchor = 'start', mono = false, opacity } = {}) {
    const fam = mono ? 'var(--mono)' : 'var(--ff)';
    lines.forEach((t, i) => {
      this.parts.push(`<text x="${x}" y="${(y + i * fs * lh).toFixed(1)}" text-anchor="${anchor}" style="font-family:${fam};font-size:${fs}px;font-weight:${weight};fill:${color}${opacity ? `;opacity:${opacity}` : ''}">${esc(t)}</text>`);
    });
    return y + (lines.length - 1) * fs * lh;
  }
  arrow(x, y1, y2, label, { width = 300, fs = 12 } = {}) {
    this.parts.push(`<line x1="${x}" y1="${y1}" x2="${x}" y2="${y2 - 7}" style="stroke:var(--subtle)" stroke-width="2"/>`,
      `<path d="M${x - 6} ${y2 - 9}L${x} ${y2}L${x + 6} ${y2 - 9}z" style="fill:var(--subtle)"/>`);
    if (label) {
      const lines = wrap(label, width, fs);
      const top = (y1 + y2) / 2 - ((lines.length - 1) * fs * 1.3) / 2 + fs * 0.35;
      this.text(x + 12, top, lines, { fs, weight: 600, color: 'var(--muted)' });
    }
  }
}

const PAD = 14;

// One profile's column: lord-web, WPE WebKit with its sandbox, the data folder. Returns its height.
function column(svg, f, l, app, prof, x, y, w, draw) {
  const fsL = 14.5, fsS = 12, fsK = 11.5;
  const inner = w - 2 * PAD;
  // lord-web
  const title = `${loc(f.web.label, l)} · ${loc(app.app, l)} · ${loc(prof.name, l)}`;
  const tl = wrap(title, inner, fsL, 0.57);
  const kl = wrap(loc(f.web.kicker, l), inner, fsK);
  const sl = wrap(loc(f.web.sub, l), inner, fsS);
  const h1 = PAD + tl.length * fsL * 1.3 + 4 + kl.length * fsK * 1.35 + 6 + sl.length * fsS * 1.35 + PAD - 4;
  // engine
  const el = wrap(loc(f.engine.label, l), inner, fsL, 0.57);
  const esl = wrap(loc(f.engine.sub, l), inner, fsS);
  const sbl = wrap(loc(f.engine.sandbox, l), inner - 2 * 10, fsK);
  const sbh = 10 + sbl.length * fsK * 1.35 + 6;
  const h2 = PAD + el.length * fsL * 1.3 + 4 + esl.length * fsS * 1.35 + 10 + sbh + PAD - 2;
  // data
  const dl = wrap(fill(loc(f.data, l), { app: app.id, profile: prof.id }), inner, fsK, 0.6);
  const h3 = 12 + dl.length * fsK * 1.35 + 8;
  const gap = 12;
  if (draw) {
    svg.rect(x, y, w, h1, 'ours');
    let yy = svg.text(x + PAD, y + PAD + fsL * 0.9, tl, { fs: fsL, weight: 800, color: 'var(--t-ours-fg)' });
    yy = svg.text(x + PAD, yy + 4 + fsK * 1.35, kl, { fs: fsK, weight: 600, color: 'var(--t-ours-fg)', opacity: 0.85 });
    svg.text(x + PAD, yy + 6 + fsS * 1.35, sl, { fs: fsS, color: 'var(--t-ours-fg)' });
    const y2 = y + h1 + gap;
    svg.parts.push(`<line x1="${x + w / 2}" y1="${y + h1}" x2="${x + w / 2}" y2="${y2}" style="stroke:var(--subtle)" stroke-width="2"/>`);
    svg.rect(x, y2, w, h2, 'reused');
    yy = svg.text(x + PAD, y2 + PAD + fsL * 0.9, el, { fs: fsL, weight: 800, color: 'var(--t-reused-fg)' });
    yy = svg.text(x + PAD, yy + 4 + fsS * 1.35, esl, { fs: fsS, color: 'var(--t-reused-fg)' });
    const sy = yy + 10;
    svg.rect(x + PAD - 4, sy, w - 2 * PAD + 8, sbh, 'reused', { dash: true, r: 8, sw: 1.5, fillOverride: 'var(--surface)' });
    svg.text(x + PAD + 6, sy + 10 + fsK * 0.9, sbl, { fs: fsK, weight: 600, color: 'var(--t-reused-fg)' });
    const y3 = y2 + h2 + gap;
    svg.parts.push(`<line x1="${x + w / 2}" y1="${y2 + h2}" x2="${x + w / 2}" y2="${y3}" style="stroke:var(--subtle)" stroke-width="2"/>`);
    svg.rect(x, y3, w, h3, 'user', { dash: true, r: 10, sw: 1.5 });
    svg.text(x + PAD, y3 + 12 + fsK * 0.9, dl, { fs: fsK, weight: 600, color: 'var(--t-user-fg)', mono: false });
  }
  return h1 + gap + h2 + gap + h3;
}

function layout(f, l, narrow) {
  const W = narrow ? 360 : 780, M = narrow ? 8 : 12;
  const svg = new Svg();
  let y = M;
  // lord-server
  const fsL = narrow ? 17 : 19, fs = narrow ? 12.5 : 13.5;
  const sw = W - 2 * M, inner = sw - 2 * (PAD + 4);
  const kl = wrap(loc(f.server.kicker, l), inner, fs);
  const ll = loc(f.server.lines, l).flatMap((s) => wrap(s, inner, fs));
  const sh = PAD + 4 + fsL + 6 + kl.length * fs * 1.35 + 8 + ll.length * fs * 1.4 + PAD;
  svg.rect(M, y, sw, sh, 'ours');
  let yy = svg.text(M + PAD + 4, y + PAD + 4 + fsL * 0.85, [loc(f.server.label, l)], { fs: fsL, weight: 800, color: 'var(--t-ours-fg)' });
  yy = svg.text(M + PAD + 4, yy + 6 + fs * 1.35, kl, { fs, weight: 600, color: 'var(--t-ours-fg)', opacity: 0.85 });
  svg.text(M + PAD + 4, yy + 8 + fs * 1.35, ll, { fs, lh: 1.4, color: 'var(--t-ours-fg)' });
  y += sh;
  const ah = narrow ? 70 : 56;
  svg.arrow(W / 2 - (narrow ? 120 : 0), y, y + ah, loc(f.start, l), { width: narrow ? 230 : 330, fs: narrow ? 11.5 : 12.5 });
  y += ah;

  // the apps' scopes
  const apps = f.apps;
  const colW = narrow ? sw - 2 * 12 : 228, cgap = narrow ? 18 : 12, gpad = narrow ? 12 : 12;
  const groupW = (a) => (narrow ? sw : a.profiles.length * colW + (a.profiles.length - 1) * cgap + 2 * gpad);
  const totalW = apps.reduce((s, a) => s + groupW(a), 0);
  const ggap = narrow ? 0 : (sw - totalW) / Math.max(1, apps.length - 1);
  let gx = M;
  const groups = [];
  let rowBottom = y;
  const gy0 = y;
  apps.forEach((a, ai) => {
    const gw = groupW(a);
    const gyStart = narrow ? rowBottom + (ai ? 22 : 0) : gy0;
    const head = narrow
      ? [fill(loc(f.scopeShort, l), { app: loc(a.app, l) })]
      : wrap(fill(loc(f.scope, l), { app: loc(a.app, l) }), gw - 2 * gpad, 12.5);
    const headSub = narrow ? wrap(loc(f.scopeSub, l), gw - 2 * gpad, 11.5) : [];
    const hh = 12 + head.length * 12.5 * 1.35 + headSub.length * 11.5 * 1.35 + 10;
    const heights = a.profiles.map((p) => column(svg, f, l, a, p, 0, 0, colW, false));
    const colsH = narrow ? heights.reduce((s, h) => s + h, 0) + (heights.length - 1) * cgap : Math.max(...heights);
    const gh = hh + colsH + gpad;
    groups.push({ a, gx: narrow ? M : gx, gy: gyStart, gw, gh, hh, heights, head, headSub });
    if (narrow) rowBottom = gyStart + gh + 46 + 34; else gx += gw + ggap;  // narrow: the site sits under each group
  });
  if (!narrow) {
    const gh = Math.max(...groups.map((g) => g.gh));
    groups.forEach((g) => { g.gh = gh; });
    rowBottom = gy0 + gh;
  }
  for (const g of groups) {
    svg.rect(g.gx, g.gy, g.gw, g.gh, 'kernel', { dash: true, r: 16, sw: 2, fillOverride: 'var(--surface-2)' });
    let hy = svg.text(g.gx + gpad, g.gy + 12 + 12.5 * 0.9, g.head, { fs: 12.5, weight: 800, color: 'var(--ink)' });
    if (g.headSub.length) svg.text(g.gx + gpad, hy + 11.5 * 1.35, g.headSub, { fs: 11.5, weight: 500, color: 'var(--muted)' });
    let cx = g.gx + gpad, cy = g.gy + g.hh;
    g.a.profiles.forEach((p, i) => {
      column(svg, f, l, g.a, p, cx, cy, colW, true);
      if (narrow) cy += g.heights[i] + cgap; else cx += colW + cgap;
    });
    if (narrow) {
      // the site right under the app's group
      const sy = g.gy + g.gh;
      const label = fill(loc(f.net, l), { app: loc(g.a.app, l) });
      svg.arrow(M + 40, sy, sy + 46, label, { width: 260, fs: 11.5 });
      site(svg, loc(g.a.site, l), M + 40, sy + 46, narrow);
      g.siteBottom = sy + 46 + 34;
    }
  }
  if (narrow) {
    y = groups.at(-1).siteBottom;
  } else {
    y = rowBottom;
    for (const g of groups) {
      const cx = g.gx + g.gw / 2;
      svg.arrow(cx - 90, y, y + 50, fill(loc(f.net, l), { app: loc(g.a.app, l) }), { width: 190, fs: 12 });
      site(svg, loc(g.a.site, l), cx - 90, y + 50, narrow);
    }
    y += 50 + 34;
  }
  // legend
  y += 22;
  const items = [['ours', f.legend.ours], ['reused', f.legend.reused], ['kernel', f.legend.kernel], ['user', f.legend.net]];
  let lx = M, ly = y;
  for (const [tone, txt] of items) {
    const t = loc(txt, l), w = 22 + t.length * 11.5 * 0.56 + 18;
    if (lx + w > W - M) { lx = M; ly += 22; }
    svg.rect(lx, ly - 11, 14, 14, tone, { r: 4, sw: 1.5, dash: tone === 'kernel' || tone === 'user', fillOverride: tone === 'kernel' ? 'var(--surface-2)' : undefined });
    svg.text(lx + 20, ly, [t], { fs: 11.5, weight: 600, color: 'var(--muted)' });
    lx += w;
  }
  const H = Math.ceil(ly + 12);
  return { W, H, body: svg.parts.join('') };
}

function site(svg, host, x, y, narrow) {
  const w = host.length * 12.5 * 0.62 + 40;
  svg.rect(x - 20, y, w, 30, 'user', { r: 15, sw: 1.5 });
  svg.parts.push(`<circle cx="${x - 4}" cy="${y + 15}" r="6" style="fill:none;stroke:var(--t-user-fg)" stroke-width="1.5"/><path d="M${x - 10} ${y + 15}h12M${x - 4} ${y + 9}c-3 4-3 8 0 12M${x - 4} ${y + 9}c3 4 3 8 0 12" style="fill:none;stroke:var(--t-user-fg)" stroke-width="1.2"/>`);
  svg.text(x + 8, y + 19.5, [host], { fs: 12.5, weight: 700, color: 'var(--t-user-fg)', mono: true });
}

export function archFigure(f, l) {
  const title = loc(f.title, l), cap = loc(f.caption, l);
  const one = (lay, cls, n) => {
    const tid = `arch-${f.id}-${n}-t`, did = `arch-${f.id}-${n}-d`;
    return `<div class="${cls}"><svg viewBox="0 0 ${lay.W} ${lay.H}" width="100%" role="img" aria-labelledby="${tid} ${did}" xmlns="http://www.w3.org/2000/svg"><title id="${tid}">${esc(title)}</title><desc id="${did}">${esc(cap)}</desc>${lay.body}</svg></div>`;
  };
  return `<figure class="arch-fig" id="fig-${esc(f.id)}"><p class="arch-title">${esc(title)}</p>${one(layout(f, l, false), 'arch-wide', 'w')}${one(layout(f, l, true), 'arch-narrow', 'n')}<figcaption>${esc(cap)}${f.notice ? `<small class="tm-note">${esc(loc(f.notice, l))}</small>` : ''}</figcaption></figure>`;
}

// For docs/PAPER.md: the figure as a text block.
export function archText(f) {
  return [`**Figure: ${loc(f.title, 'en')}.**`, '', '```', ...(f.text.en || []), '```', '', `*${loc(f.caption, 'en')}*`].join('\n');
}
