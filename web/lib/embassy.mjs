// The /language/ page's picture: Lumo waving in front of an embassy with flagpoles. Our own drawing, built from
// plain shapes: the building follows the site's colours (CSS classes in site.css, light and dark), the flags are
// simple stripes and discs (decoration only: the page names each language by its own name, never by a flag).
// lumo is the URL of the hashed mascot SVG (assets/brand/mascot/lumo-wave.svg).

const W = 56, H = 36;   // one flag
const stripesH = (cs) => cs.map((c, i) => `<rect x="0" y="${(H / cs.length) * i}" width="${W}" height="${H / cs.length + 0.5}" fill="${c}"/>`).join('');
const stripesV = (cs) => cs.map((c, i) => `<rect x="${(W / cs.length) * i}" y="0" width="${W / cs.length + 0.5}" height="${H}" fill="${c}"/>`).join('');
const FLAGS = [
  `<rect width="${W}" height="${H}" fill="#fff"/><circle cx="28" cy="18" r="10" fill="#BC002D"/>`,                                    // disc
  `<rect width="${W}" height="${H}" fill="#009C3B"/><path d="M28 4 52 18 28 32 4 18z" fill="#FFDF00"/><circle cx="28" cy="18" r="7.5" fill="#002776"/>`,   // rhombus
  stripesH(['#000', '#DD0000', '#FFCE00']),
  stripesH(['#FDB913', '#006A44', '#C1272D']),
  stripesV(['#0055A4', '#fff', '#EF4135']),
  stripesH(['#0057B7', '#FFD700']),
];
const EDGE = '<path class="lg-edge" d="M0 2Q14-2 28 2T56 2V36Q42 40 28 36T0 36Z"/>';   // a hairline, so a white flag shows on a light page
const BRAND = `<rect width="${W}" height="${H}" fill="#5B3FE0"/><path d="M17 24 19 13 24 18 28 10 32 18 37 13 39 24z" fill="#fff"/>`;   // a small crown

// a pole at x with its top at y and a flag; the flag waves (CSS, off with reduced motion) inside a wavy clip
const pole = (x, y, flag, i) => `<g class="lg-pole"><rect x="${x - 2}" y="${y}" width="4" height="${360 - y}" rx="2"/><circle cx="${x}" cy="${y - 3}" r="4.5"/></g>
<g transform="translate(${x + 2} ${y + 2})"><g class="lg-flag lg-f${i}" clip-path="url(#lg-wave)">${flag}${EDGE}</g></g>`;

export function embassy(lumo, alt) {
  const left = [[40, 96], [88, 116], [136, 136]], right = [[478, 136], [522, 116], [566, 96]];
  const poles = [...left, ...right].map(([x, y], i) => pole(x, y, FLAGS[i], i)).join('\n');
  const cols = [212, 252, 370, 410].map((x) => `<rect class="lg-col" x="${x}" y="182" width="18" height="124"/>`).join('');
  return `<svg class="lg-svg" viewBox="0 0 640 400" role="img" aria-label="${alt}">
<defs><clipPath id="lg-wave" clipPathUnits="userSpaceOnUse"><path d="M0 2Q14-2 28 2T56 2V36Q42 40 28 36T0 36Z"/></clipPath></defs>
<circle class="lg-sun" cx="320" cy="230" r="196"/>
<ellipse class="lg-ground" cx="320" cy="368" rx="300" ry="22"/>
<g class="lg-bld">
<rect class="lg-step" x="166" y="330" width="308" height="16" rx="3"/>
<rect class="lg-step" x="178" y="317" width="284" height="15" rx="3"/>
<rect class="lg-step" x="190" y="305" width="260" height="14" rx="3"/>
<rect class="lg-wall" x="200" y="178" width="240" height="128"/>
<path class="lg-door" d="M296 306V246a24 24 0 0 1 48 0v60z"/>
<rect class="lg-win" x="276" y="200" width="16" height="30" rx="8"/><rect class="lg-win" x="348" y="200" width="16" height="30" rx="8"/>
${cols}
<rect class="lg-roof" x="188" y="164" width="264" height="18" rx="3"/>
<path class="lg-roof" d="M182 166 320 106 458 166z"/>
<g class="lg-globe"><circle cx="320" cy="143" r="14"/><path d="M306 143h28M320 129a20 20 0 0 1 0 28M320 129a20 20 0 0 0 0 28"/></g>
</g>
<g class="lg-pole"><rect x="318" y="58" width="4" height="50" rx="2"/><circle cx="320" cy="55" r="4.5"/></g>
<g transform="translate(322 60)"><g class="lg-flag lg-f6" clip-path="url(#lg-wave)">${BRAND}</g></g>
${poles}
<image href="${lumo}" x="230" y="196" width="180" height="180"/>
</svg>`;
}
