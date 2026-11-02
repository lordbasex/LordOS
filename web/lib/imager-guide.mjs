// imager-guide.mjs — the "How to use LordOS Imager" guide (/imager/, /es/imager/, /pt/imager/) and the
// "How to write LordOS" block of the download section. Content: web/content/imager/guide.<lang>.json;
// screenshots: web/content/imager/shots/{light,dark}/<lang>/*.webp (1600 px wide, the desktop app's
// real interface with demo values); the video slot: web/content/imager/video/ (see videoSlot()).
import fs from 'node:fs';
import path from 'node:path';
import { inline } from './md.mjs';

// The video turns on by itself when its files are there (one English narration, three subtitle
// tracks). Until then the page shows the poster with "Video coming soon".
export const VIDEO_BASE = 'lordos_imager_guide';
export const VIDEO_LANGS = ['en', 'es', 'pt'];

export function imagerGuide({ content, repo, lang: l, t, g, version, esc, webp, hashed, read, lumo, pageUrl, prefix, links, videoDir }) {
  const md = (s) => inline(String(s || ''));
  const fill = (s) => String(s).replace('{v}', version);
  const shotDir = (theme) => path.join(content, 'shots', theme, l);

  // one screenshot in both themes, in the app's window frame (the page shows the one that matches it)
  const shotCache = new Map();
  function variant(file, key) {
    if (!shotCache.has(file)) {
      const thumb = webp(file, 800, 80, key);
      const full = { url: hashed(read(file), key, 'webp') };
      shotCache.set(file, { thumb, full });
    }
    return shotCache.get(file);
  }
  function shot(name, caption, { sizes = '(max-width: 900px) 92vw, 760px', eager = false, cls = '' } = {}) {
    const imgs = ['light', 'dark'].map((theme) => {
      const file = path.join(shotDir(theme), name + '.webp');
      if (!fs.existsSync(file)) return '';
      const { thumb, full } = variant(file, `imager-${theme}-${l}-${name}`);
      const load = eager && theme === 'light' ? ' fetchpriority="high"' : ' loading="lazy" decoding="async"';
      return `<a class="g-card ig-${theme === 'light' ? 'l' : 'd'}" href="${full.url}" data-w="1600" data-h="${thumb.h * 2}" data-cap="${esc(caption)}" aria-label="${esc(g.open + ': ' + caption)}"><img src="${thumb.url}" srcset="${thumb.url} ${thumb.w}w, ${full.url} 1600w" sizes="${sizes}" width="${thumb.w}" height="${thumb.h}" alt="${esc(caption)}"${load}></a>`;
    }).join('');
    return `<figure class="ig-win${cls ? ' ' + cls : ''}"><div class="ig-bar" aria-hidden="true"><span>LordOS Imager</span></div>${imgs}<figcaption>${esc(caption)}</figcaption></figure>`;
  }
  // a screenshot from docs/ (the board's own screens), one theme
  function docShot(file, caption) {
    const f = path.join(repo, file);
    if (!fs.existsSync(f)) return '';
    const thumb = webp(f, 800, 80), full = webp(f, 1600, 80);
    return `<figure class="ig-screen"><a class="g-card" href="${full.url}" data-w="${full.w}" data-h="${full.h}" data-cap="${esc(caption)}" aria-label="${esc(g.open + ': ' + caption)}"><img src="${thumb.url}" srcset="${thumb.url} ${thumb.w}w, ${full.url} ${full.w}w" sizes="(max-width: 900px) 92vw, 380px" width="${thumb.w}" height="${thumb.h}" alt="${esc(caption)}" loading="lazy" decoding="async"></a><figcaption>${esc(caption)}</figcaption></figure>`;
  }
  const tip = (x) => (x ? `<aside class="ig-tip" aria-label="${esc(g.tipFrom)}"><img src="${lumo[x[0]] || lumo.wave}" alt="" width="64" height="64" loading="lazy"><div><p class="ig-tip-t">${esc(x[1])}</p><p>${md(x[2])}</p></div></aside>` : '');
  const num = (i) => String(i + 1).padStart(2, '0');

  const dl = `<div class="ig-dl">
<a class="btn btn-primary" href="${links.mac}">${esc(t.imgMac)}</a>
<a class="btn btn-primary" href="${links.win}">${esc(t.imgWin)}</a>
<a class="btn btn-primary" href="${links.linux}">${esc(t.imgLinux)}</a>
</div>`;

  // ---- hero ----
  const hero = `<section class="ig-hero" aria-labelledby="ig-h">
<div class="wrap ig-hero-in">
<div class="ig-hero-txt">
<p class="kicker">${esc(g.kicker)}</p>
<h1 id="ig-h">${esc(g.title)}</h1>
<p class="ig-lead">${esc(g.lead)}</p>
${dl}
<p class="ig-ver">${esc(fill(g.version))}<br>${esc(g.alsoIn)}</p>
</div>
<div class="ig-hero-art">
${shot('01-device', g.heroAlt, { eager: true, sizes: '(max-width: 900px) 92vw, 600px', cls: 'ig-tilt' })}
<div class="ig-hello"><img src="${lumo.wave}" alt="" width="132" height="132"><p class="ig-bubble">${esc(g.hello)}</p></div>
</div>
</div>
</section>`;

  const need = `<section class="wrap ig-need" aria-labelledby="ig-need-h">
<h2 id="ig-need-h" class="ig-h2s">${esc(g.needTitle)}</h2>
<ul>${g.need.map(([a, b], i) => `<li><span class="ig-need-i" aria-hidden="true">${NEED_ICONS[i] || ''}</span><strong>${esc(a)}</strong><span>${md(b)}</span></li>`).join('')}</ul>
</section>`;

  // ---- video: the mockup until the files exist ----
  const video = videoSlot({ l, g, esc, hashed, read, videoDir, lumo });

  // ---- steps ----
  const toc = `<nav class="ig-toc" aria-label="${esc(g.tocLabel)}"><p>${esc(g.tocLabel)}</p><ol>${g.steps.map((s, i) => `<li><a href="#step-${s.id}"><span>${num(i)}</span>${esc(s.title)}</a></li>`).join('')}<li><a href="#cooling"><span>★</span>${esc(g.coolTitle)}</a></li><li><a href="#safety"><span>★</span>${esc(g.secTitle)}</a></li><li><a href="#help"><span>?</span>${esc(g.faqTitle)}</a></li></ol></nav>`;

  const steps = g.steps.map((s, i) => {
    const points = s.points ? `<ul class="ig-points">${s.points.map((p) => `<li>${md(p)}</li>`).join('')}</ul>` : '';
    let media = '';
    if (s.subs) {
      media = `<ol class="ig-subs">${s.subs.map(([file, title, text], k) => `<li id="tab-${file.replace(/^\d+-(tab-)?/, '')}">${shot(file, title, { sizes: '(max-width: 700px) 92vw, 380px' })}<div><h4><span class="ig-chip">${k + 1}</span>${esc(title)}</h4><p>${md(text)}</p></div></li>`).join('')}</ol>`;
    } else if (s.boot) {
      media = `<div class="ig-pair">${s.boot.map(([f, c]) => docShot(f, c)).join('')}</div>`;
    } else if (s.shots && s.shots.length > 1) {
      const [first, second, ...rest] = s.shots;
      media = `<div class="ig-pair">${shot(first[0], first[1], { sizes: '(max-width: 700px) 92vw, 380px' })}${shot(second[0], second[1], { sizes: '(max-width: 700px) 92vw, 380px' })}</div>`
        + (rest.length ? `<div class="ig-trio">${rest.map(([f, c]) => shot(f, c, { sizes: '(max-width: 700px) 92vw, 250px' })).join('')}</div>` : '');
    } else if (s.shots) {
      media = shot(s.shots[0][0], s.shots[0][1]);
    }
    return `<section class="ig-step" id="step-${s.id}" aria-labelledby="step-${s.id}-h">
<div class="ig-num" aria-hidden="true">${num(i)}</div>
<div class="ig-step-body">
<p class="ig-step-k">${esc(g.stepWord)} ${i + 1}</p>
<h2 id="step-${s.id}-h">${esc(s.title)}</h2>
<p class="ig-text">${md(s.text)}</p>
${points}
${media}
${tip(s.tip)}
</div>
</section>`;
  }).join('\n');

  // ---- cooling chooser ----
  const cooling = `<section class="ig-cool" id="cooling" aria-labelledby="cooling-h">
<h2 id="cooling-h" class="ig-h2s">${esc(g.coolTitle)}</h2>
<p class="ig-sub">${esc(g.coolLead)}</p>
<ul class="ig-cool-cards">${g.cool.map(([id, title, text, pick]) => `<li class="ig-cc ig-cc-${id}"><span class="ig-cc-art" aria-hidden="true">${COOL_ART[id] || ''}</span><h3>${esc(title)}</h3><p>${md(text)}</p><p class="ig-pick">${md(pick)}</p></li>`).join('')}</ul>
<h3 class="ig-h3">${esc(g.profTitle)}</h3>
<div class="ig-table" role="region" aria-labelledby="cooling-h" tabindex="0"><table><thead><tr>${g.profHead.map((h) => `<th scope="col">${esc(h)}</th>`).join('')}</tr></thead><tbody>${g.prof.map(([a, b, c]) => `<tr><th scope="row">${esc(a)}</th><td>${md(b)}</td><td>${md(c)}</td></tr>`).join('')}</tbody></table></div>
<div class="ig-note"><p class="ig-note-t">${esc(g.ocTitle)}</p><p>${md(g.ocText)}</p></div>
</section>`;

  const options = `<section class="ig-opt" aria-labelledby="opt-h">
<div><h2 id="opt-h" class="ig-h2s">${esc(g.optTitle)}</h2><p>${md(g.optText)}</p></div>
${shot(g.optShot[0], g.optShot[1], { sizes: '(max-width: 900px) 92vw, 520px' })}
</section>`;

  const safety = `<section class="ig-safe" id="safety" aria-labelledby="safety-h">
<div class="wrap">
<img class="ig-safe-lumo" src="${lumo.shield}" alt="" width="120" height="120" loading="lazy">
<h2 id="safety-h" class="ig-h2s">${esc(g.secTitle)}</h2>
<p class="ig-sub">${esc(g.secLead)}</p>
<ul class="ig-safe-grid">${g.sec.map(([a, b]) => `<li><svg width="22" height="22" viewBox="0 0 24 24" fill="none" stroke="currentColor" stroke-width="2" stroke-linecap="round" stroke-linejoin="round" aria-hidden="true"><path d="M12 3l8 4v5c0 5-3.5 8-8 9-4.5-1-8-4-8-9V7z"/><path d="M8.5 12l2.5 2.5 4.5-5"/></svg><h3>${esc(a)}</h3><p>${md(b)}</p></li>`).join('')}</ul>
<p class="ig-safe-link"><a href="${links.signing}">${esc(g.secLink)} →</a></p>
</div>
</section>`;

  const faq = `<section class="wrap ig-faq" id="help" aria-labelledby="help-h">
<h2 id="help-h" class="ig-h2s">${esc(g.faqTitle)}</h2>
<div class="ig-faq-list">${g.faq.map(([q, a], i) => `<details id="help-${i + 1}"><summary>${esc(q)}</summary><p>${md(a)}</p></details>`).join('')}</div>
</section>`;

  const end = `<section class="wrap ig-end" aria-labelledby="end-h">
<div class="ig-end-card">
<img src="${lumo.celebrate}" alt="" width="150" height="150" loading="lazy">
<div>
<h2 id="end-h" class="ig-h2s">${esc(g.endTitle)}</h2>
<p>${esc(g.endText)}</p>
${dl}
<p class="ig-end-other"><a href="${prefix(l)}#write">${esc(g.endOther)}</a></p>
</div>
</div>
<p class="ig-shots-note">${esc(fill(g.shotsNote))}</p>
</section>`;

  const main = `${hero}
${need}
${video}
<div class="wrap ig-guide">
${toc}
<div class="ig-steps">
${steps}
${cooling}
${options}
</div>
</div>
${safety}
${faq}
${end}`;
  return { main, meta: g.meta };
}


