// studio_catalog_test.mjs — LordOS Studio's background catalog (assets/studio/backgrounds.json, docs/CAMERA-STUDIO-PLAN.md
// 6.2) against its loops' curation record (tools/studio/bg-sources.json) and, when it is on this machine, the folder
// published as the LordOS-wallpapers repository's studio/. Every entry is one studio-bg-get accepts (E-S4: names, files,
// a whole size of at most 100 MB, a SHA-256) and every license is an allowed one with a credit where it needs one. Every
// loop has a 3840×2160 variant made from an original at least that big, a 1920×1080 one and a 640×360 thumbnail; every
// picture is AI-generated (CC0, "ai": true) with one variant, "full", and a 640×360 thumbnail, and the built-in ones are
// that variant byte for byte. With LORDOS_STUDIO_STAGE (the publish folder, tools/studio/publish-studio.py's --out) it
// also checks every published file's size, hash and real resolution (JPEG frame header; MP4 avc1 sample entry, H.264
// High), MANIFEST.json and README.md, and that nothing else is there. Run from server/.
import assert from 'node:assert/strict';
import { createHash } from 'node:crypto';
import { existsSync, readFileSync, statSync, readdirSync } from 'node:fs';
import { join, dirname } from 'node:path';
import { fileURLToPath } from 'node:url';
import { BG_CATEGORIES, bgVariant } from '../../sdk/studiomodel.js';

const ROOT = join(dirname(fileURLToPath(import.meta.url)), '..', '..');
const cat = JSON.parse(readFileSync(join(ROOT, 'assets/studio/backgrounds.json'), 'utf8'));
const src = JSON.parse(readFileSync(join(ROOT, 'tools/studio/bg-sources.json'), 'utf8'));
const ALLOWED = new Set(['CC0', 'Public domain', 'CC BY 2.0', 'CC BY 3.0', 'CC BY 4.0', 'CC BY-SA 2.0', 'CC BY-SA 3.0', 'CC BY-SA 4.0', 'MIT']);
const MAX_FILE = 100 * 1000 * 1000;
const SIZES = { video: [['2160.h264', 3840, 2160], ['1080.h264', 1920, 1080]] };
const AI_AUTHOR = 'Generated with ChatGPT for LordOS';
const sha256 = (buf) => createHash('sha256').update(buf).digest('hex');

// a JPEG's size from its first frame header (SOF0..SOF15 but DHT, JPG and DAC)
export function jpegSize(b) {
  if (b[0] !== 0xff || b[1] !== 0xd8) return null;
  let i = 2;
  while (i + 9 < b.length) {
    if (b[i] !== 0xff) return null;
    const m = b[i + 1];
    if (m === 0xd8 || (m >= 0xd0 && m <= 0xd7) || m === 0x01) { i += 2; continue; }
    const len = b.readUInt16BE(i + 2);
    if (m >= 0xc0 && m <= 0xcf && m !== 0xc4 && m !== 0xc8 && m !== 0xcc) return { w: b.readUInt16BE(i + 7), h: b.readUInt16BE(i + 5) };
    i += 2 + len;
  }
  return null;
}

// an MP4's first video sample entry: its codec, size and (for avc1) the profile in avcC
export function mp4Video(b) {
  const boxes = (s, e) => {
    const out = [];
    for (let i = s; i + 8 <= e;) {
      let n = b.readUInt32BE(i);
      const t = b.toString('latin1', i + 4, i + 8);
      let h = 8;
      if (n === 1) { n = Number(b.readBigUInt64BE(i + 8)); h = 16; }
      if (n === 0) n = e - i;
      if (n < h || i + n > e) break;
      out.push({ t, s: i + h, e: i + n });
      i += n;
    }
    return out;
  };
  const find = (list, t) => list.find((x) => x.t === t);
  const moov = find(boxes(0, b.length), 'moov');
  if (!moov) return null;
  for (const trak of boxes(moov.s, moov.e).filter((x) => x.t === 'trak')) {
    const mdia = find(boxes(trak.s, trak.e), 'mdia');
    const minf = mdia && find(boxes(mdia.s, mdia.e), 'minf');
    const stbl = minf && find(boxes(minf.s, minf.e), 'stbl');
    const stsd = stbl && find(boxes(stbl.s, stbl.e), 'stsd');
    if (!stsd) continue;
    const entries = boxes(stsd.s + 8, stsd.e);
    const v = entries.find((x) => ['avc1', 'avc3', 'hvc1', 'hev1', 'av01'].includes(x.t));
    if (!v) continue;
    const w = b.readUInt16BE(v.s + 24), h = b.readUInt16BE(v.s + 26);
    const avcC = find(boxes(v.s + 78, v.e), 'avcC');
    return { codec: v.t, w, h, profile: avcC ? b[avcC.s + 1] : null };
  }
  return null;
}

let checks = 0;
const ok = (c, msg) => { checks++; assert.ok(c, msg); };

