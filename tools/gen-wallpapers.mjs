#!/usr/bin/env node
// Builds the LordOS wallpaper collection (assets/wallpapers/) from the original PNGs.
//
// Usage: node tools/gen-wallpapers.mjs <source dir>
//   <source dir>/<category>/lordos-<name>.png              (category: signature, lumo, cosmos, cyberpunk; .jpg too)
//   <source dir>/<category>/lordos-<name>@<W>x<H>.png      optional higher-resolution variant
// Categories missing from <source dir> keep their entries in index.json (one category can be added on its own).
//
// For each original it writes, with ffmpeg (decode, Lanczos scaling) and cjpeg (libjpeg-turbo):
//   assets/wallpapers/<category>/lordos-<name>.jpg            same size and name as the original
//   assets/wallpapers/<category>/lordos-<name>@<W>x<H>.jpg    each variant, at its own size
//   assets/wallpapers/thumbs/<category>/lordos-<name>.jpg     320×180 (cover crop) for Settings
// and assets/wallpapers/index.json, which the SDK bundles (sdk/wallpapers.js) to pick, for each
// screen, the smallest variant that covers it (or the largest one). To add a 4K version later,
// drop lordos-<name>@3840x2160.png next to the original and run this again.
//
// JPEG at quality 90 with 4:4:4 chroma (the art has fine violet and gold detail that 4:2:0 blurs),
// baseline + optimized Huffman tables. stb_image (lord-server) decodes it; WebP isn't supported.
//
// An original larger than BASE (Cyberpunk City HD: 3840×2160 pictures upscaled from 1672×941) gets a base file at
// 1920×1080 (Lanczos) and is kept itself as the @<W>x<H> variant, so a 1080p screen never decodes the 4K file. Such a
// category can set its own quality in ENCODE: an upscaled picture has no detail that quality 90 or 4:4:4 would keep.
import { execFileSync } from 'node:child_process';
import fs from 'node:fs';
import os from 'node:os';
import path from 'node:path';

const root = path.resolve(path.dirname(new URL(import.meta.url).pathname), '..');
const src = process.argv[2];
if (!src || !fs.statSync(src, { throwIfNoEntry: false })?.isDirectory()) {
  console.error('usage: node tools/gen-wallpapers.mjs <source dir with signature/ lumo/ cosmos/>');
  process.exit(2);
}
const CATEGORIES = ['signature', 'lumo', 'cosmos', 'cyberpunk'];
const THUMB = [320, 180];
const BASE = [1920, 1080];
const ENCODE = {
  default: { base: [90, '1x1'], large: [90, '1x1'] },
  // upscaled from 1672×941: the base keeps 4:4:4 for the neon edges; at 4K, 4:2:0 still has more colour detail than
  // the picture ever had
  cyberpunk: { base: [85, '1x1'], large: [80, '2x2'] },
};
const out = path.join(root, 'assets', 'wallpapers');
const tmp = fs.mkdtempSync(path.join(os.tmpdir(), 'lordos-wp-'));

const run = (cmd, args) => execFileSync(cmd, args, { stdio: ['ignore', 'pipe', 'pipe'] }).toString();
function size(file) {
  const [w, h] = run('ffprobe', ['-v', 'error', '-select_streams', 'v:0', '-show_entries', 'stream=width,height',
    '-of', 'csv=p=0', file]).trim().split(',').map(Number);
  return { w, h };
}
// ffmpeg → PPM (with an optional filter) → cjpeg
function jpeg(input, output, vf, quality, sample) {
  const ppm = path.join(tmp, 'frame.ppm');
  run('ffmpeg', ['-loglevel', 'error', '-y', '-i', input, ...(vf ? ['-vf', vf] : []), '-pix_fmt', 'rgb24', ppm]);
  fs.mkdirSync(path.dirname(output), { recursive: true });
  run('cjpeg', ['-quality', String(quality), '-sample', sample, '-optimize', '-outfile', output, ppm]);
}

const IMG = /\.(png|jpe?g)$/;
const prior = (() => { try { return JSON.parse(fs.readFileSync(path.join(out, 'index.json'), 'utf8')); } catch (e) { return []; } })();
const index = [];
for (const cat of CATEGORIES) {
  const dir = path.join(src, cat);
  if (!fs.existsSync(dir)) { index.push(...prior.filter((e) => e.category === cat)); continue; }
  const enc = ENCODE[cat] || ENCODE.default;
  const pngs = fs.readdirSync(dir).filter((n) => IMG.test(n)).sort();
  for (const name of pngs.filter((n) => !n.includes('@'))) {
    const base = name.replace(IMG, '');
    if (!/^lordos-[a-z0-9-]+$/.test(base)) throw new Error(`bad wallpaper name: ${name}`);
    const id = base.replace(/^lordos-/, '');
    const entry = { id, category: cat, thumb: `thumbs/${cat}/${base}.jpg`, variants: [] };
    const variants = [name, ...pngs.filter((n) => n.startsWith(`${base}@`) && /@\d+x\d+\.(png|jpe?g)$/.test(n))];
    const add = (input, file, vf, w, h, [q, sample]) => {
      jpeg(input, path.join(out, file), vf, q, sample);
      entry.variants.push({ w, h, file });
      console.log(`${file}  ${w}×${h}  ${(fs.statSync(path.join(out, file)).size / 1024).toFixed(0)} KB`);
    };
    for (const v of variants) {
      const { w, h } = size(path.join(dir, v));
      if (v === name && (w > BASE[0] || h > BASE[1])) {
        // too big for a base: a 1920×1080 one, and the original as its own variant
        const s = Math.min(BASE[0] / w, BASE[1] / h), bw = Math.round(w * s / 2) * 2, bh = Math.round(h * s / 2) * 2;
        add(path.join(dir, v), `${cat}/${base}.jpg`, `scale=${bw}:${bh}:flags=lanczos`, bw, bh, enc.base);
        add(path.join(dir, v), `${cat}/${base}@${w}x${h}.jpg`, null, w, h, enc.large);
        continue;
      }
      add(path.join(dir, v), `${cat}/${v.replace(IMG, '.jpg')}`, null, w, h, v === name ? enc.base : enc.large);
    }
    entry.variants.sort((a, b) => a.w - b.w);
    entry.file = entry.variants[0].file;
    const [tw, th] = THUMB;
    jpeg(path.join(dir, name), path.join(out, entry.thumb),
      `scale=${tw}:${th}:force_original_aspect_ratio=increase:flags=lanczos,crop=${tw}:${th}`, 85, '2x2');
    index.push(entry);
  }
}
fs.writeFileSync(path.join(out, 'index.json'), JSON.stringify(index, null, 1) + '\n');
fs.rmSync(tmp, { recursive: true, force: true });
console.log(`${index.length} wallpapers → assets/wallpapers/index.json`);