// The video slot. With web/content/imager/video/lordos_imager_guide.mp4 (and .webm, and the subtitles
// lordos_imager_guide.<en|es|pt>.vtt) the real player is built; without them, the mockup.
// IMAGER_VIDEO_DIR points somewhere else when the video is kept out of git.
function videoSlot({ l, g, esc, hashed, read, videoDir, lumo }) {
  const f = (ext) => path.join(videoDir, `${VIDEO_BASE}.${ext}`);
  const posterFile = path.join(videoDir, 'poster.webp');
  const poster = fs.existsSync(posterFile) ? hashed(read(posterFile), 'imager-video-poster', 'webp') : '';
  const ready = fs.existsSync(f('mp4'));
  const head = `<div class="wrap ig-video-head"><h2 id="vid-h" class="ig-h2s">${esc(g.videoTitle)}</h2><p class="ig-sub">${esc(g.videoLead)}</p></div>`;
  if (ready) {
    const mp4 = hashed(read(f('mp4')), VIDEO_BASE, 'mp4');
    const webm = fs.existsSync(f('webm')) ? hashed(read(f('webm')), VIDEO_BASE, 'webm') : '';
    const names = { en: 'English', es: 'Español', pt: 'Português' };
    const tracks = VIDEO_LANGS.filter((x) => fs.existsSync(f(`${x}.vtt`))).map((x) => {
      const vtt = hashed(read(f(`${x}.vtt`)), `${VIDEO_BASE}.${x}`, 'vtt');
      // the narration is English: the Spanish and Portuguese pages show their own subtitles by default
      return `<track kind="subtitles" srclang="${x}" label="${names[x]}" src="${vtt}"${x === l && l !== 'en' ? ' default' : ''}>`;
    }).join('');
    return `<section class="ig-video" aria-labelledby="vid-h">${head}
<div class="wrap"><figure class="ig-vframe"><video controls preload="none" playsinline${poster ? ` poster="${poster}"` : ''} width="1920" height="1080">${webm ? `<source src="${webm}" type="video/webm">` : ''}<source src="${mp4}" type="video/mp4">${tracks}<p>${esc(g.videoFallback)} <a href="${mp4}">MP4</a></p></video>
<figcaption>${esc(g.videoCredit)}</figcaption></figure></div>
</section>`;
  }
  return `<section class="ig-video" aria-labelledby="vid-h">${head}
<div class="wrap"><figure class="ig-vframe ig-soon">${poster ? `<img src="${poster}" alt="" width="1280" height="720" loading="lazy">` : ''}
<figcaption class="ig-soon-in"><span class="ig-play" aria-hidden="true"><svg width="34" height="34" viewBox="0 0 24 24" fill="currentColor"><path d="M8 5.5v13a1 1 0 001.5.86l10-6.5a1 1 0 000-1.72l-10-6.5A1 1 0 008 5.5z"/></svg></span><span class="ig-pill">${esc(g.videoSoon)}</span><span>${esc(g.videoSoonText)}</span></figcaption></figure></div>
</section>`;
}

