// reduce-cities.mjs — GeoNames' cities15000.txt (tab-separated, 19 columns) to assets/geo/cities.tsv (see
// fetch-cities.sh). Every field is checked; a line that doesn't fit is left out, never guessed. The output is sorted
// by population (largest first) so a search shows the likeliest city first, and has a header line naming the source.
import { readFileSync, writeFileSync } from 'node:fs';

const [src, dst] = process.argv.slice(2);
if (!src || !dst) { console.error('usage: reduce-cities.mjs <cities15000.txt> <cities.tsv>'); process.exit(2); }
const text = readFileSync(src, 'utf8');
const clean = (s) => String(s).replace(/[\t\r\n\u0000-\u001f\u007f]/g, ' ').trim();
const out = [];
for (const line of text.split('\n')) {
  const f = line.split('\t');
  if (f.length < 19) continue;
  const id = Number(f[0]), lat = Number(f[4]), lon = Number(f[5]), pop = Number(f[14]);
  const name = clean(f[1]), ascii = clean(f[2]), cc = f[8], adm1 = clean(f[10]);
  if (!Number.isInteger(id) || id <= 0) continue;
  if (!name || name.length > 80 || ascii.length > 80) continue;
  if (!/^[A-Z]{2}$/.test(cc)) continue;
  if (!(lat >= -90 && lat <= 90 && lon >= -180 && lon <= 180)) continue;
  if (!/^[A-Za-z0-9]{0,20}$/.test(adm1)) continue;
  out.push({ id, name, ascii: ascii || name, cc, adm1, lat: Math.round(lat * 100) / 100, lon: Math.round(lon * 100) / 100, pop: Number.isFinite(pop) && pop > 0 ? pop : 0 });
}
if (out.length < 10000) { console.error(`reduce-cities: only ${out.length} cities; refusing to replace the list`); process.exit(1); }
out.sort((a, b) => b.pop - a.pop || a.id - b.id);
const head = '# GeoNames cities15000 (https://www.geonames.org), CC BY 4.0 — id\tname\tascii (empty: same as name)\tcountry\tadmin1\tlat\tlon; largest first';
writeFileSync(dst, [head, ...out.map((c) => [c.id, c.name, c.ascii === c.name ? '' : c.ascii, c.cc, c.adm1, String(c.lat), String(c.lon)].join('\t'))].join('\n') + '\n');
