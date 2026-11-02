#!/usr/bin/env node
// Builds the LordOS wallpaper collection (assets/wallpapers/) from the original PNGs.
//
// Usage: node tools/gen-wallpapers.mjs <source dir>
//   <source dir>/<category>/lordos-<name>.png              (category: horizons, cosmos)
//   <source dir>/<category>/lordos-<name>@<W>x<H>.png      optional higher-resolution variant
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
// baseline + optimized Huffman tables. stb_image (axia-server) decodes it; WebP isn't supported.
import { execFileSync } from 'node:child_process';
import fs from 'node:fs';
import os from 'node:os';
import path from 'node:path';

const root = path.resolve(path.dirname(new URL(import.meta.url).pathname), '..');
const src = process.argv[2];
if (!src || !fs.statSync(src, { throwIfNoEntry: false })?.isDirectory()) {
  console.error('usage: node tools/gen-wallpapers.mjs <source dir with horizons/ cosmos/>');
  process.exit(2);
}
const CATEGORIES = ['horizons', 'cosmos'];
const THUMB = [320, 180];
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

const index = [];
for (const cat of CATEGORIES) {
  const dir = path.join(src, cat);
  if (!fs.existsSync(dir)) continue;
  const pngs = fs.readdirSync(dir).filter((n) => n.endsWith('.png')).sort();
  for (const name of pngs.filter((n) => !n.includes('@'))) {
    const base = name.replace(/\.png$/, '');
    if (!/^lordos-[a-z0-9-]+$/.test(base)) throw new Error(`bad wallpaper name: ${name}`);
    const id = base.replace(/^lordos-/, '');
    const entry = { id, category: cat, thumb: `thumbs/${cat}/${base}.jpg`, variants: [] };
    const variants = [name, ...pngs.filter((n) => n.startsWith(`${base}@`) && /@\d+x\d+\.png$/.test(n))];
    for (const v of variants) {
      const file = `${cat}/${v.replace(/\.png$/, '.jpg')}`;
      jpeg(path.join(dir, v), path.join(out, file), null, 90, '1x1');
      const { w, h } = size(path.join(dir, v));
      entry.variants.push({ w, h, file });
      console.log(`${file}  ${w}×${h}  ${(fs.statSync(path.join(out, file)).size / 1024).toFixed(0)} KB`);
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