// ---- the catalog
ok(cat.format === 1 && /^https:\/\/raw\.githubusercontent\.com\/lordbasex\/[^?#]+$/.test(cat.base), 'the catalog base');
const ids = new Set();
const counts = { picture: 0, video: 0 };
const perCat = {};
for (const it of cat.items) {
  const id = it.id;
  ok(/^[a-z0-9-]{1,48}$/.test(id) && !ids.has(id), `${id}: a bad or repeated id`);
  ids.add(id);
  ok(it.kind === 'picture' || it.kind === 'video', `${id}: kind`);
  ok(BG_CATEGORIES.includes(it.category), `${id}: category ${it.category}`);
  ok(ALLOWED.has(it.license), `${id}: license "${it.license}" isn't allowed`);
  ok(!/unsplash|pexels|pixabay/i.test(it.source || '') && /^https:\/\//.test(it.source || ''), `${id}: source ${it.source}`);
  ok(typeof it.author === 'string' && it.author.length > 0, `${id}: no author`);
  ok(typeof it.name === 'string' && it.name.length > 0, `${id}: no name`);
  if (/^CC BY/.test(it.license)) ok(typeof it.credit === 'string' && it.credit.includes(it.license), `${id}: CC BY needs a credit line naming its license`);
  // a loop: 3840×2160 and 1920×1080; a picture: AI-generated, CC0, one variant "full" at the generator's 1672×941 (or 940)
  const want = SIZES[it.kind] || (it.variants || []).map((v) => [v.name, v.w, v.h]);
  if (it.kind === 'picture') {
    ok(it.ai === true && it.license === 'CC0' && it.author === AI_AUTHOR && !it.credit, `${id}: a picture is AI-generated and CC0`);
    ok(it.variants.length === 1 && it.variants[0].name === 'full' && it.variants[0].w === 1672 && [940, 941].includes(it.variants[0].h),
      `${id}: the picture's variant`);
  } else {
    ok(it.ai === undefined, `${id}: a loop isn't AI-generated`);
  }
  ok(Array.isArray(it.variants) && it.variants.length === want.length, `${id}: ${want.length} variants`);
  want.forEach(([name, w, h], k) => {
    const v = it.variants[k];
    ok(v.name === name && v.w === w && v.h === h, `${id}: variant ${k} is ${v.name} ${v.w}x${v.h}, not ${name} ${w}x${h}`);
    ok(/^[a-z0-9.-]{1,48}$/.test(v.name), `${id}: variant name`);
    ok(v.file === (it.kind === 'picture' ? `pictures/${id}.jpg` : `videos/${id}.${name}.mp4`), `${id}: file ${v.file}`);
    ok(Number.isInteger(v.size) && v.size > 1000 && v.size <= MAX_FILE, `${id}: ${name} size ${v.size} (E-S4: at most 100 MB)`);
    ok(/^[0-9a-f]{64}$/.test(v.sha256), `${id}: ${name} hash`);
    if (it.kind === 'video') ok(v.codec === 'h264' && v.fps > 0 && v.fps <= 30.01 && v.seconds >= 4 && v.seconds <= 10, `${id}: ${name} codec/fps/length`);
  });
  if (it.kind === 'video') ok(it.variants.some((v) => v.w >= 3840 && v.h >= 2160), `${id}: no 4K variant`);
  // the thumbnail ships: a 640×360 JPEG
  const tf = join(ROOT, 'assets/studio/backgrounds', `${id}.thumb.jpg`);
  ok(existsSync(tf), `${id}: thumbnail missing`);
  const tb = readFileSync(tf);
  const ts = jpegSize(tb);
  ok(ts && ts.w === 640 && ts.h === 360 && tb.length > 1000 && tb.length < 160000, `${id}: thumbnail ${ts && `${ts.w}x${ts.h}`} ${tb.length} bytes`);
  if (it.builtin) {
    // the package ships the variant the CPU path takes (bgVariant), which bgSrc reads from assets/
    const b = readFileSync(join(ROOT, 'assets/studio/backgrounds', `${id}.jpg`));
    const v = bgVariant(it);
    ok(it.kind === 'picture' && v && b.length === v.size && sha256(b) === v.sha256, `${id}: the built-in picture isn't its ${v && v.name} variant`);
    perCat[it.category] = (perCat[it.category] || 0) + 1;
  }
  counts[it.kind]++;
}
ok(BG_CATEGORIES.every((c) => perCat[c] === 1), `one built-in picture per category: ${JSON.stringify(perCat)}`);
// nothing else in the shipped folder
for (const f of readdirSync(join(ROOT, 'assets/studio/backgrounds'))) {
  const m = /^([a-z0-9-]+)(\.thumb)?\.jpg$/.exec(f);
  ok(m && ids.has(m[1]) && (m[2] || cat.items.find((x) => x.id === m[1]).builtin), `assets/studio/backgrounds/${f} isn't in the catalog`);
}

// ---- the curation record: the same loops (the pictures are build-ai-backgrounds.py's), each one's original at least
// 3840×2160 (never upscaled)
const rec = new Map(src.items.map((x) => [x.id, x]));
const loops = cat.items.filter((x) => x.kind === 'video');
ok(src.items.every((x) => x.kind === 'video'), 'bg-sources.json records only loops');
ok(rec.size === loops.length && loops.every((x) => rec.has(x.id)), 'bg-sources.json and the catalog list the same loops');
for (const it of loops) {
  const r = rec.get(it.id);
  for (const k of ['kind', 'category', 'name', 'author', 'license', 'licenseUrl', 'source']) ok(r[k] === it[k], `${it.id}: ${k} differs from bg-sources.json`);
  const o = r.origin, p = r.process, og = o.original;
  ok(typeof o.verifiedLicense === 'string' && o.verifiedLicense.length > 0, `${it.id}: no license check recorded`);
  ok(og && og.w > 0 && og.h > 0, `${it.id}: the original's resolution isn't recorded`);
  if (o.type === 'polyhaven') {
    // a perspective view needs at least one panorama pixel per output pixel at its centre
    const minFov = (2 * Math.atan(Math.PI * 3840 / og.w)) * 180 / Math.PI;
    ok(og.w >= 16384 && og.h * 2 === og.w && p.fov >= minFov - 1e-9, `${it.id}: a ${og.w}-wide panorama can't make 3840 at FOV ${p.fov}`);
  } else if (o.type === 'nasa') {
    ok(og.w * p.crop.w >= 3840 && og.w * p.crop.w * 9 / 16 >= 2160, `${it.id}: the crop is under 3840x2160`);
  } else if (o.type === 'ambientcg') {
    ok(Math.min(og.w, og.h * 16 / 9) >= 3840, `${it.id}: the map is under 3840 wide`);
  } else if (o.type === 'commons') {
    const [cw, ch] = p.crop ? [p.crop[2], p.crop[3]] : [og.w, og.h];
    ok(cw >= 3840 && ch >= 2160, `${it.id}: the clip is ${cw}x${ch}`);
    ok(/^https:\/\/commons\.wikimedia\.org\/wiki\/File:/.test(it.source), `${it.id}: a Commons file page`);
  } else {
    ok(o.type === 'procedural' && it.license === 'MIT' && it.author === 'LordOS' && og.w >= 3840 && og.h >= 2160, `${it.id}: origin ${o.type}`);
  }
}

// ---- the publish folder (LORDOS_STUDIO_STAGE: the LordOS-wallpapers repository's studio/, publish-studio.py's --out)
const stage = process.env.LORDOS_STUDIO_STAGE;
let staged = 0, bytes = 0;
if (stage && existsSync(stage)) {
  const listed = new Map(JSON.parse(readFileSync(join(stage, 'MANIFEST.json'), 'utf8')).files.map((x) => [x.file, x]));
  const readme = readFileSync(join(stage, 'README.md'), 'utf8');
  ok(/CC0 1\.0/.test(readme) && /AI-generated/.test(readme), 'README.md: the pictures\' license');
  const files = new Set();
  for (const it of cat.items) {
    if (it.kind === 'video') ok(readme.includes(it.source) && readme.includes(it.credit || it.author) && readme.includes(it.license), `${it.id}: README.md doesn't credit it`);
    for (const v of it.variants) {
      const f = join(stage, v.file);
      ok(existsSync(f), `${v.file}: not published`);
      ok(readme.includes(it.kind === 'picture' ? v.file : v.file.slice('videos/'.length)), `${v.file}: not in README.md`);
      const b = readFileSync(f);
      ok(b.length === v.size && sha256(b) === v.sha256, `${v.file}: size or hash isn't the catalog's`);
      const m = listed.get(v.file);
      ok(m && m.id === it.id && m.size === v.size && m.sha256 === v.sha256, `${v.file}: MANIFEST.json differs`);
      if (it.kind === 'picture') {
        const s = jpegSize(b);
        ok(s && s.w === v.w && s.h === v.h, `${v.file}: ${s && `${s.w}x${s.h}`} isn't ${v.w}x${v.h}`);
      } else {
        const s = mp4Video(b);
        ok(s && s.codec === 'avc1' && s.w === v.w && s.h === v.h && s.profile === 100, `${v.file}: ${JSON.stringify(s)}`);
      }
      files.add(v.file);
      staged++;
      bytes += b.length;
    }
  }
  const top = readdirSync(stage).filter((x) => !x.startsWith('.')).sort();
  ok(JSON.stringify(top) === JSON.stringify(['MANIFEST.json', 'README.md', 'pictures', 'videos']), `the publish folder holds ${top}`);
  for (const d of ['pictures', 'videos']) for (const f of readdirSync(join(stage, d))) ok(files.has(`${d}/${f}`), `${d}/${f} is published but not in the catalog`);
  ok(listed.size === files.size, 'MANIFEST.json lists other files');
}

console.log(`studio catalog: ${cat.items.length} backgrounds (${counts.picture} pictures, ${counts.video} loops, every loop with a 3840x2160 ` +
  `variant from a big enough original); ${checks} checks` + (stage ? `; ${staged} published files (${(bytes / 1e6).toFixed(1)} MB) match` : '; publish folder: skipped: needs LORDOS_STUDIO_STAGE'));