// small line icons (Tabler-style strokes) for "What you need"
const svg = (d) => `<svg width="24" height="24" viewBox="0 0 24 24" fill="none" stroke="currentColor" stroke-width="2" stroke-linecap="round" stroke-linejoin="round">${d}</svg>`;
const NEED_ICONS = [
  svg('<rect x="3" y="4" width="18" height="12" rx="2"/><path d="M8 20h8M12 16v4"/>'),
  svg('<path d="M7 3h7l4 4v14H7z"/><path d="M10 7v3M13 7v3"/>'),
  svg('<rect x="3" y="6" width="18" height="12" rx="2"/><rect x="7" y="9" width="5" height="5" rx="1"/><path d="M15 10h3M15 13h3"/>'),
  svg('<circle cx="12" cy="13" r="8"/><path d="M12 9v4l2 2M9 2h6"/>'),
];
// the four coolers, drawn simply (no product shapes)
const fan = (cx, cy, r) => `<circle cx="${cx}" cy="${cy}" r="${r}" fill="none" stroke="currentColor" stroke-width="2"/><path d="M${cx} ${cy}c0-${r * 0.7} ${r * 0.6}-${r * 0.7} ${r * 0.6}-${r * 0.2}S${cx} ${cy} ${cx} ${cy}zM${cx} ${cy}c${r * 0.6} ${r * 0.35} ${r * 0.3} ${r * 0.9}-${r * 0.1} ${r * 0.7}S${cx} ${cy} ${cx} ${cy}zM${cx} ${cy}c-${r * 0.6} ${r * 0.35}-${r * 0.75}-${r * 0.25}-${r * 0.45}-${r * 0.5}S${cx} ${cy} ${cx} ${cy}z" fill="currentColor" opacity=".55"/>`;
const COOL_ART = {
  'case-fan': `<svg viewBox="0 0 96 64" width="96" height="64"><rect x="6" y="8" width="84" height="48" rx="9" fill="currentColor" opacity=".14" stroke="currentColor" stroke-width="2"/>${fan(48, 32, 15)}</svg>`,
  'active-cooler': `<svg viewBox="0 0 96 64" width="96" height="64"><rect x="10" y="40" width="76" height="16" rx="3" fill="#1E9E55" opacity=".35"/>${[20, 30, 40, 56, 66, 76].map((x) => `<rect x="${x - 3}" y="12" width="6" height="30" rx="2" fill="currentColor" opacity=".35"/>`).join('')}${fan(48, 27, 13)}</svg>`,
  passive: `<svg viewBox="0 0 96 64" width="96" height="64"><rect x="6" y="8" width="84" height="48" rx="9" fill="currentColor" opacity=".14" stroke="currentColor" stroke-width="2"/>${[22, 32, 42, 52, 62, 72].map((x) => `<rect x="${x}" y="18" width="5" height="28" rx="2" fill="currentColor" opacity=".45"/>`).join('')}</svg>`,
  none: `<svg viewBox="0 0 96 64" width="96" height="64"><rect x="8" y="12" width="80" height="44" rx="5" fill="#1E9E55" opacity=".4" stroke="#1E9E55" stroke-width="2"/><rect x="38" y="26" width="16" height="16" rx="2" fill="currentColor" opacity=".55"/><rect x="62" y="18" width="18" height="10" rx="2" fill="currentColor" opacity=".3"/><rect x="62" y="32" width="18" height="10" rx="2" fill="currentColor" opacity=".3"/></svg>`,
};

// ---- the "How to write LordOS to a USB stick or SD card" block of the download section ----
export function writeBlock({ l, t, g, esc, links, version, isoVersion = version, guideUrl }) {
  const h = g.home;
  const md = (s) => inline(String(s || ''));
  const iso = `lordos-live-${isoVersion}-amd64.iso`, pi = `lordos-${version}-rpi5-arm64.img.xz`;
  const cmd = (id, label, text) => `<div class="cmd"><div class="cmd-bar"><span class="cmd-t" aria-hidden="true"><i></i><i></i><i></i>${esc(label)}</span><button type="button" class="copy" data-copy="${id}" data-done="${esc(t.copied)}" aria-label="${esc(t.copy)}: ${esc(label)}" hidden>${esc(t.copy)}</button></div><pre id="${id}" tabindex="0">${esc(text)}</pre></div>`;
  return `<section class="dl-write" id="write" aria-labelledby="wr-h">
<div class="center"><h3 id="wr-h" class="dl-write-h">${esc(h.title)}</h3><p class="sub">${esc(h.lead)}</p></div>
<div class="dl-write-grid">
<article class="dl-card dl-imager dl-rec" aria-labelledby="img-h">
<p class="dl-badge">${esc(h.badge)}</p>
<h3 id="img-h">${esc(t.imgTitle)}</h3>
<p>${esc(t.imgText)}</p>
<ol class="dl-steps">${h.steps.map((s) => `<li>${esc(s)}</li>`).join('')}</ol>
<div class="dl-btns">
<a href="${links.mac}" class="btn btn-violet">${esc(t.imgMac)}</a>
<a href="${links.win}" class="btn btn-violet">${esc(t.imgWin)}</a>
<a href="${links.linux}" class="btn btn-violet">${esc(t.imgLinux)}</a>
</div>
<p class="dl-guide"><a href="${guideUrl}">${esc(h.guide)} →</a></p>
<p class="dl-hint">${esc(t.imgNote)}</p>
</article>
<article class="dl-card dl-own" aria-labelledby="own-h">
<h3 id="own-h">${esc(h.optTitle)}</h3>
<p>${md(h.optText)}</p>
${cmd('wr-pc', h.optPc, `sha256sum -c --ignore-missing SHA256SUMS\nsudo dd if=${iso} of=/dev/sdX bs=4M status=progress conv=fsync`)}
${cmd('wr-pi', h.optPi, `sha256sum -c --ignore-missing SHA256SUMS\nxz -dc ${pi} | sudo dd of=/dev/sdX bs=4M status=progress conv=fsync`)}
<p class="dl-hint"><a href="${links.signing}">${esc(h.optVerify)}</a></p>
<p class="dl-hint">${md(h.optPiSetup)} <a href="${links.switchFiles}">${esc(h.optPiLink)}</a></p>
</article>
</div>
</section>`;
}
